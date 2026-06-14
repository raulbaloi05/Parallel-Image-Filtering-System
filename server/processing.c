/**
 * Programul de mai jos implementeaza procesarea paralela a imaginilor,
 * constituind nucleul functional al sistemului PIF (Parallel Image Filtering).
 * Este apelat din server.c la fiecare cerere ns__applyFilter primita de la client.
 *
 * Procesarea se realizeaza in urmatorii pasi:
 * -- imaginea primita ca blob (vector de bytes) este decodificata cu BlobToImage();
 * -- imaginea este impartita in 4 zone egale (grila 2x2);
 * -- se creeaza PROCESS_COUNT (4) procese copil prin fork(), fiecare procesand
 *    zona sa in paralel si salvand rezultatul intr-un fisier temporar in /tmp/;
 * -- parintele asteapta terminarea copiilor cu waitpid() si citeste statisticile
 *    de CPU/RAM din /proc/PID/stat inainte de reap;
 * -- cele 4 zone procesate sunt asamblate intr-o imagine finala cu CompositeImage();
 * -- rezultatul este convertit inapoi in blob cu ImageToBlob() si returnat.
 *
 * Filtrele suportate: grayscale, blur, sharpen, edge, negative.
 * Biblioteca de procesare: GraphicsMagick (API C).
 *
 * Am tratat urmatoarele situatii limita:
 * -- imaginea nu poate fi decodificata (BlobToImage esueaza): returneaza -1;
 * -- un fisier temporar nu poate fi citit la asamblare: zona respectiva e ignorata;
 * -- copiii folosesc _exit() in loc de exit() pentru a evita coruptia
 *    mutex-urilor GraphicsMagick mostenite de la parinte la fork().
 */
#define _POSIX_C_SOURCE 200809L /* Expune clock_gettime(), CLOCK_MONOTONIC si alte extensii POSIX */

#include <stdio.h>              /* Utilizat pentru: fopen(), fclose(), fgets(), snprintf(), FILE */
#include <stdlib.h>             /* Utilizat pentru: free() */
#include <string.h>             /* Utilizat pentru: strcmp(), snprintf() */
#include <unistd.h>             /* Utilizat pentru: sysconf(), _SC_CLK_TCK, _exit(), getpid() */
#include <time.h>               /* Utilizat pentru: clock_gettime(), struct timespec, CLOCK_MONOTONIC */
#include <sys/wait.h>           /* Utilizat pentru: waitpid() */
#include <GraphicsMagick/magick/api.h> /* Utilizat pentru: BlobToImage(), ImageToBlob(), CropImage(), CompositeImage(), DestroyImage(), DestroyImageInfo(), CloneImageInfo(), CloneImage(), WriteImage(), ReadImage(), TransformColorspace(), BlurImage(), SharpenImage(), EdgeImage(), NegateImage(), GetExceptionInfo(), DestroyExceptionInfo(), CatchException(), GRAYColorspace, OverCompositeOp, Image, ImageInfo, ExceptionInfo, RectangleInfo */

#include "processing.h"        /* Utilizat pentru: process_image(), ProcessInfo, PROCESS_COUNT (prin dataTypes.h) */
#include <math.h> /* Utilizat pentru: fmin() */
#define PROC_PATH_LEN    64  /* Lungimea bufferului pentru calea /proc/[pid]/stat */
#define PROC_LINE_LEN    128 /* Lungimea bufferului pentru citirea liniilor din /proc/[pid]/status */
#define FILENAME_BUF_LEN 128 /* Lungimea bufferului pentru numele fisierelor temporare din /tmp/ */
#define MS_PER_SEC       1000   /* Milisecunde intr-o secunda */
#define KB_PER_MB        1024   /* Kilobytes intr-un megabyte */
#define CPU_PERCENT      100    /* Factor de scalare pentru calculul procentajului CPU */

#include <pthread.h>           /* Utilizat pentru: pthread_t, pthread_mutex_t, pthread_mutex_lock(), pthread_mutex_unlock(), pthread_create(), pthread_join(), PTHREAD_MUTEX_INITIALIZER */

extern pthread_rwlock_t fork_lock;

/*UPDATE DIN NOU:
* citire info despre un proces din /proc
* calculeaza cpu time (ms) (inainte erau doar dummy stats)
* calculeaza ram usage (MB) (la fel, foloseam dummy stats inainte)
*/
static void read_proc_stats(pid_t pid, int *cpu_ms, int *ram_kb) {
    *cpu_ms = 0;
    *ram_kb = 0;

    //construire path spre /proc/[pid]/stat
    char path[PROC_PATH_LEN];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    //deschidere fisier stat cu info despre CPU 
    FILE *f = fopen(path, "r");
    if (f) {
        long utime, stime;
        //citire timpul + system (in ticks)
        (void)fscanf(f, "%*d %*s %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu",
               &utime, &stime);
        fclose(f);
        //convertire ticks in ms (milisecunde)
        long ticks = sysconf(_SC_CLK_TCK);
        if (ticks > 0)
            *cpu_ms = (int)((utime + stime) * MS_PER_SEC / ticks);
    }
    //aici trecem la ram
    snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);
    f = fopen(path, "r");
    if (f) {
        char line[PROC_LINE_LEN];
        //cautare linia VmRSS de unde se afla memoria reala folosita
        while (fgets(line, sizeof(line), f)) {
            long val;
            if (sscanf(line, "VmRSS: %ld kB", &val) == 1) {
                *ram_kb = (int)(val / KB_PER_MB);
                break;
            }
        }
        fclose(f);
    }
}


/*
 * ce face:
 *  - primeste o imagine ca bytes (blob)
 *  - o imparte in 4 bucati
 *  - proceseaza fiecare bucata in paralel (fork)
 *  - aplica un filtru (blur, grayscale etc)
 *  - reconstruieste imaginea finala
 *  - returneaza rezultatul tot ca bytes
 *
 * UPDATE (20.04.2026):
 *  - returneaza info despre procese (PID, status, cpu, ram)
 *
 * IMPORTANT:
 *  - nu foloseste socket (doar procesare)
 *  - foloseste GraphicsMagick API
 *  - foloseste fisiere temporare in /tmp
 */

int process_image(const unsigned char *input, size_t in_size,
                  unsigned char **output, size_t *out_size,
                  const char *filter,
                  ProcessInfo *proc_array) 
{
    pid_t parent_pid = getpid();
    pid_t pids[PROCESS_COUNT];
    struct timespec t_start;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    // FORK fara GraphicsMagick in parinte
    for (int i = 0; i < PROCESS_COUNT; i++) {
        pids[i] = fork();

        if (pids[i] == 0) {
            // Copilul isi creeaza propriul context GM izolat, independent de parinte
            InitializeMagick(NULL);
            ExceptionInfo child_ex;
            GetExceptionInfo(&child_ex);
            ImageInfo *child_info = CloneImageInfo(NULL);

            Image *child_img = BlobToImage(child_info, input, in_size, &child_ex);
            if (child_img) {
                unsigned long half_w = child_img->columns / 2;
                unsigned long half_h = child_img->rows / 2;
                unsigned long x = (i % 2) * half_w;
                unsigned long y = (i / 2) * half_h;

                RectangleInfo rect = {half_w, half_h, x, y};
                Image *part = CropImage(child_img, &rect, &child_ex);

                if (part) {
                    // Aplicare filtre
                    if (strcmp(filter, "grayscale") == 0) {
                        TransformColorspace(part, GRAYColorspace);
                    } else if (strcmp(filter, "blur") == 0) {
                        Image *tmp = BlurImage(part, 0.0, 3.0, &child_ex);
                        if (tmp) { DestroyImage(part); part = tmp; }
                    } else if (strcmp(filter, "sharpen") == 0) {
                        Image *tmp = SharpenImage(part, 0.0, 2.0, &child_ex);
                        if (tmp) { DestroyImage(part); part = tmp; }
                    } else if (strcmp(filter, "edge") == 0) {
                        Image *tmp = EdgeImage(part, 1.0, &child_ex);
                        if (tmp) { DestroyImage(part); part = tmp; }
                    } else if (strcmp(filter, "negative") == 0) {
                        NegateImage(part, 0);
                    }

                    char filename[FILENAME_BUF_LEN];
                    snprintf(filename, sizeof(filename), "/tmp/part_%d_%d.png", parent_pid, i);
                    snprintf(part->filename, sizeof(part->filename), "%s", filename);

                    WriteImage(child_info, part);
                    DestroyImage(part);
                }
                DestroyImage(child_img);
            }
            DestroyImageInfo(child_info);
            DestroyExceptionInfo(&child_ex);
            
            // Distrugem contextul GM al copilului inainte de _exit pentru a nu lasa thread-uri
            DestroyMagick(); 
            _exit(0);
        }
        else {
            if (proc_array) {
                proc_array[i].pid = pids[i];
                proc_array[i].cpu = 0;
                proc_array[i].ram = 0;
                snprintf(proc_array[i].status, sizeof(proc_array[i].status), "RUN");
            }
        }
    }

    // ASTEPTAM COPIII
    for (int i = 0; i < PROCESS_COUNT; i++) {
        int cpu_ms = 0, ram_mb = 0;
        if (pids[i] > 0) {
            if (proc_array) read_proc_stats(pids[i], &cpu_ms, &ram_mb);
            waitpid(pids[i], NULL, 0);

            if (proc_array) {
                struct timespec t_end;
                clock_gettime(CLOCK_MONOTONIC, &t_end);
                long wall_ms = (t_end.tv_sec - t_start.tv_sec) * 1000 + 
                               (t_end.tv_nsec - t_start.tv_nsec) / 1000000;
                proc_array[i].cpu = (wall_ms > 0) ? (int)(cpu_ms * 100 / wall_ms) : 0;
                proc_array[i].ram = ram_mb;
                snprintf(proc_array[i].status, sizeof(proc_array[i].status), "DONE");
            }
        }
    }

    // ASAMBLAM REZULTATUL
    // Parintele initializeaza GM doar acum, cand `fork` nu mai este un pericol
    InitializeMagick(NULL);
    ExceptionInfo exception;
    GetExceptionInfo(&exception);
    ImageInfo *image_info = CloneImageInfo(NULL);

    Image *result = BlobToImage(image_info, input, in_size, &exception);
    if (!result) {
        DestroyImageInfo(image_info);
        DestroyExceptionInfo(&exception);
        DestroyMagick();
        return -1;
    }

    unsigned long half_w = result->columns / 2;
    unsigned long half_h = result->rows / 2;

    for (int i = 0; i < PROCESS_COUNT; i++) {
        char filename[128];
        snprintf(filename, sizeof(filename), "/tmp/part_%d_%d.png", parent_pid, i);
        snprintf(image_info->filename, sizeof(image_info->filename), "%s", filename);
        
        Image *part = ReadImage(image_info, &exception);
        if (part) {
            unsigned long x = (i % 2) * half_w;
            unsigned long y = (i / 2) * half_h;
            CompositeImage(result, OverCompositeOp, part, x, y);
            DestroyImage(part);
            (void)remove(filename);
        }
    }

    // Conversie finala folosind MagickFree
    unsigned char *magick_blob = ImageToBlob(image_info, result, out_size, &exception);
    if (magick_blob) {
        *output = malloc(*out_size);
        memcpy(*output, magick_blob, *out_size);
        MagickFree(magick_blob); 
    } else {
        *output = NULL;
    }

    DestroyImage(result);
    DestroyImageInfo(image_info);
    DestroyExceptionInfo(&exception);
    
    // Parintele curata tot inainte de a termina
    DestroyMagick(); 

    return 0;
}