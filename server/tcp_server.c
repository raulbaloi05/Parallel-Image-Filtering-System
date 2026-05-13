/**
 * tcp_server.c — server TCP pentru clientul REMOTE (IN).
 *
 * Asculta pe portul TCP_PORT (18083). Protocol binar bazat pe Header din
 * dataTypes.h, identic ca framing cu UNIX socket-ul, dar extins pentru
 * transfer fisiere binare (imagini) bidirectional fara overhead base64/SOAP.
 *
 * Ops suportate:
 *  -- TCP_CONNECT       : aloca un client_id, raspunde TCP_CONNECT_RESP
 *  -- TCP_APPLY_FILTER  : primeste filter + img, returneaza img procesata
 *  -- TCP_BYE           : deconecteaza clientul
 *
 * Reuseaza process_image() din processing.c si global_state din server.c.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

#include "dataTypes.h"
#include "processing.h"

#define TCP_PORT       18083
#define TCP_BACKLOG    16
#define MAX_FILTER_LEN NAME_LEN
#define MAX_JOB_ID     10000

extern pthread_mutex_t state_mutex;
extern ServerState global_state;
extern int is_ip_banned(const char *ip);

/* logging din server.c — refolosim printf local pentru a evita expunere */
static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

/* send_all: trimite exact n bytes sau eroare */
static int send_all(int fd, const void *buf, size_t n) {
    const char *p = (const char*)buf;
    size_t left = n;
    while (left > 0) {
        ssize_t s = send(fd, p, left, 0);
        if (s <= 0) return -1;
        p += s;
        left -= (size_t)s;
    }
    return 0;
}

/* recv_all: primeste exact n bytes sau eroare */
static int recv_all(int fd, void *buf, size_t n) {
    char *p = (char*)buf;
    size_t left = n;
    while (left > 0) {
        ssize_t r = recv(fd, p, left, 0);
        if (r <= 0) return -1;
        p += r;
        left -= (size_t)r;
    }
    return 0;
}

static void send_error(int fd, const char *msg) {
    size_t len = strlen(msg) + 1;
    Header h = { TCP_ERROR, len, 0 };
    if (send_all(fd, &h, sizeof(h)) == 0) {
        send_all(fd, msg, len);
    }
}

static int alloc_client(const char *ip) {
    pthread_mutex_lock(&state_mutex);
    if (strcmp(global_state.config.status, "CLOSED") == 0 ||
        global_state.active_clients_count >= (int)global_state.config.max_clients_number) {
        pthread_mutex_unlock(&state_mutex);
        return -1;
    }
    int id = rand() % MAX_JOB_ID + 1;
    ClientInfo *c = &global_state.clients[global_state.active_clients_count++];
    c->job_id = id;
    snprintf(c->ip, IP_LEN, "%.*s", IP_LEN - 1, ip);
    for (int i = 0; i < PROCESS_COUNT; i++) {
        c->P[i].pid = 0;
        c->P[i].cpu = 0;
        c->P[i].ram = 0;
        memcpy(c->P[i].status, "IDLE", sizeof("IDLE"));
    }
    pthread_mutex_unlock(&state_mutex);
    return id;
}

static void remove_client(int id) {
    pthread_mutex_lock(&state_mutex);
    for (int i = 0; i < global_state.active_clients_count; i++) {
        if (global_state.clients[i].job_id == id) {
            for (int j = i; j < global_state.active_clients_count - 1; j++)
                global_state.clients[j] = global_state.clients[j + 1];
            global_state.active_clients_count--;
            break;
        }
    }
    pthread_mutex_unlock(&state_mutex);
}

static ProcessInfo *find_client_procs(int id) {
    for (int i = 0; i < global_state.active_clients_count; i++) {
        if (global_state.clients[i].job_id == id)
            return global_state.clients[i].P;
    }
    return NULL;
}

/* handler conexiune. ruleaza pe thread propriu pana clientul iese. */
static void handle_tcp_conn(int fd, const char *peer_ip) {
    if (is_ip_banned(peer_ip)) {
        send_error(fd, "IP banned");
        return;
    }

    int client_id = -1;

    while (1) {
        Header req;
        if (recv_all(fd, &req, sizeof(req)) < 0) break;

        if (req.type == TCP_CONNECT) {
            int id = alloc_client(peer_ip);
            if (id < 0) {
                send_error(fd, "Server full or closed");
                break;
            }
            client_id = id;
            Header resp = { TCP_CONNECT_RESP, sizeof(int), 0 };
            if (send_all(fd, &resp, sizeof(resp)) < 0) break;
            if (send_all(fd, &id, sizeof(int)) < 0) break;
            printf("[TCP] Client connected id=%d ip=%s\n", id, peer_ip);
        }
        else if (req.type == TCP_APPLY_FILTER) {
            /* payload: [int client_id][NAME_LEN filter][size_t img_size][img bytes] */
            if (req.size < sizeof(int) + MAX_FILTER_LEN + sizeof(size_t)) {
                send_error(fd, "Bad apply payload");
                break;
            }
            int cid;
            char filter[MAX_FILTER_LEN];
            size_t img_size;
            if (recv_all(fd, &cid, sizeof(int)) < 0) break;
            if (recv_all(fd, filter, MAX_FILTER_LEN) < 0) break;
            filter[MAX_FILTER_LEN - 1] = '\0';
            if (recv_all(fd, &img_size, sizeof(size_t)) < 0) break;

            if (img_size == 0 || img_size > (size_t)1024 * 1024 * 1024) {
                send_error(fd, "Invalid image size");
                break;
            }
            unsigned char *img = (unsigned char*)malloc(img_size);
            if (!img) {
                send_error(fd, "OOM");
                break;
            }
            if (recv_all(fd, img, img_size) < 0) {
                free(img);
                break;
            }

            /* incrementare contor filtru */
            pthread_mutex_lock(&state_mutex);
            for (int i = 0; i < FILTERNR; i++) {
                if (strcmp(global_state.filters[i].name, filter) == 0) {
                    global_state.filters[i].uses++;
                    break;
                }
            }
            ProcessInfo *procs = find_client_procs(cid);
            pthread_mutex_unlock(&state_mutex);

            unsigned char *out = NULL;
            size_t out_size = 0;
            long long t0 = now_ms();
            int rc = process_image(img, img_size, &out, &out_size, filter, procs);
            int dt = (int)(now_ms() - t0);
            free(img);

            if (rc != 0 || out == NULL) {
                send_error(fd, "Processing failed");
                if (out) free(out);
                continue;
            }

            /* payload: [int processingTime][size_t out_size][bytes] */
            size_t payload = sizeof(int) + sizeof(size_t) + out_size;
            Header resp = { TCP_FILTER_RESP, payload, 0 };
            if (send_all(fd, &resp, sizeof(resp)) < 0) { free(out); break; }
            if (send_all(fd, &dt, sizeof(int)) < 0) { free(out); break; }
            if (send_all(fd, &out_size, sizeof(size_t)) < 0) { free(out); break; }
            if (send_all(fd, out, out_size) < 0) { free(out); break; }
            free(out);
            printf("[TCP] applyFilter '%s' client=%d %zu->%zu bytes in %d ms\n",
                   filter, cid, img_size, out_size, dt);
        }
        else if (req.type == TCP_BYE) {
            int id;
            if (req.size >= sizeof(int)) {
                if (recv_all(fd, &id, sizeof(int)) < 0) break;
            } else {
                id = client_id;
            }
            remove_client(id);
            int status = 1;
            Header resp = { TCP_BYE_RESP, sizeof(int), 0 };
            if (send_all(fd, &resp, sizeof(resp)) < 0) break;
            send_all(fd, &status, sizeof(int));
            printf("[TCP] Client disconnected id=%d\n", id);
            client_id = -1;
            break;
        }
        else {
            send_error(fd, "Unknown op");
        }
    }

    if (client_id > 0) remove_client(client_id);
}

typedef struct {
    int fd;
    char ip[IP_LEN];
} ConnArg;

static void *tcp_worker(void *arg) {
    ConnArg *ca = (ConnArg*)arg;
    handle_tcp_conn(ca->fd, ca->ip);
    close(ca->fd);
    free(ca);
    return NULL;
}

void *tcp_main(void *arg) {
    int port = arg ? *(int*)arg : TCP_PORT;
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        perror("[TCP] socket");
        return NULL;
    }
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("[TCP] bind");
        close(srv);
        return NULL;
    }
    if (listen(srv, TCP_BACKLOG) < 0) {
        perror("[TCP] listen");
        close(srv);
        return NULL;
    }
    printf("[TCP Thread] Listening on port %d\n", port);

    while (1) {
        struct sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
        int cfd = accept(srv, (struct sockaddr*)&caddr, &clen);
        if (cfd < 0) continue;

        ConnArg *ca = (ConnArg*)malloc(sizeof(ConnArg));
        if (!ca) { close(cfd); continue; }
        ca->fd = cfd;
        inet_ntop(AF_INET, &caddr.sin_addr, ca->ip, IP_LEN);

        pthread_t t;
        if (pthread_create(&t, NULL, tcp_worker, ca) != 0) {
            close(cfd);
            free(ca);
            continue;
        }
        pthread_detach(t);
    }

    close(srv);
    return NULL;
}
