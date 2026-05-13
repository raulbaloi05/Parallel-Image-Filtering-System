/**
 * Programul de mai jos implementeaza serverul pe socket UNIX al sistemului PIF,
 * utilizat exclusiv pentru comunicarea cu panoul de administrare (admin.c).
 * Ruleaza ca thread secundar in cadrul procesului principal al serverului.
 *
 * Functioneaza prin rutarea cererilor primite pe socket-ul /tmp/unixds:
 * -- REQUEST_CLIENTS              : trimite lista clientilor activi;
 * -- REQUEST_FILTERS              : trimite statisticile de utilizare ale filtrelor;
 * -- REQUEST_SYS_INFO             : trimite CPU%, RAM si uptime-ul serverului;
 * -- REQUEST_SERVER_CONFIGURATION : trimite configuratia curenta (status, max clienti);
 * -- SEND_SERVER_CONFIGURATION    : primeste si aplica o noua configuratie de la admin;
 * -- KILL_CLIENT                  : trimite SIGKILL proceselor copil ale unui client;
 * -- CHANGE_CONECTIONS            : comuta starea serverului intre OPEN si CLOSED;
 * -- REQUEST_LOGS                 : trimite istoricul de evenimente al serverului.
 *
 * Informatiile de sistem (CPU, RAM) sunt citite din /proc/stat si /proc/meminfo.
 * Starea globala (global_state) este accesata prin extern din server.c si
 * protejata de acelasi state_mutex pentru a preveni conflictele intre threaduri.
 *
 * Am tratat urmatoarele situatii limita:
 * -- erori la crearea/bind/listen pe socket-ul UNIX;
 * -- clientul admin se deconecteaza neasteptat (recv returneaza 0).
 */
#include <stdio.h>         /* Utilizat pentru: fopen(), fclose(), fgets(), fscanf(), printf(), perror(), FILE */
#include <stdlib.h>        /* Utilizat pentru: (rezervat pentru extensii viitoare) */
#include <string.h>        /* Utilizat pentru: memset(), strcmp(), memcpy(), strncpy() */
#include <unistd.h>        /* Utilizat pentru: close() */
#include <time.h>          /* Utilizat pentru: time(), time_t */
#include <sys/socket.h>    /* Utilizat pentru: socket(), bind(), listen(), accept(), send(), recv(), setsockopt(), AF_UNIX, SOCK_STREAM, SOL_SOCKET, SO_RCVTIMEO */
#include <sys/un.h>        /* Utilizat pentru: struct sockaddr_un */
#include <sys/time.h>      /* Utilizat pentru: struct timeval */
#include <signal.h>        /* Utilizat pentru: kill(), SIGKILL */
#include <pthread.h>       /* Utilizat pentru: pthread_mutex_lock(), pthread_mutex_unlock(), pthread_mutex_t */
#include "dataTypes.h"     /* Utilizat pentru: Header, ServerState, ClientInfo, Filter, ServerConfiguration, SysInfo, LogEntry, toKill, constante REQUEST_x/SEND_x, FILTERNR, PROCESS_COUNT, STATUS_LEN, UPTIME_LEN, LINE_BUF_LEN */

#define SECS_PER_DAY  86400 /* Secunde intr-o zi (60 * 60 * 24) */
#define SECS_PER_HOUR 3600  /* Secunde intr-o ora (60 * 60) */
#define SECS_PER_MIN  60    /* Secunde intr-un minut */
#define KB_PER_MB     1024  /* Kilobytes intr-un megabyte */
#define LINE_BUF_LEN  256   /* Lungimea bufferului pentru citirea liniilor din /proc/meminfo */
#define ADMIN_TIMEOUT 60    /* Timeout in secunde pentru deconectare admin inactiv */
#define MAX_BANNED    100   /* Numarul maxim de IP-uri banate */

/* Lista IP-urilor banate */
typedef struct {
    char ips[MAX_BANNED][IP_LEN];
    int count;
} BanList;

static BanList banned_ips = {.count = 0};
static pthread_mutex_t ban_mutex = PTHREAD_MUTEX_INITIALIZER;

/*
 * verifica daca un IP este in lista de banati
 * functie non-static pentru a putea fi accesata din server.c
 */
int is_ip_banned(const char *ip) {
    pthread_mutex_lock(&ban_mutex);
    for (int i = 0; i < banned_ips.count; i++) {
        if (strcmp(banned_ips.ips[i], ip) == 0) {
            pthread_mutex_unlock(&ban_mutex);
            return 1;
        }
    }
    pthread_mutex_unlock(&ban_mutex);
    return 0;
}

/*
 * adauga un IP in lista de banati
 */
static void ban_ip(const char *ip) {
    pthread_mutex_lock(&ban_mutex);
    if (banned_ips.count < MAX_BANNED) {
        int already_banned = 0;
        for (int i = 0; i < banned_ips.count; i++) {
            if (strcmp(banned_ips.ips[i], ip) == 0) {
                already_banned = 1;
                break;
            }
        }
        if (!already_banned) {
            strncpy(banned_ips.ips[banned_ips.count], ip, IP_LEN - 1);
            banned_ips.ips[banned_ips.count][IP_LEN - 1] = '\0';
            banned_ips.count++;
        }
    }
    pthread_mutex_unlock(&ban_mutex);
}

/*
 * calculeaza utilizarea generala a procesorului
 * citeste /proc/stat la apeluri succesive si face diferenta
 */
static int get_cpu_usage() {
    static long long prev_total = 0, prev_idle = 0;
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return 0;
    
    long long user, nice, system, idle, iowait, irq, softirq;
    // extragere timpi pentru fiecare stare a CPU-ului
    fscanf(f, "cpu %lld %lld %lld %lld %lld %lld %lld",
           &user, &nice, &system, &idle, &iowait, &irq, &softirq);
    fclose(f);
    
    long long total = user + nice + system + idle + iowait + irq + softirq;
    long long idle_total = idle + iowait;
    
    // calcul diferenta fata de masuratoarea anterioara
    long long dtotal = total - prev_total;
    long long didle = idle_total - prev_idle;
    
    prev_total = total;
    prev_idle = idle_total;
    
    if (dtotal == 0) return 0;
    // calcul procentaj de utilizare activa
    return (int)((dtotal - didle) * 100 / dtotal);
}

/*
 * extrage memoria RAM totala si cea folosita a sistemului (in MB)
 * citeste din /proc/meminfo
 */
static void get_ram_usage(long *used_mb, long *total_mb) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) { *used_mb = 0; *total_mb = 0; return; }
    
    long mem_total = 0, mem_available = 0;
    char line[LINE_BUF_LEN];

    // cautare MemTotal si MemAvailable linie cu linie
    while (fgets(line, sizeof(line), f)) {
        long val;
        if (sscanf(line, "MemTotal: %ld kB", &val) == 1) mem_total = val;
        else if (sscanf(line, "MemAvailable: %ld kB", &val) == 1) { mem_available = val; break; }
    }
    fclose(f);

    // convertire din kB in MB
    *total_mb = mem_total / KB_PER_MB;
    *used_mb = (mem_total - mem_available) / KB_PER_MB;
}

// preluam starea globala si mutex-ul din server.c
#ifndef UNIX_SERVER_STANDALONE
extern pthread_mutex_t state_mutex;
extern ServerState global_state;
#endif

/*
 * ruteaza cererile venite pe socket-ul UNIX (de la dashboard / monitor)
 * blocheaza state_mutex doar pe sectiunile critice la citire/scriere
 * implementeaza timeout pentru deconectarea adminilor inactivi
 */
void handle_request(int client_fd) {
    Header req_header;
    struct timeval timeout;
    timeout.tv_sec = ADMIN_TIMEOUT;
    timeout.tv_usec = 0;
    
    // setare timeout pe socket pentru recv
    if (setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        perror("setsockopt failed");
        return;
    }
    
    // asteapta comenzi atat timp cat clientul este conectat
    while(1) {
        int recv_result = recv(client_fd, &req_header, sizeof(Header), 0);
        
        if (recv_result == 0) {
            // clientul s-a deconectat
            printf("[UNIX Thread] Admin disconnected\n");
            break;
        } else if (recv_result < 0) {
            // timeout - trimitem notificare si apoi deconectam
            printf("[UNIX Thread] Admin timeout - sending disconnect notification\n");
            Header timeout_notif = {ADMIN_TIMEOUT_DISCONNECT, sizeof(Header), 0};
            send(client_fd, &timeout_notif, sizeof(Header), 0);
            break;
        }
        
        switch(req_header.type) {
            case REQUEST_CLIENTS: {
                // trimite datele tuturor clientilor activi
                pthread_mutex_lock(&state_mutex);
                Header resp = {SEND_CLIENTS, sizeof(Header) + global_state.active_clients_count * sizeof(ClientInfo), global_state.active_clients_count};
                send(client_fd, &resp, sizeof(Header), 0);
                for (int i = 0; i < global_state.active_clients_count; i++)
                    send(client_fd, &global_state.clients[i], sizeof(ClientInfo), 0);
                pthread_mutex_unlock(&state_mutex);
                break;
            }
            case REQUEST_FILTERS: {
                // trimite statistici despre utilizarea filtrelor
                pthread_mutex_lock(&state_mutex);
                Header resp = {SEND_FILTERS, sizeof(Header) + FILTERNR * sizeof(Filter), FILTERNR};
                send(client_fd, &resp, sizeof(Header), 0);
                for (int i = 0; i < FILTERNR; i++)
                    send(client_fd, &global_state.filters[i], sizeof(Filter), 0);
                pthread_mutex_unlock(&state_mutex);
                break;
            }
            case REQUEST_SYS_INFO: {
                // trimite date sistem (CPU, RAM si uptime)
                pthread_mutex_lock(&state_mutex);
                SysInfo info;
                info.CPU_usage = get_cpu_usage();
                get_ram_usage(&info.RAM_usage, &info.RAM_max);
                
                time_t now = time(NULL);
                int diff = now - global_state.start_time;
                sprintf(info.uptime, "Server is UP for %dd %dh %dmin %dsec",
                        diff/SECS_PER_DAY, (diff%SECS_PER_DAY)/SECS_PER_HOUR,
                        (diff%SECS_PER_HOUR)/SECS_PER_MIN, diff%SECS_PER_MIN);
                
                Header resp = {SEND_SYS_INFO, sizeof(SysInfo), 1};
                send(client_fd, &resp, sizeof(Header), 0);
                send(client_fd, &info, sizeof(SysInfo), 0);
                pthread_mutex_unlock(&state_mutex);
                break;
            }
            case REQUEST_SERVER_CONFIGURATION: {
                // trimite configuratiile serverului
                pthread_mutex_lock(&state_mutex);
                Header resp = {SEND_SERVER_CONFIGURATION, sizeof(ServerConfiguration), 1};
                send(client_fd, &resp, sizeof(Header), 0);
                send(client_fd, &global_state.config, sizeof(ServerConfiguration), 0);
                pthread_mutex_unlock(&state_mutex);
                break;
            }
            case SEND_SERVER_CONFIGURATION: {
                // primeste o noua configuratie de la admin si suprascrie pe cea curenta
                ServerConfiguration config;
                recv(client_fd, &config, sizeof(ServerConfiguration), 0);
                
                pthread_mutex_lock(&state_mutex);
                global_state.config = config; 
                pthread_mutex_unlock(&state_mutex);
                break;
            }
            case KILL_CLIENT: {
                // SIGKILL pentru procesele copil ale unui anumit client
                toKill tk;
                recv(client_fd, &tk, sizeof(toKill), 0);
                for (int i = 0; i < PROCESS_COUNT; i++) {
                    if (tk.childrens[i] > 0) kill(tk.childrens[i], SIGKILL);
                }
                
                // sterge clientul fortat din array-ul global
                pthread_mutex_lock(&state_mutex);
                for (int i = 0; i < global_state.active_clients_count; i++) {
                    if (global_state.clients[i].job_id == tk.job_id) {
                        for (int j = i; j < global_state.active_clients_count - 1; j++)
                            global_state.clients[j] = global_state.clients[j + 1];
                        global_state.active_clients_count--;
                        break;
                    }
                }
                pthread_mutex_unlock(&state_mutex);
                break;
            }
            case CHANGE_CONECTIONS: {
                // toggle starea serverului (permite/blocheaza conexiuni noi)
                pthread_mutex_lock(&state_mutex);
                if (strcmp(global_state.config.status, "OPEN") == 0)
                    memcpy(global_state.config.status, "CLOSED", sizeof("CLOSED"));
                else
                    memcpy(global_state.config.status, "OPEN", sizeof("OPEN"));
                pthread_mutex_unlock(&state_mutex);
                break;
            }
            case REQUEST_LOGS: {
                // trimite intregul buffer de log-uri spre interfata de admin
                pthread_mutex_lock(&state_mutex);
                int n = global_state.log_count;
                Header resp = {SEND_LOGS, sizeof(Header) + n * sizeof(LogEntry), n};
                send(client_fd, &resp, sizeof(Header), 0);
                for (int i = 0; i < n; i++)
                    send(client_fd, &global_state.logs[i], sizeof(LogEntry), 0);
                pthread_mutex_unlock(&state_mutex);
                break;
            }
            case BAN_CLIENT: {
                // SIGKILL pentru procesele copil ale unui anumit client
                toKill tk;
                recv(client_fd, &tk, sizeof(toKill), 0);
                for (int i = 0; i < PROCESS_COUNT; i++) {
                    if (tk.childrens[i] > 0) kill(tk.childrens[i], SIGKILL);
                }

                // adaugare IP in lista de banati
                pthread_mutex_lock(&state_mutex);
                char client_ip[IP_LEN] = {0};
                for (int i = 0; i < global_state.active_clients_count; i++) {
                    if (global_state.clients[i].job_id == tk.job_id) {
                        strncpy(client_ip, global_state.clients[i].ip, IP_LEN - 1);
                        break;
                    }
                }
                pthread_mutex_unlock(&state_mutex);
                
                if (client_ip[0] != '\0') {
                    ban_ip(client_ip);
                    printf("[UNIX Thread] Client IP %s has been banned\n", client_ip);
                }
                
                // sterge clientul fortat din array-ul global
                pthread_mutex_lock(&state_mutex);
                for (int i = 0; i < global_state.active_clients_count; i++) {
                    if (global_state.clients[i].job_id == tk.job_id) {
                        for (int j = i; j < global_state.active_clients_count - 1; j++)
                            global_state.clients[j] = global_state.clients[j + 1];
                        global_state.active_clients_count--;
                        break;
                    }
                }
                pthread_mutex_unlock(&state_mutex);
                break;
            }
        }
    }
}

/*
 * functia executata de thread-ul separat de UNIX socket
 * asculta comunicarea locala inter-proces
 */
void* unix_main(void* arg) {
    const char* socket_path = (const char*)arg;
    int server_fd, client_fd;
    struct sockaddr_un serveraddr;
    
    // creare file descriptor pentru socket
    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("Unix socket creation failed");
        return NULL;
    }
    
    // setare structura adresa (familie UNIX si cale spre fisier)
    memset(&serveraddr, 0, sizeof(struct sockaddr_un));
    serveraddr.sun_family = AF_UNIX;
    strncpy(serveraddr.sun_path, socket_path, sizeof(serveraddr.sun_path) - 1);
    
    // asocieri fisier - socket
    if (bind(server_fd, (struct sockaddr *)&serveraddr, sizeof(serveraddr)) < 0) {
        perror("Unix socket bind failed");
        close(server_fd);
        return NULL;
    }
    
    // ascultare apeluri de conexiune
    if (listen(server_fd, 1) < 0) {
        perror("Unix socket listen failed");
        close(server_fd);
        return NULL;
    }

    printf("[UNIX Thread] Unix Socket Server running on %s\n", socket_path);
    
    // bucla infinita de preluare clienti (dashboard local)
    while(1) {
        client_fd = accept(server_fd, NULL, NULL);
        if(client_fd > 0) {
            handle_request(client_fd);
            close(client_fd); // curatare
        }
    }
    
    close(server_fd);
    return NULL;
}