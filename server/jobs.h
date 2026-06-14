#ifndef JOBS_H
#define JOBS_H

#include <stddef.h>

/*
 * Coada FIFO de joburi cu tichete pentru procesarea asincrona a imaginilor.
 *
 * Fluxul:
 *   1. clientul trimite imaginea  -> jobs_submit() o pune in coada si
 *      returneaza imediat un tichet;
 *   2. un worker thread (jobs_worker_main) proceseaza joburile in ordinea
 *      sosirii, apeland process_image();
 *   3. clientul interogheaza periodic jobs_poll() cu tichetul; cand jobul
 *      este DONE primeste rezultatul, iar slotul este eliberat (one-shot).
 *
 * Folosita atat de serverul SOAP (server.c) cat si de cel TCP (tcp_server.c).
 */

/* Starile unui job (valorile ajung si in protocolul TCP — nu le reordona) */
#define JOB_UNKNOWN (-1) /* tichet inexistent sau rezultat deja ridicat */
#define JOB_PENDING   0  /* in coada, neinceput */
#define JOB_RUNNING   1  /* in curs de procesare */
#define JOB_DONE      2  /* terminat cu succes, rezultat disponibil */
#define JOB_ERROR     3  /* procesarea a esuat */

#define MAX_JOBS 8192      /* capacitatea cozii */

/* Adauga un job in coada. Datele sunt copiate intern.
 * Returneaza tichetul (>0) sau -1 daca coada e plina / parametri invalizi. */
int jobs_submit(const unsigned char *data, size_t size,
                const char *filter, int client_id);

/* Interogheaza starea jobului cu tichetul dat.
 * - intoarce una din valorile JOB_*;
 * - la JOB_DONE: *out primeste bufferul rezultat (malloc — apelantul face
 *   free), *out_size dimensiunea, *ms durata procesarii; slotul se elibereaza;
 * - la JOB_ERROR: err primeste mesajul de eroare; slotul se elibereaza.
 * out/out_size/ms/err pot fi NULL daca nu intereseaza. */
int jobs_poll(int ticket, unsigned char **out, size_t *out_size,
              int *ms, char *err, size_t errlen);

/* Numarul de joburi aflate in coada (PENDING + RUNNING). */
int jobs_queue_size(void);

/* Bucla worker-ului; de pornit o singura data, intr-un pthread. */
void *jobs_worker_main(void *arg);

#endif
