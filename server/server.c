/**
 * Programul de mai jos implementeaza serverul principal al sistemului PIF
 * (Parallel Image Filtering). Acesta expune 5 endpoint-uri prin protocolul
 * SOAP/HTTP si permite clientilor sa aplice filtre pe imagini in paralel.
 *
 * Endpoint-uri expuse:
 * -- ns__connect      : inregistreaza un client nou, returneaza un ID unic;
 * -- ns__echo         : returneaza mesajul primit (folosit ca ping/healthcheck);
 * -- ns__applyFilter  : primeste o imagine + filtru, returneaza imaginea procesata;
 * -- ns__bye          : deconecteaza un client si il elimina din lista activa;
 * -- ns__serverInfo   : returneaza statistici despre server (clienti, status).
 *
 * Arhitectura:
 * -- Threadul principal ruleaza bucla SOAP (soap_accept -> soap_serve);
 * -- Un thread secundar (unix_main din unix_server.c) ruleaza socket-ul UNIX
 *    pentru comunicarea cu panoul de administrare (admin.c);
 * -- Starea globala (global_state) este partajata intre cele doua threaduri
 *    si protejata de state_mutex pentru a preveni accesul concurent.
 *
 * Am tratat urmatoarele situatii limita:
 * -- serverul refuza conexiuni noi daca este CLOSED sau a atins limita de clienti;
 * -- procesarea imaginii esueaza: se returneaza fault SOAP catre client;
 * -- clientul trimite date invalide (imagine goala, filtru lipsa): fault SOAP.
 */
#define _POSIX_C_SOURCE 200809L /* Expune extensiile POSIX.1-2008: gettimeofday(), pthread_* etc. */

#include "soapH.h"              /* Utilizat pentru: struct soap, soap_init(), soap_bind(), soap_accept(), soap_serve(), soap_destroy(), soap_end(), soap_done(), soap_print_fault(), soap_receiver_fault(), soap_malloc(), soap_strdup(), SOAP_OK */
#include "ns.nsmap"             /* Utilizat pentru: namespace-urile SOAP generate de gSOAP (necesar pentru soap_serve) */
#include "processing.h"        /* Utilizat pentru: process_image() */
#include "dataTypes.h"         /* Utilizat pentru: ServerState, ClientInfo, ProcessInfo, Filter, LogEntry, SysInfo, ServerConfiguration, constante de tip mesaj, MAX_LOGS, FILTERNR, IP_LEN, STATUS_LEN, NAME_LEN, PROCESS_COUNT */
#include <GraphicsMagick/magick/api.h> /* Utilizat pentru: InitializeMagick(), DestroyMagick() */
#include <sys/time.h>          /* Utilizat pentru: gettimeofday(), struct timeval */
#include <pthread.h>           /* Utilizat pentru: pthread_t, pthread_mutex_t, pthread_mutex_lock(), pthread_mutex_unlock(), pthread_create(), pthread_join(), PTHREAD_MUTEX_INITIALIZER */
#include <stdlib.h>            /* Utilizat pentru: rand(), free(), setenv() */
#include <stdio.h>             /* Utilizat pentru: printf(), fprintf(), stderr */
#include <string.h>            /* Utilizat pentru: strcmp(), memcpy(), strncpy(), snprintf() */
#include <time.h>              /* Utilizat pentru: time(), localtime(), strftime(), time_t, struct tm */
#include <stdarg.h>            /* Utilizat pentru: va_list, va_start(), va_end(), vsnprintf() */
#include <arpa/inet.h>

#define MAX_JOB_ID          10000 /* Valoarea maxima a unui ID de sesiune generat aleator */
#define DEFAULT_MAX_CLIENTS 10    /* Numarul maxim implicit de clienti simultani la pornire */
#define SOAP_PORT           18082 /* Portul TCP pe care asculta serverul SOAP */
#define SOAP_BACKLOG        100   /* Dimensiunea cozii de conexiuni in asteptare (soap_bind) */
#define TIMESTAMP_LEN       32    /* Lungimea bufferului pentru timestamp-ul din log ([HH:MM:SS] + text) */
#define HALF_DIV            2     /* Impartitor pentru jumatate (folosit in calcule de layout) */

// stare globala protejata de mutex (inlocuieste memoria partajata din versiunile vechi)
pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
ServerState global_state;

// declaratie functie din unix_server.c pentru verificare IP-uri banate
extern int is_ip_banned(const char *ip);

/*
 * adauga un mesaj in log-ul serverului
 * foloseste mutex pentru a evita probleme la acces concurent
 */
static void log_entry(const char *fmt, ...) {
    pthread_mutex_lock(&state_mutex);
    if (global_state.log_count < MAX_LOGS) {
        // obtinere timp curent
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        char ts[TIMESTAMP_LEN];
        strftime(ts, sizeof(ts), "%H:%M:%S", t);
        
        // formatare si salvare mesaj
        LogEntry *e = &global_state.logs[global_state.log_count++];
        int off = snprintf(e->message, sizeof(e->message), "[%s] ", ts);
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(e->message + off, sizeof(e->message) - off, fmt, ap);
        va_end(ap);
    }
    pthread_mutex_unlock(&state_mutex);
}

// utilitar pentru a obtine timpul in ms
long long current_timestamp() {
    struct timeval te;
    gettimeofday(&te, NULL);
    return te.tv_sec * 1000LL + te.tv_usec / 1000;
}

/*
 * Endpoint pentru conectare client:
 * - verifica daca serverul este deschis si daca are loc
 * - genereaza un ID unic pentru noul client
 * - initializeaza info despre procesele clientului
 */
int ns__connect(struct soap *soap, struct _ns__connect *req, struct _ns__connectResponse *resp) {
    (void)req;
    
    // extrage IP-ul real al clientului din socket
    char client_ip[IP_LEN] = "unknown";

    if (soap->ip) {
        struct in_addr addr;
        addr.s_addr = htonl(soap->ip);
        inet_ntop(AF_INET, &addr, client_ip, IP_LEN);
    } else if (strlen(soap->host) > 0) {
        snprintf(client_ip, IP_LEN, "%.*s", IP_LEN - 1, soap->host);
}
    
    // verificare daca IP-ul este banat
    if (is_ip_banned(client_ip)) {
        return soap_receiver_fault(soap, "Access Denied", "Your IP has been banned by the administrator.");
    }
    
    pthread_mutex_lock(&state_mutex);
    // respingere conexiune daca e inchis sau plin
    if (strcmp(global_state.config.status, "CLOSED") == 0 || 
        global_state.active_clients_count >= (int)global_state.config.max_clients_number) {
        pthread_mutex_unlock(&state_mutex);
        return soap_receiver_fault(soap, "Server Refused", "Server is full or closed by Admin.");
    }
    
    int id = rand() % MAX_JOB_ID + 1;
    
    // adaugare client in vectorul global
    ClientInfo *c = &global_state.clients[global_state.active_clients_count++];
    c->job_id = id;
    snprintf(c->ip, IP_LEN, "%.*s", IP_LEN - 1, client_ip);
    
    // setare stadiu initial procese la IDLE si 0
    for(int i = 0; i < PROCESS_COUNT; i++) {
        c->P[i].pid = 0;
        c->P[i].cpu = 0;
        c->P[i].ram = 0;
        memcpy(c->P[i].status, "IDLE", sizeof("IDLE"));
    }
    
    pthread_mutex_unlock(&state_mutex);

    printf("[Server] New client connected. Assigned ID: %d from IP: %s\n", id, client_ip);
    log_entry("Client connected. ID=%d IP=%s", id, client_ip);
    
    resp->connect = (int *)soap_malloc(soap, sizeof(int));
    if (resp->connect) *resp->connect = id;
    return SOAP_OK;
}

// echo endpoint
int ns__echo(struct soap *soap, struct _ns__echo *req, struct _ns__echoResponse *resp) {
    resp->echo = soap_strdup(soap, req->echoRequest ? req->echoRequest : "");
    return SOAP_OK;
}

/*
 * Endpoint principal de procesare:
 * - verifica cererea si datele trimise
 * - actualizeaza statistici filtre
 * - apeleaza process_image() pentru a modifica imaginea
 * - calculeaza timpul total de procesare si trimite raspunsul
 */
int ns__applyFilter(struct soap *soap, struct _ns__applyFilter *req, struct ns__applyFilterResponse *resp) {
    if (!req || !req->filterType || !req->imageData.__ptr) {
        return soap_receiver_fault(soap, "Bad Request", "Missing filter or image data");
    }
    
    int client_id = (req->clientId) ? *req->clientId : -1;

    pthread_mutex_lock(&state_mutex);
    // incrementare contor pentru filtrul folosit
    for (int i = 0; i < FILTERNR; i++) {
        if (strcmp(global_state.filters[i].name, req->filterType) == 0) {
            global_state.filters[i].uses++;
            break;
        }
    }
    
    // cautare client in lista pentru a-i trimite array-ul de procese in functia de baza
    ProcessInfo *client_procs = NULL;
    for (int i = 0; i < global_state.active_clients_count; i++) {
        if (global_state.clients[i].job_id == client_id) {
            client_procs = global_state.clients[i].P;
            break;
        }
    }
    pthread_mutex_unlock(&state_mutex);

    long long start_time = current_timestamp();
    unsigned char *out_blob = NULL;
    size_t out_size = 0;
    
    // aici are loc prelucrarea efectiva a imaginii folosind functia din processing.c
    int res = process_image(req->imageData.__ptr, req->imageData.__size, &out_blob, &out_size, req->filterType, client_procs);
    
    if (res == 0 && out_blob != NULL) {
        // copiere imagine rezultata in structura de raspuns SOAP
        resp->imageData.__ptr = (unsigned char*)soap_malloc(soap, out_size);
        memcpy(resp->imageData.__ptr, out_blob, out_size);
        resp->imageData.__size = out_size;
        
        free(out_blob);
        
        // calculare si trimitere durata executie
        resp->processingTime = (int)(current_timestamp() - start_time);
        printf("[Server] Successfully processed in %d ms for client %d\n", resp->processingTime, client_id);
        log_entry("Filter '%s' applied for client %d in %d ms", req->filterType, client_id, resp->processingTime);
        return SOAP_OK;
    } else {
        // fail
        printf("[Server] Processing failed for client %d!\n", client_id);
        log_entry("Filter '%s' FAILED for client %d", req->filterType, client_id);
        return soap_receiver_fault(soap, "Image processing failed", "GraphicsMagick error");
    }
}

// endpoint pentru deconectare client
int ns__bye(struct soap *soap, struct _ns__bye *req, struct _ns__byeResponse *resp) {
    int id = (req->byeRequest != NULL) ? req->byeRequest->id : -1;
    
    pthread_mutex_lock(&state_mutex);
    // scoatere client din lista si shiftare la stanga
    for (int i = 0; i < global_state.active_clients_count; i++) {
        if (global_state.clients[i].job_id == id) {
            for (int j = i; j < global_state.active_clients_count - 1; j++) {
                global_state.clients[j] = global_state.clients[j + 1];
            }
            global_state.active_clients_count--;
            break;
        }
    }
    pthread_mutex_unlock(&state_mutex);

    printf("[Server] Client %d disconnected.\n", id);
    log_entry("Client disconnected. ID=%d", id);
    
    resp->status = (int *)soap_malloc(soap, sizeof(int));
    if (resp->status) *resp->status = 1;
    return SOAP_OK;
}

// endpoint pentru informatii de status (folosit pentru monitorizare / dashboard)
int ns__serverInfo(struct soap *soap, struct _ns__serverInfo *req, struct ns__serverInfoResponse *resp) {
    (void)req;
    
    pthread_mutex_lock(&state_mutex);
    int active = global_state.active_clients_count;
    char status[STATUS_LEN];
    memcpy(status, global_state.config.status, STATUS_LEN);
    pthread_mutex_unlock(&state_mutex);

    // pregatire date statistice cerute de admin/monitor
    resp->clients = active;
    resp->activeJobs = active;
    resp->uptime = soap_strdup(soap, status);
    resp->memory = soap_strdup(soap, "Stable");
    resp->queueSize = 0; // momentan nu folosim o coada propriu-zisa
    
    return SOAP_OK;
}

/*
 * Thread pentru a rula serverul SOAP
 * asculta pe portul dat in loop si asteapta conexiuni
 */
void* soap_main(void* arg) {
    int port = *(int*)arg;
    struct soap soap;
    soap_init(&soap);
    soap.bind_flags = SO_REUSEADDR;

    printf("[SOAP Thread] Starting on port %d...\n", port);

    if (soap_bind(&soap, NULL, port, SOAP_BACKLOG) < 0) {
        soap_print_fault(&soap, stderr);
        return NULL;
    }

    while (1) {
        if (soap_accept(&soap) < 0) {
            soap_print_fault(&soap, stderr);
            break;
        }
        
        // dezactivam keepalive pentru a nu intra intr-o bucla gresita in soap_serve
        soap.keep_alive = 0;
        soap.max_keep_alive = 1;

        if (soap_serve(&soap) != SOAP_OK) {
            soap_print_fault(&soap, stderr);
        }
        
        // cleanup resurse
        soap_destroy(&soap); 
        soap_end(&soap);     
    }

    soap_done(&soap);
    return NULL;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    
    // limitare resurse OMP sa nu interfereze
    setenv("OMP_NUM_THREADS", "1", 1);
    
    // initializare context GraphicsMagick
    InitializeMagick(NULL);

    // setare valori default in array global la pornire
    pthread_mutex_lock(&state_mutex);
    global_state.active_clients_count = 0;
    global_state.log_count = 0;
    global_state.start_time = time(NULL);
    global_state.config.max_clients_number = DEFAULT_MAX_CLIENTS;
    memcpy(global_state.config.status, "OPEN", sizeof("OPEN"));

    const char* f_names[] = {"grayscale", "blur", "negative", "edge", "sharpen"};
    for(int i = 0; i < FILTERNR; i++) {
        strncpy(global_state.filters[i].name, f_names[i], NAME_LEN - 1);
        global_state.filters[i].name[NAME_LEN - 1] = '\0';
        global_state.filters[i].uses = 0;
    }
    pthread_mutex_unlock(&state_mutex);

    pthread_t unixthr, tcpthr;
    int sport = SOAP_PORT;
    int tport = 18083;

    // resetam socketul UNIX in caz ca exista deja pe disk
    unlink(UNIXSOCKET);

    // pornire thread aditional pentru conexiuni pe UNIX socket (comunicare inter-proces locala)
    pthread_create(&unixthr, NULL, unix_main, (void*)UNIXSOCKET);

    // pornire thread TCP raw pentru clientul REMOTE binar (transfer fisiere)
    pthread_create(&tcpthr, NULL, tcp_main, &tport);

    printf("Server started with UNIX socket, SOAP and TCP threads\n");

    // rulam serverul SOAP pe thread-ul principal
    soap_main(&sport);

    pthread_join(unixthr, NULL);
    pthread_join(tcpthr, NULL);

    DestroyMagick();
    return 0;
}