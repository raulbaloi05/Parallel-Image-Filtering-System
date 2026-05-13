/**
 * config.h — incarcarea configuratiei clientului PIF.
 *
 * DE CE exista acest modul?
 * MilestoneA.txt cere: "libconfig (mandatory)" + "parsing argumente/optiuni
 * pe linia de comanda". Modulul asta le acopera pe amandoua.
 *
 * REGULA DE PRECEDENTA (de la mai slab la mai tare):
 *   1. valorile default (hardcodate, plasa de siguranta)
 *   2. variabile de mediu (PIF_SERVER_HOST, PIF_SERVER_PORT, PIF_FILTER, ...)
 *   3. fisierul client.cfg (libconfig)
 *   4. argumentele din linia de comanda (-h, -p, -F, -i, -o)
 * Aceasta e conventia standard in aplicatiile UNIX — CLI bate fisierul,
 * fisierul bate env-ul, env-ul bate defaults.
 */

#ifndef CONFIG_H
#define CONFIG_H

#define CFG_HOST_MAX    64    /* lungime max hostname */
#define CFG_PATH_MAX   256    /* lungime max cai de fisier */
#define CFG_FILTER_MAX  64    /* lungime max nume filtru (ex: "grayscale") */

/**
 * Toata starea de configurare a clientului.
 * DE CE struct si nu globale? Testabila, pass-by-pointer curat,
 * clang-analyzer vede cand un camp nu e initializat.
 */
typedef struct clientConfig {
    /* --- parametri retea --- */
    char host[CFG_HOST_MAX];          /* adresa serverului */
    int  port;                         /* portul TCP */

    /* --- parametri functionalitate proiect --- */
    char filter_name[CFG_FILTER_MAX]; /* filtrul de aplicat: "grayscale", "blur", etc. */
    char input_path[CFG_PATH_MAX];    /* calea imaginii de intrare */
    char output_path[CFG_PATH_MAX];   /* unde salvam rezultatul */

    /* --- flaguri CLI --- */
    int  verbose;        /* 1 daca -v */
} clientConfigType;

void config_set_defaults(clientConfigType *cfg);
void config_load_env(clientConfigType *cfg);
int  config_load_file(clientConfigType *cfg, const char *path);
int  config_parse_args(clientConfigType *cfg, int argc, char **argv);
void config_print(const clientConfigType *cfg);

#endif /* CONFIG_H */
