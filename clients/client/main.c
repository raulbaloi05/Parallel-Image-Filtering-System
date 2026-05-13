#define _POSIX_C_SOURCE 200809L

/**
 * main.c — client PIF interactiv (SOAP/HTTP).
 *
 * Flux:
 *   1. config + connect
 *   2. loop: citeste comanda, aplica filtru
 *   3. "exit" → bye + deconectare
 *
 * Comenzi:
 *   <filtru> <input> <output>   — aplica filtru
 *   exit                        — deconectare
 *   help                        — lista comenzi
 *
 * Exemplu:
 *   ./pif-client -h 127.0.0.1 -p 18082 -v
 */

#include "config.h"    /* Utilizat pentru: config_set_defaults(), config_load_env(), config_load_file(), config_parse_args(), config_print(), clientConfigType, CFG_FILTER_MAX, CFG_PATH_MAX */
#include "client.h"    /* Utilizat pentru: client_connect(), client_apply_filter(), client_bye() */
#include "soapH.h"     /* Utilizat pentru: struct soap, soap_new(), soap_destroy(), soap_end(), soap_free() */

#include <stdio.h>     /* Utilizat pentru: printf(), fprintf(), fgets(), fflush(), stdout, stderr, stdin */
#include <stdlib.h>    /* Utilizat pentru: EXIT_SUCCESS, EXIT_FAILURE */
#include <string.h>    /* Utilizat pentru: strcmp(), strcspn(), sscanf() */

#define CMD_MAX               512 /* Lungimea maxima a unei comenzi introduse interactiv la promptul pif> */
#define SOAP_CONNECT_TIMEOUT  10  /* Timeout in secunde pentru stabilirea conexiunii TCP cu serverul */
#define SOAP_IO_TIMEOUT       30  /* Timeout in secunde pentru trimiterea/primirea datelor SOAP */

static void print_help(void)
{
    printf("Comenzi disponibile:\n");
    printf("  <filtru> <input> <output>  — aplica filtru pe imagine\n");
    printf("  Filtre: grayscale, blur, sharpen, edge, negative\n");
    printf("  exit                       — deconectare si iesire\n");
    printf("  help                       — aceasta lista\n");
}

int main(int argc, char **argv)
{
    clientConfigType cfg;
    config_set_defaults(&cfg);
    config_load_env(&cfg);
    (void)config_load_file(&cfg, NULL);
    if (config_parse_args(&cfg, argc, argv) < 0) {
        return EXIT_FAILURE;
    }
    if (cfg.verbose) {
        config_print(&cfg);
    }

    struct soap *soap = soap_new();
    if (soap == NULL) {
        fprintf(stderr, "[client] soap_new esuat\n");
        return EXIT_FAILURE;
    }
    soap->keep_alive = 0;
    soap->connect_timeout = SOAP_CONNECT_TIMEOUT;
    soap->send_timeout = SOAP_IO_TIMEOUT;
    soap->recv_timeout = SOAP_IO_TIMEOUT;

    int client_id = 0;
    if (client_connect(soap, &cfg, &client_id) < 0) {
        soap_destroy(soap);
        soap_end(soap);
        soap_free(soap);
        return EXIT_FAILURE;
    }

    printf("Conectat. clientID=%d\n", client_id);
    printf("Scrie 'help' pentru lista de comenzi.\n");

    char line[CMD_MAX];
    int exit_code = EXIT_SUCCESS;

    while (1) {
        printf("pif> ");
        fflush(stdout);

        if (fgets(line, sizeof(line), stdin) == NULL) {
            /* EOF (Ctrl+D) — deconectare curata */
            printf("\n");
            break;
        }

        /* strip newline */
        line[strcspn(line, "\n")] = '\0';

        if (line[0] == '\0') continue;

        if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0) {
            break;
        }

        if (strcmp(line, "help") == 0) {
            print_help();
            continue;
        }

        /* parse: <filtru> <input> <output> */
        char filter[CFG_FILTER_MAX], input[CFG_PATH_MAX], output[CFG_PATH_MAX];
        if (sscanf(line, "%63s %255s %255s", filter, input, output) != 3) {
            printf("Sintaxa: <filtru> <input> <output>\n");
            continue;
        }

        int flt_rc = client_apply_filter(soap, &cfg, client_id,
                                         filter, input, output);
        if (flt_rc == -2) {
            /* eroare retea/SOAP — reconecteaza */
            printf("[client] conexiune pierduta, reconectare...\n");
            soap_end(soap);
            if (client_connect(soap, &cfg, &client_id) < 0) {
                exit_code = EXIT_FAILURE;
                goto cleanup;
            }
            printf("Reconectat. clientID=%d\n", client_id);
        } else if (flt_rc < 0) {
            /* eroare locala (fisier lipsa, disc plin etc.) — ramane conectat */
            printf("[client] filtrare esuata (eroare locala).\n");
        }
    }

    client_bye(soap, &cfg, client_id);

cleanup:
    soap_destroy(soap);
    soap_end(soap);
    soap_free(soap);
    return exit_code;
}
