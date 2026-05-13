/**
 * tcp_client.c — client TCP pentru PIF.
 *
 * Protocol binar identic cu cel din tcp_server.c. Reuseaza Header din
 * dataTypes.h. Transfer fisiere bidirectional (client -> server -> client)
 * fara overhead base64/SOAP -> potrivit pentru imagini mari (sute MB).
 *
 * Utilizare:
 *   tcp_client <host> <port> <filter> <input_path> <output_path>
 *
 * Exemple:
 *   tcp_client 127.0.0.1 18083 grayscale in.jpg out.jpg
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/stat.h>

#include "../../server/dataTypes.h"

#define MAX_FILTER_LEN NAME_LEN

static int send_all(int fd, const void *buf, size_t n) {
    const char *p = (const char*)buf;
    while (n > 0) {
        ssize_t s = send(fd, p, n, 0);
        if (s <= 0) return -1;
        p += s; n -= (size_t)s;
    }
    return 0;
}

static int recv_all(int fd, void *buf, size_t n) {
    char *p = (char*)buf;
    while (n > 0) {
        ssize_t r = recv(fd, p, n, 0);
        if (r <= 0) return -1;
        p += r; n -= (size_t)r;
    }
    return 0;
}

static int read_file(const char *path, unsigned char **buf, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror("fopen input"); return -1; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return -1; }
    rewind(f);
    unsigned char *b = (unsigned char*)malloc((size_t)sz);
    if (!b) { fclose(f); return -1; }
    if (fread(b, 1, (size_t)sz, f) != (size_t)sz) {
        free(b); fclose(f); return -1;
    }
    fclose(f);
    *buf = b;
    *size = (size_t)sz;
    return 0;
}

static int write_file(const char *path, const unsigned char *buf, size_t size) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror("fopen output"); return -1; }
    if (fwrite(buf, 1, size, f) != size) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

static int tcp_connect_to(const char *host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        /* fallback DNS */
        struct hostent *he = gethostbyname(host);
        if (!he) { fprintf(stderr, "Host '%s' nerezolvat\n", host); close(fd); return -1; }
        memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
    }
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return -1;
    }
    return fd;
}

static int op_connect(int fd, int *out_id) {
    Header h = { TCP_CONNECT, 0, 0 };
    if (send_all(fd, &h, sizeof(h)) < 0) return -1;
    Header r;
    if (recv_all(fd, &r, sizeof(r)) < 0) return -1;
    if (r.type == TCP_ERROR) {
        char *msg = (char*)malloc(r.size);
        if (msg && recv_all(fd, msg, r.size) == 0) {
            fprintf(stderr, "Server err: %s\n", msg);
        }
        free(msg);
        return -1;
    }
    if (r.type != TCP_CONNECT_RESP || r.size != sizeof(int)) return -1;
    if (recv_all(fd, out_id, sizeof(int)) < 0) return -1;
    return 0;
}

static int op_apply(int fd, int client_id, const char *filter,
                    const unsigned char *img, size_t img_size,
                    unsigned char **out, size_t *out_size, int *dt) {
    char fbuf[MAX_FILTER_LEN];
    memset(fbuf, 0, sizeof(fbuf));
    strncpy(fbuf, filter, MAX_FILTER_LEN - 1);

    size_t payload = sizeof(int) + MAX_FILTER_LEN + sizeof(size_t) + img_size;
    Header h = { TCP_APPLY_FILTER, payload, 0 };
    if (send_all(fd, &h, sizeof(h)) < 0) return -1;
    if (send_all(fd, &client_id, sizeof(int)) < 0) return -1;
    if (send_all(fd, fbuf, MAX_FILTER_LEN) < 0) return -1;
    if (send_all(fd, &img_size, sizeof(size_t)) < 0) return -1;
    if (send_all(fd, img, img_size) < 0) return -1;

    Header r;
    if (recv_all(fd, &r, sizeof(r)) < 0) return -1;
    if (r.type == TCP_ERROR) {
        char *msg = (char*)malloc(r.size);
        if (msg && recv_all(fd, msg, r.size) == 0) {
            fprintf(stderr, "Server err: %s\n", msg);
        }
        free(msg);
        return -1;
    }
    if (r.type != TCP_FILTER_RESP) return -1;

    int t;
    size_t osz;
    if (recv_all(fd, &t, sizeof(int)) < 0) return -1;
    if (recv_all(fd, &osz, sizeof(size_t)) < 0) return -1;
    unsigned char *buf = (unsigned char*)malloc(osz);
    if (!buf) return -1;
    if (recv_all(fd, buf, osz) < 0) { free(buf); return -1; }

    *out = buf;
    *out_size = osz;
    *dt = t;
    return 0;
}

static int op_bye(int fd, int client_id) {
    Header h = { TCP_BYE, sizeof(int), 0 };
    if (send_all(fd, &h, sizeof(h)) < 0) return -1;
    if (send_all(fd, &client_id, sizeof(int)) < 0) return -1;
    Header r;
    if (recv_all(fd, &r, sizeof(r)) < 0) return -1;
    if (r.type != TCP_BYE_RESP) return -1;
    int status;
    if (recv_all(fd, &status, sizeof(int)) < 0) return -1;
    return status == 1 ? 0 : -1;
}

int main(int argc, char **argv) {
    if (argc != 6) {
        fprintf(stderr,
                "Utilizare: %s <host> <port> <filter> <input> <output>\n"
                "  filter: grayscale|blur|negative|edge|sharpen\n",
                argv[0]);
        return 1;
    }
    const char *host = argv[1];
    int port = atoi(argv[2]);
    const char *filter = argv[3];
    const char *in_path = argv[4];
    const char *out_path = argv[5];

    unsigned char *img = NULL;
    size_t img_size = 0;
    if (read_file(in_path, &img, &img_size) < 0) {
        fprintf(stderr, "Nu pot citi %s\n", in_path);
        return 1;
    }
    printf("[client] Citit %zu bytes din %s\n", img_size, in_path);

    int fd = tcp_connect_to(host, port);
    if (fd < 0) { free(img); return 1; }

    int client_id = -1;
    if (op_connect(fd, &client_id) < 0) {
        fprintf(stderr, "Connect esuat\n");
        close(fd); free(img); return 1;
    }
    printf("[client] Conectat. id=%d\n", client_id);

    unsigned char *out = NULL;
    size_t out_size = 0;
    int dt = 0;
    if (op_apply(fd, client_id, filter, img, img_size, &out, &out_size, &dt) < 0) {
        fprintf(stderr, "applyFilter esuat\n");
        op_bye(fd, client_id);
        close(fd); free(img); return 1;
    }
    printf("[client] Procesat in %d ms. Rezultat: %zu bytes\n", dt, out_size);

    if (write_file(out_path, out, out_size) < 0) {
        fprintf(stderr, "Scriere %s esuata\n", out_path);
    } else {
        printf("[client] Salvat in %s\n", out_path);
    }

    op_bye(fd, client_id);
    close(fd);
    free(img);
    free(out);
    return 0;
}
