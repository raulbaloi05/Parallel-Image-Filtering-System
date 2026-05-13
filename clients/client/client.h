/**
 * client.h — API client PIF peste gSOAP (SOAP/HTTP).
 *
 * Functiile returneaza 0 la succes, -1 la eroare.
 * Apelantul detine ciclul de viata al struct soap*.
 */

#ifndef CLIENT_H
#define CLIENT_H

#include "config.h"

struct soap;

/**
 * Apeleaza ns:connect. Primeste clientID alocat de server.
 */
int client_connect(struct soap *soap, const clientConfigType *cfg, int *client_id);

/**
 * Citeste imaginea de la input_path, trimite via ns:applyFilter
 * cu filtrul dat si salveaza rezultatul la output_path.
 */
int client_apply_filter(struct soap *soap, const clientConfigType *cfg,
                        int client_id,
                        const char *filter_name,
                        const char *input_path,
                        const char *output_path);

/**
 * Trimite ns:bye cu id-ul sesiunii pentru a inchide corect pe server.
 */
int client_bye(struct soap *soap, const clientConfigType *cfg, int client_id);

#endif /* CLIENT_H */
