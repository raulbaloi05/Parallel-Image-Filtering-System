/* _POSIX_C_SOURCE 200809L — expune POSIX.1-2008: getopt, optarg etc.
   DE CE? Cu -std=c11, GCC ascunde extensiile POSIX implicit. Trebuie sa fie
   PRIMUL lucru din fisier, inainte de orice #include. */
#define _POSIX_C_SOURCE 200809L

/**
 * config.c — implementarea incarcarii configuratiei clientului PIF.
 *
 * Lib externa: libconfig (https://hyperrealm.github.io/libconfig/).
 * Instalare: sudo apt install libconfig-dev
 * Link: -lconfig (vezi Makefile).
 *
 * DE CE libconfig? MilestoneA.txt o cere explicit ("mandatory").
 * Sintaxa curata (key = value;), tipuri native, usor de folosit in C.
 */

#include "config.h"

#include <libconfig.h>   /* config_t, config_init, config_read_file, ... */
#include <stdio.h>       /* fprintf, snprintf */
#include <stdlib.h>      /* atoi, getenv */
#include <string.h>      /* strncmp, memset */
#include <unistd.h>      /* getopt, optarg */

/* Valorile default — detalii de implementare, nu API public. */
#define DEFAULT_HOST      "127.0.0.1"
#define DEFAULT_PORT      18082
#define DEFAULT_FILTER    "grayscale"    /* filtrul implicit daca nu se specifica */
#define DEFAULT_INPUT     "./input.jpg"  /* imaginea de intrare implicita */
#define DEFAULT_OUTPUT    "./output.jpg" /* unde salvam rezultatul implicit */
#define DEFAULT_CFG_PATH  "./client.cfg"
#define PORT_MAX          65535

void config_set_defaults(clientConfigType *cfg)
{
    /* memset zero — garanteaza ca niciun byte din struct nu e neinitializat,
       inclusiv padding bytes. clang-analyzer-core.uninitialized ne-ar prinde
       daca am omite asta. */
    memset(cfg, 0, sizeof(*cfg));

    /* snprintf in loc de strcpy — strcpy nu verifica dimensiunea bufferului
       si poate produce overflow. clang-tidy il blocheaza prin
       clang-analyzer-security.insecureAPI.strcpy. */
    (void)snprintf(cfg->host,        sizeof(cfg->host),        "%s", DEFAULT_HOST);
    (void)snprintf(cfg->filter_name, sizeof(cfg->filter_name), "%s", DEFAULT_FILTER);
    (void)snprintf(cfg->input_path,  sizeof(cfg->input_path),  "%s", DEFAULT_INPUT);
    (void)snprintf(cfg->output_path, sizeof(cfg->output_path), "%s", DEFAULT_OUTPUT);
    cfg->port    = DEFAULT_PORT;
    cfg->verbose = 0;

    /* DE CE "(void)" inainte de snprintf? cert-err33-c cere sa folosim
       sau sa ignoram EXPLICIT valoarea returnata. */
}

void config_load_env(clientConfigType *cfg)
{
    /* MilestoneA.txt cere "informatii de mediu". Sursele (de la slab la tare):
       defaults < env < fisier < CLI. Env-ul sta intre defaults si libconfig
       ca sa poti suprascrie valori implicite fara sa editezi client.cfg. */
    const char *e;

    if ((e = getenv("PIF_SERVER_HOST")) != NULL && *e != '\0') {
        (void)snprintf(cfg->host, sizeof(cfg->host), "%s", e);
    }
    if ((e = getenv("PIF_SERVER_PORT")) != NULL && *e != '\0') {
        int p = atoi(e);
        if (p > 0 && p <= PORT_MAX) {
            cfg->port = p;
        }
    }
    if ((e = getenv("PIF_FILTER")) != NULL && *e != '\0') {
        (void)snprintf(cfg->filter_name, sizeof(cfg->filter_name), "%s", e);
    }
    if ((e = getenv("PIF_INPUT")) != NULL && *e != '\0') {
        (void)snprintf(cfg->input_path, sizeof(cfg->input_path), "%s", e);
    }
    if ((e = getenv("PIF_OUTPUT")) != NULL && *e != '\0') {
        (void)snprintf(cfg->output_path, sizeof(cfg->output_path), "%s", e);
    }
    if ((e = getenv("PIF_VERBOSE")) != NULL && *e != '\0') {
        cfg->verbose = (atoi(e) != 0) ? 1 : 0;
    }
}

int config_load_file(clientConfigType *cfg, const char *path)
{
    if (path == NULL) {
        path = DEFAULT_CFG_PATH;
    }

    /* Pattern standard libconfig: init → read → lookup → destroy. */
    config_t conf;
    config_init(&conf);

    if (config_read_file(&conf, path) == CONFIG_FALSE) {
        /* Fisierul lipseste sau are erori de sintaxa — NU e fatal.
           Continuam cu defaults + CLI. */
        (void)fprintf(stderr,
            "[config] atentie: nu pot citi %s (%s linia %d) — folosesc defaults\n",
            path, config_error_text(&conf), config_error_line(&conf));
        config_destroy(&conf);
        return -1;
    }

    /* config_lookup_string() intoarce pointer valabil pana la config_destroy().
       De aceea copiem cu snprintf in struct-ul nostru. */
    const char *tmp = NULL;

    if (config_lookup_string(&conf, "server.host", &tmp) == CONFIG_TRUE) {
        (void)snprintf(cfg->host, sizeof(cfg->host), "%s", tmp);
    }

    int tmp_int = 0;
    if (config_lookup_int(&conf, "server.port", &tmp_int) == CONFIG_TRUE) {
        cfg->port = tmp_int;
    }

    /* Parametrii specifici proiectului PIF — cititi din sectiunea [client]. */
    if (config_lookup_string(&conf, "client.filter_name", &tmp) == CONFIG_TRUE) {
        (void)snprintf(cfg->filter_name, sizeof(cfg->filter_name), "%s", tmp);
    }
    if (config_lookup_string(&conf, "client.input_path", &tmp) == CONFIG_TRUE) {
        (void)snprintf(cfg->input_path, sizeof(cfg->input_path), "%s", tmp);
    }
    if (config_lookup_string(&conf, "client.output_path", &tmp) == CONFIG_TRUE) {
        (void)snprintf(cfg->output_path, sizeof(cfg->output_path), "%s", tmp);
    }

    /* config_destroy elibereaza memoria interna a libconfig. Fara el, leak. */
    config_destroy(&conf);
    return 0;
}

static void print_usage(const char *prog)
{
    (void)fprintf(stderr,
        "Usage: %s [optiuni]\n"
        "  -h <host>     adresa server       (default %s)\n"
        "  -p <port>     port TCP            (default %d)\n"
        "  -c <path>     fisier config       (default %s)\n"
        "  -F <filtru>   filtru imagine      (default %s)\n"
        "  -i <cale>     imagine de intrare  (default %s)\n"
        "  -o <cale>     imagine de iesire   (default %s)\n"
        "  -v            verbose\n"
        "  --help        acest mesaj\n",
        prog,
        DEFAULT_HOST, DEFAULT_PORT, DEFAULT_CFG_PATH,
        DEFAULT_FILTER, DEFAULT_INPUT, DEFAULT_OUTPUT);
}

int config_parse_args(clientConfigType *cfg, int argc, char **argv)
{
    /* --help inainte de getopt — getopt ar confunda "--help" cu optiune invalida. */
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--help", sizeof("--help") - 1) == 0) {
            print_usage(argv[0]);
            return -1;
        }
    }

    /* getopt — POSIX standard pentru optiuni scurte.
       "h:p:c:F:i:o:vf":
         ':'  dupa litera = optiunea cere argument (optarg)
         fara ':' = flag boolean */
    int opt;
    while ((opt = getopt(argc, argv, "h:p:c:F:i:o:v")) != -1) {
        switch (opt) {
            case 'h':
                (void)snprintf(cfg->host, sizeof(cfg->host), "%s", optarg);
                break;
            case 'p':
                /* atoi returneaza 0 pentru input invalid — acceptabil pentru demo.
                   In cod de productie: strtol() cu verificare errno. */
                cfg->port = atoi(optarg);
                break;
            case 'c':
                /* -c incarca fisierul indicat INAINTE ca restul optiunilor CLI
                   sa-l suprascrie — ordinea conteaza. */
                (void)config_load_file(cfg, optarg);
                break;
            case 'F':
                (void)snprintf(cfg->filter_name, sizeof(cfg->filter_name),
                               "%s", optarg);
                break;
            case 'i':
                (void)snprintf(cfg->input_path, sizeof(cfg->input_path),
                               "%s", optarg);
                break;
            case 'o':
                (void)snprintf(cfg->output_path, sizeof(cfg->output_path),
                               "%s", optarg);
                break;
            case 'v':
                cfg->verbose = 1;
                break;
            default:
                print_usage(argv[0]);
                return -1;
        }
    }
    return 0;
}

void config_print(const clientConfigType *cfg)
{
    (void)fprintf(stdout,
        "[config] host=%s port=%d filter=\"%s\" input=\"%s\" output=\"%s\""
        " verbose=%d\n",
        cfg->host, cfg->port,
        cfg->filter_name, cfg->input_path, cfg->output_path,
        cfg->verbose);
}
