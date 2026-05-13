#define _POSIX_C_SOURCE 200809L

/**
 * Programul de mai jos implementeaza operatiile de retea ale clientului PIF
 * (Parallel Image Filtering), comunicand cu serverul prin protocolul SOAP/HTTP.
 * Este utilizat de main.c care gestioneaza bucla interactiva si configuratia.
 *
 * Expune urmatoarele operatii:
 * -- client_connect()      : trimite cererea SOAP ns:connect, primeste clientID;
 * -- client_apply_filter() : citeste imaginea de pe disc, o trimite la server
 *                            prin ns:applyFilter si salveaza rezultatul;
 * -- client_bye()          : trimite ns:bye pentru a inchide sesiunea pe server.
 *
 * Comunicarea foloseste stub-urile generate automat de gSOAP din pif.h
 * (structuri _ns__*, _ns__*Response si functii soap_call___ns__*).
 * Fisierele soapClient.c si soapC.c contin codul de serializare XML — nu le
 * modifica manual.
 *
 * Am tratat urmatoarele situatii limita:
 * -- fisierul imagine de intrare nu poate fi deschis sau este gol;
 * -- cererea SOAP esueaza (timeout, server indisponibil): returneaza -1;
 * -- serverul returneaza imagine goala sau dimensiune invalida;
 * -- scrierea rezultatului pe disc esueaza (disc plin, permisiuni).
 */

#include "client.h"    /* Utilizat pentru: client_connect(), client_apply_filter(), client_bye(), clientConfigType */
#include "soapH.h"     /* Utilizat pentru: struct soap, soap_call___ns__connect(), soap_call___ns__applyFilter(), soap_call___ns__bye(), soap_print_fault(), soap_closesock(), struct _ns__connect, struct _ns__connectResponse, struct _ns__applyFilter, struct ns__applyFilterResponse, struct _ns__bye, struct _ns__byeResponse, struct ns__byeRequest, SOAP_OK */
#include "ns.nsmap"    /* Utilizat pentru: namespace-urile SOAP generate de gSOAP */

#include <stdio.h>     /* Utilizat pentru: fopen(), fclose(), fseek(), ftell(), rewind(), fread(), fwrite(), fprintf(), stdout, stderr, SEEK_END */
#include <stdlib.h>    /* Utilizat pentru: malloc(), free() */
#include <string.h>    /* Utilizat pentru: memset() */

#define URL_BUF_LEN     128 /* Lungimea bufferului pentru URL-ul serverului (http://host:port) */
#define PROCESS_WORKERS 4   /* Numarul de procese paralele folosite la procesarea imaginii pe server */

static void make_url(char *buf, size_t n, const clientConfigType *cfg)
{
    snprintf(buf, n, "http://%s:%d", cfg->host, cfg->port);
}

int client_connect(struct soap *soap, const clientConfigType *cfg, int *client_id)
{
    char url[URL_BUF_LEN];
    make_url(url, sizeof(url), cfg);

    struct _ns__connect req;
    struct _ns__connectResponse resp;
    memset(&req, 0, sizeof(req));
    memset(&resp, 0, sizeof(resp));

    if (soap_call___ns__connect(soap, url, NULL, &req, &resp) != SOAP_OK) {
        soap_print_fault(soap, stderr);
        return -1;
    }
    if (resp.connect == NULL) {
        fprintf(stderr, "[client] server nu a returnat clientID\n");
        return -1;
    }
    *client_id = *resp.connect;
    if (cfg->verbose) {
        fprintf(stdout, "[client] conectat la %s, clientID=%d\n", url, *client_id);
    }
    return 0;
}

int client_apply_filter(struct soap *soap, const clientConfigType *cfg,
                        int client_id,
                        const char *filter_name,
                        const char *input_path,
                        const char *output_path)
{
    char url[URL_BUF_LEN];
    make_url(url, sizeof(url), cfg);

    /* Citim imaginea in memorie. */
    FILE *fin = fopen(input_path, "rb");
    if (fin == NULL) {
        fprintf(stderr, "[client] nu pot deschide: %s\n", input_path);
        return -1;  /* eroare locala, fara reconectare */
    }
    if (fseek(fin, 0, SEEK_END) < 0) { fclose(fin); return -1; }
    long fsize = ftell(fin);
    rewind(fin);
    if (fsize <= 0) {
        fprintf(stderr, "[client] fisier gol: %s\n", input_path);
        fclose(fin);
        return -1;  /* eroare locala, fara reconectare */
    }

    unsigned char *img = (unsigned char *)malloc((size_t)fsize);
    if (img == NULL) { fclose(fin); return -1; }
    if ((long)fread(img, 1, (size_t)fsize, fin) != fsize) {
        fprintf(stderr, "[client] fread incomplet\n");
        free(img); fclose(fin); return -1;
    }
    fclose(fin);

    if (cfg->verbose) {
        fprintf(stdout, "[client] imagine incarcata: %s (%ld bytes)\n", input_path, fsize);
    }

    /* Construim request-ul SOAP. */
    struct _ns__applyFilter req;
    struct ns__applyFilterResponse resp;
    memset(&req, 0, sizeof(req));
    memset(&resp, 0, sizeof(resp));

    req.imageData.__ptr  = img;
    req.imageData.__size = (int)fsize;
    req.imageData.id      = NULL;
    req.imageData.type    = NULL;
    req.imageData.options = NULL;
    req.filterType   = (char *)filter_name;
    req.processCount = PROCESS_WORKERS;
    req.clientId     = &client_id;

    int rc = soap_call___ns__applyFilter(soap, url, NULL, &req, &resp);
    free(img);
    soap_closesock(soap);

    if (rc != SOAP_OK) {
        soap_print_fault(soap, stderr);
        return -2;  /* eroare retea/SOAP — reconectare necesara */
    }
    if (resp.imageData.__ptr == NULL || resp.imageData.__size <= 0) {
        fprintf(stderr, "[client] server a returnat imagine goala\n");
        return -1;
    }

    /* Salvam rezultatul. */
    FILE *fout = fopen(output_path, "wb");
    if (fout == NULL) {
        fprintf(stderr, "[client] nu pot scrie: %s\n", output_path);
        return -1;
    }
    if ((int)fwrite(resp.imageData.__ptr, 1, (size_t)resp.imageData.__size, fout)
        != resp.imageData.__size) {
        fprintf(stderr, "[client] fwrite incomplet\n");
        fclose(fout); return -1;
    }
    fclose(fout);

    fprintf(stdout, "[client] rezultat salvat: %s (%d bytes, %d ms)\n",
            output_path, resp.imageData.__size, resp.processingTime);
    return 0;
}

int client_bye(struct soap *soap, const clientConfigType *cfg, int client_id)
{
    char url[URL_BUF_LEN];
    make_url(url, sizeof(url), cfg);

    struct ns__byeRequest body;
    memset(&body, 0, sizeof(body));
    body.id = client_id;

    struct _ns__bye req;
    struct _ns__byeResponse resp;
    memset(&req, 0, sizeof(req));
    memset(&resp, 0, sizeof(resp));
    req.byeRequest = &body;

    if (soap_call___ns__bye(soap, url, NULL, &req, &resp) != SOAP_OK) {
        soap_print_fault(soap, stderr);
        return -1;
    }
    if (cfg->verbose) {
        int st = (resp.status != NULL) ? *resp.status : -1;
        fprintf(stdout, "[client] bye status=%d\n", st);
    }
    return 0;
}
