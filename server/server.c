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

// stare globala protejata de mutex
pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
ServerState global_state;

// declaratie functie din unix_server.c pentru verificare IP-uri banate
extern int is_ip_banned(const char *ip);
void* tcp_main(void* arg);

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

// background worker
void* worker_main(void* arg) {
    (void)arg;
    printf("[Worker Thread] Started and waiting for jobs...\n");

    while (1) {
        pthread_mutex_lock(&state_mutex);
        
        while (global_state.q_size == 0) {
            pthread_cond_wait(&global_state.q_cond, &state_mutex);
        }
        
        int ticket_id = global_state.queue[global_state.q_head];
        global_state.q_head = (global_state.q_head + 1) % MAX_QUEUE_SIZE;
        global_state.q_size--;
        
        JobRecord *job = NULL;
        ProcessInfo *client_procs = NULL;
        char filter_name[NAME_LEN];

        for (int i = 0; i < global_state.job_count; i++) {
            if (global_state.jobs[i].ticket_id == ticket_id) {
                job = &global_state.jobs[i];
                job->status = JOB_PROCESSING;
                strncpy(filter_name, job->filter, NAME_LEN - 1);
                
                for (int j = 0; j < global_state.active_clients_count; j++) {
                    if (global_state.clients[j].job_id == job->client_id) {
                        client_procs = global_state.clients[j].P;
                        break;
                    }
                }
                break;
            }
        }
        pthread_mutex_unlock(&state_mutex);

        if (!job) continue;

        // Read input file from disk
        char in_path[128];
        snprintf(in_path, sizeof(in_path), "/tmp/pif_in_%d.dat", ticket_id);
        
        FILE *in_file = fopen(in_path, "rb");
        if (!in_file) {
            pthread_mutex_lock(&state_mutex);
            job->status = JOB_FAILED;
            pthread_mutex_unlock(&state_mutex);
            continue;
        }

        fseek(in_file, 0, SEEK_END);
        size_t in_size = ftell(in_file);
        fseek(in_file, 0, SEEK_SET);
        unsigned char *in_blob = malloc(in_size);
        fread(in_blob, 1, in_size, in_file);
        fclose(in_file);

        unsigned char *out_blob = NULL;
        size_t out_size = 0;
        
        // Execute the GraphicsMagick operations
        int res = process_image(in_blob, in_size, &out_blob, &out_size, filter_name, client_procs);
        
        free(in_blob);    // Clean up input heap memory
        remove(in_path);  // Delete the input file from disk to save space

        pthread_mutex_lock(&state_mutex);
        if (res == 0 && out_blob != NULL) {
            // Save result to disk
            char out_path[128];
            snprintf(out_path, sizeof(out_path), "/tmp/pif_out_%d.dat", ticket_id);
            FILE *out_file = fopen(out_path, "wb");
            if (out_file) {
                fwrite(out_blob, 1, out_size, out_file);
                fclose(out_file);
                job->out_size = out_size;
                job->status = JOB_DONE;
                log_entry("Job %d completed successfully", ticket_id);
            } else {
                job->status = JOB_FAILED;
            }
            free(out_blob); // Clean up output heap memory
        } else {
            job->status = JOB_FAILED;
            log_entry("Job %d failed during processing", ticket_id);
        }
        pthread_mutex_unlock(&state_mutex);
    }
    return NULL;
}

/*
 * Endpoint pentru conectare client:
 * - verifica daca serverul este deschis si daca are loc
 * - genereaza un ID unic pentru noul client
 * - initializeaza info despre procesele clientului
 */
int __ns__connect(struct soap *soap, struct _ns__connect *req, struct _ns__connectResponse *resp) {
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
int __ns__echo(struct soap *soap, struct _ns__echo *req, struct _ns__echoResponse *resp) {
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

int __ns__applyFilter(struct soap *soap, struct _ns__applyFilter *req, struct ns__applyFilterResponse *resp) {
    if (!req || !req->filterType || !req->imageData.__ptr) {
        return soap_receiver_fault(soap, "Bad Request", "Missing filter or image data");
    }
    
    int client_id = (req->clientId) ? *req->clientId : -1;
    int ticket_id = rand() % 1000000 + 1;

    // Immediately save incoming image to disk
    char in_path[128];
    snprintf(in_path, sizeof(in_path), "/tmp/pif_in_%d.dat", ticket_id);
    FILE *in_file = fopen(in_path, "wb");
    if (!in_file) {
        return soap_receiver_fault(soap, "Server Error", "Could not write image to disk");
    }
    fwrite(req->imageData.__ptr, 1, req->imageData.__size, in_file);
    fclose(in_file);

    pthread_mutex_lock(&state_mutex);
    if (global_state.q_size >= MAX_QUEUE_SIZE || global_state.job_count >= MAX_QUEUE_SIZE) {
        pthread_mutex_unlock(&state_mutex);
        remove(in_path); // Cleanup if queue is full
        return soap_receiver_fault(soap, "Server Overloaded", "The processing queue is full.");
    }

    JobRecord *job = &global_state.jobs[global_state.job_count++];
    job->ticket_id = ticket_id;
    job->client_id = client_id;
    strncpy(job->filter, req->filterType, NAME_LEN - 1);
    job->out_size = 0;
    job->status = JOB_PENDING;

    global_state.queue[global_state.q_tail] = ticket_id;
    global_state.q_tail = (global_state.q_tail + 1) % MAX_QUEUE_SIZE;
    global_state.q_size++;

    for (int i = 0; i < FILTERNR; i++) {
        if (strcmp(global_state.filters[i].name, req->filterType) == 0) {
            global_state.filters[i].uses++;
            break;
        }
    }

    pthread_cond_signal(&global_state.q_cond);
    pthread_mutex_unlock(&state_mutex);

    log_entry("Queued filter '%s' for client %d. Ticket: %d", req->filterType, client_id, ticket_id);
    resp->ticketId = ticket_id; 
    return SOAP_OK;
}

int __ns__checkStatus(struct soap *soap, struct _ns__checkStatus *req, struct ns__checkStatusResponse *resp) {
    if (!req) return soap_receiver_fault(soap, "Bad Request", "Missing request");
    
    int ticket_id = req->ticketId;
    JobRecord *found_job = NULL;

    pthread_mutex_lock(&state_mutex);
    for (int i = 0; i < global_state.job_count; i++) {
        if (global_state.jobs[i].ticket_id == ticket_id) {
            found_job = &global_state.jobs[i];
            break;
        }
    }

    if (!found_job) {
        pthread_mutex_unlock(&state_mutex);
        return soap_receiver_fault(soap, "Not Found", "Invalid ticket ID");
    }

    switch(found_job->status) {
        case JOB_PENDING:
            resp->statusString = soap_strdup(soap, "PENDING");
            break;
        case JOB_PROCESSING:
            resp->statusString = soap_strdup(soap, "PROCESSING");
            break;
        case JOB_FAILED:
            resp->statusString = soap_strdup(soap, "FAILED");
            break;
        case JOB_DONE:
            resp->statusString = soap_strdup(soap, "DONE");
            
            char out_path[128];
            snprintf(out_path, sizeof(out_path), "/tmp/pif_out_%d.dat", ticket_id);
            FILE *out_file = fopen(out_path, "rb");
            if (out_file) {
                resp->imageData.__size = found_job->out_size;
                resp->imageData.__ptr = (unsigned char*)soap_malloc(soap, found_job->out_size);
                fread(resp->imageData.__ptr, 1, found_job->out_size, out_file);
                fclose(out_file);
                
                // Optional: Delete the file once the client has retrieved it
                remove(out_path);
            }
            break;
    }
    
    pthread_mutex_unlock(&state_mutex);
    return SOAP_OK;
}

// endpoint pentru deconectare client
int __ns__bye(struct soap *soap, struct _ns__bye *req, struct _ns__byeResponse *resp) {
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
int __ns__serverInfo(struct soap *soap, struct _ns__serverInfo *req, struct ns__serverInfoResponse *resp) {
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
    
    // INITIALIZE NEW QUEUE METRICS
    global_state.job_count = 0;
    global_state.q_head = 0;
    global_state.q_tail = 0;
    global_state.q_size = 0;
    pthread_cond_init(&global_state.q_cond, NULL);
    
    pthread_mutex_unlock(&state_mutex);

    pthread_t unixthr, tcpthr, workerthr;
    int sport = SOAP_PORT;
    int tport = 18083;

    // resetam socketul UNIX in caz ca exista deja pe disk
    unlink(UNIXSOCKET);

    // pornire thread-uri
    pthread_create(&unixthr, NULL, unix_main, (void*)UNIXSOCKET);
    pthread_create(&tcpthr, NULL, tcp_main, &tport);
    pthread_create(&workerthr, NULL, worker_main, NULL);

    printf("Server started with UNIX socket, SOAP, TCP, and Worker threads\n");
    soap_main(&sport);

    pthread_join(unixthr, NULL);
    pthread_join(tcpthr, NULL);

    // Cleanup resources
    pthread_cond_destroy(&global_state.q_cond);
    DestroyMagick();
    
    return 0;
}