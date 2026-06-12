/*
 * jobs.c — coada FIFO de joburi cu tichete (vezi jobs.h pentru flux).
 *
 * Sincronizare:
 * - jobs_mutex protejeaza vectorul de sloturi;
 * - jobs_cond trezeste worker-ul cand soseste un job nou;
 * - worker-ul elibereaza mutex-ul pe durata process_image() ca sa nu blocheze
 *   submit/poll in timpul procesarii (care poate dura secunde).
 *
 * Tichetele sunt one-shot: dupa ce clientul ridica rezultatul (DONE/ERROR),
 * slotul se elibereaza si tichetul devine necunoscut. Acelasi comportament
 * are si un tichet expirat prin reutilizarea slotului.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>

#include "jobs.h"
#include "processing.h"
#include "dataTypes.h"

extern pthread_mutex_t state_mutex;
extern ServerState global_state;

typedef struct {
    int ticket;                 /* 0 = slot liber */
    int client_id;
    int seq;                    /* ordinea sosirii, pentru FIFO */
    int status;                 /* JOB_PENDING / RUNNING / DONE / ERROR */
    char filter[NAME_LEN];
    unsigned char *in_data;
    size_t in_size;
    unsigned char *out_data;
    size_t out_size;
    int processing_ms;
    char error[MESSAGE_LEN];
} Job;

static Job jobs[MAX_JOBS];
static int next_seq = 1;
static pthread_mutex_t jobs_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t jobs_cond = PTHREAD_COND_INITIALIZER;

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static void free_slot(Job *j) {
    free(j->in_data);
    free(j->out_data);
    memset(j, 0, sizeof(*j));
}

int jobs_submit(const unsigned char *data, size_t size,
                const char *filter, int client_id) {
    if (!data || size == 0 || !filter) return -1;

    pthread_mutex_lock(&jobs_mutex);
    Job *slot = NULL;
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].ticket == 0) { slot = &jobs[i]; break; }
    }
    if (!slot) {
        pthread_mutex_unlock(&jobs_mutex);
        return -1; /* coada plina */
    }

    slot->in_data = (unsigned char *)malloc(size);
    if (!slot->in_data) {
        pthread_mutex_unlock(&jobs_mutex);
        return -1;
    }
    memcpy(slot->in_data, data, size);
    slot->in_size = size;
    slot->client_id = client_id;
    slot->seq = next_seq++;
    slot->status = JOB_PENDING;
    snprintf(slot->filter, sizeof(slot->filter), "%s", filter);
    /* tichet aleator 1..99999, unic intre sloturile ocupate */
    int ticket;
    do {
        ticket = rand() % 99999 + 1;
        for (int i = 0; i < MAX_JOBS; i++)
            if (jobs[i].ticket == ticket) { ticket = 0; break; }
    } while (ticket == 0);
    slot->ticket = ticket;

    pthread_cond_signal(&jobs_cond);
    pthread_mutex_unlock(&jobs_mutex);
    return ticket;
}

int jobs_poll(int ticket, unsigned char **out, size_t *out_size,
              int *ms, char *err, size_t errlen) {
    if (ticket <= 0) return JOB_UNKNOWN;

    pthread_mutex_lock(&jobs_mutex);
    Job *j = NULL;
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].ticket == ticket) { j = &jobs[i]; break; }
    }
    if (!j) {
        pthread_mutex_unlock(&jobs_mutex);
        return JOB_UNKNOWN;
    }

    int status = j->status;
    if (status == JOB_DONE) {
        if (out) { *out = j->out_data; j->out_data = NULL; } /* transfer ownership */
        if (out_size) *out_size = j->out_size;
        if (ms) *ms = j->processing_ms;
        free_slot(j);
    } else if (status == JOB_ERROR) {
        if (err && errlen > 0) snprintf(err, errlen, "%s", j->error);
        free_slot(j);
    }
    pthread_mutex_unlock(&jobs_mutex);
    return status;
}

int jobs_queue_size(void) {
    int n = 0;
    pthread_mutex_lock(&jobs_mutex);
    for (int i = 0; i < MAX_JOBS; i++)
        if (jobs[i].ticket != 0 &&
            (jobs[i].status == JOB_PENDING || jobs[i].status == JOB_RUNNING))
            n++;
    pthread_mutex_unlock(&jobs_mutex);
    return n;
}

/* Cauta jobul PENDING cu seq minim (FIFO). Apelat cu jobs_mutex tinut. */
static Job *oldest_pending(void) {
    Job *best = NULL;
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].ticket != 0 && jobs[i].status == JOB_PENDING &&
            (!best || jobs[i].seq < best->seq))
            best = &jobs[i];
    }
    return best;
}

void *jobs_worker_main(void *arg) {
    (void)arg;
    printf("[Jobs Thread] Worker pornit (coada de %d sloturi)\n", MAX_JOBS);

    for (;;) {
        pthread_mutex_lock(&jobs_mutex);
        Job *j;
        while ((j = oldest_pending()) == NULL)
            pthread_cond_wait(&jobs_cond, &jobs_mutex);

        j->status = JOB_RUNNING;
        /* copii locale ca sa putem elibera mutex-ul pe durata procesarii */
        int ticket = j->ticket;
        int client_id = j->client_id;
        char filter[NAME_LEN];
        memcpy(filter, j->filter, sizeof(filter));
        unsigned char *in_data = j->in_data;
        size_t in_size = j->in_size;
        pthread_mutex_unlock(&jobs_mutex);

        /* statistici filtre + procesele clientului, ca in applyFilter sincron */
        pthread_mutex_lock(&state_mutex);
        ProcessInfo *client_procs = NULL;
        for (int i = 0; i < FILTERNR; i++) {
            if (strcmp(global_state.filters[i].name, filter) == 0) {
                global_state.filters[i].uses++;
                break;
            }
        }
        for (int i = 0; i < global_state.active_clients_count; i++) {
            if (global_state.clients[i].job_id == client_id) {
                client_procs = global_state.clients[i].P;
                break;
            }
        }
        pthread_mutex_unlock(&state_mutex);

        long long start = now_ms();
        unsigned char *out_blob = NULL;
        size_t out_size = 0;
        int res = process_image(in_data, in_size, &out_blob, &out_size,
                                filter, client_procs);
        int elapsed = (int)(now_ms() - start);

        pthread_mutex_lock(&jobs_mutex);
        /* slotul poate fi intre timp eliberat doar de poll pe DONE/ERROR,
         * imposibil cat e RUNNING — dar verificam tichetul defensiv */
        if (j->ticket == ticket) {
            if (res == 0 && out_blob != NULL) {
                j->out_data = out_blob;
                j->out_size = out_size;
                j->processing_ms = elapsed;
                j->status = JOB_DONE;
                printf("[Jobs Thread] Tichet %d: '%s' procesat in %d ms\n",
                       ticket, filter, elapsed);
            } else {
                free(out_blob);
                snprintf(j->error, sizeof(j->error),
                         "Image processing failed (filter '%s')", filter);
                j->status = JOB_ERROR;
                printf("[Jobs Thread] Tichet %d: procesare esuata\n", ticket);
            }
        } else {
            free(out_blob);
        }
        pthread_mutex_unlock(&jobs_mutex);
    }
    return NULL;
}
