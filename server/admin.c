/**
 * Programul de mai jos implementeaza un TUI(Terminal User Interface) utilizat pentru administrarea serverului principal.
 * Aceasta este format utilizand libraria Ncurses, comunica cu serverul printr-un socket UNIX si permite: 
 * -- vizualizarea clientiilor conectati la server si eliminarea acestora;
 * -- vizualizarea filtrelor disponibile si de cate ori au fost acestea utilizate;
 * -- deconectarea tuturor clientiilor;
 * -- oprirea conexiuniilor;
 * -- setarea numarului maxim de clienti conectati pe care il poate avea serverul

 * Am tratat urmatoarele situatii limita care pot aparea in contextul programului de mai jos :
 * -- erori de conectare/citire/scriere cu socketul
 */

#include <ncurses.h> /* Utilizat pentru: initscr(), endwin(), newwin(), newpad(), delwin(), box(), keypad(), wgetch(), wrefresh(), prefresh(), refresh(), clear(), werase(), mvwprintw(), mvwvline(), mvwhline(), wattron(), wattroff(), getmaxyx(), cbreak(), noecho(), curs_set(), A_REVERSE, ACS_VLINE, ACS_HLINE, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT */
#include <unistd.h> /* Utilizat pentru: close(), sleep(), pid_t */
#include <stdlib.h> /* Utilizat pentru: malloc(), free() */
#include <string.h> /* Utilizat pentru: mamcpy(), strcmp(), strlen() */
#include <sys/socket.h> /* Utilizat pentru: socket(), connect(), send(), recv(), AF_UNIX, SOCK_STREAM */
#include <sys/un.h> /* Utilizat pentru: struct sockaddr_un */
#include <sys/select.h> /* Utilizat pentru: select(), fd_set, FD_ZERO, FD_SET */

#include "dataTypes.h" /* Utilizat pentru: Header, ClientInfo, toKill, Filter, ServerConfiguration, tipuri de mesaj pentru Header */

#define WINDOW_WIDTH 70 /* Latimea ferestrei principale */
#define PAD_WIDTH 68 /* Latimea pad-ului*/

#define COL_IP 1 /* Coloana unde incepe IP-ul clientului */
#define COL_IP_SEPARATOR 17 /* Separator vertical dupa IP */
#define COL_JOBID 18 /* Coloana unde incepe Job ID-ul */
#define COL_JOBID_SEPARATOR 25 /* Separator vertical dupa Job ID */
#define COL_PID 26 /* Coloana unde incep PID-urile workerilor */
#define COL_PID_SEPARATOR 38 /* Separator vertical dupa PID-uri */
#define COL_CPU 39 /* Coloana unde incep CPU usage-urile workerilor */
#define COL_CPU_SEPARATOR 49 /* Separator vertical dupa CPU usage-uri */
#define COL_RAM 50 /* Coloana unde incep RAM usage-urile workerilor */
#define COL_RAM_SEPARATOR 60 /* Separator vertical dupa RAM usage-uri */
#define COL_STATUS 61 /* Coloana unde incep STATUS-urile workerilor */

#define ROWS_FOR_CLIENT 5 /* Randuri pentru a da display unui client + separator */
#define ROWS_FOR_FILTER 2 /* Rand pentru a da display unui filtru + separator */
#define ROWS_FOR_LOG 1 /* Rand pentru a da display unui filtru */

#define COL_FILTER_NAME 1 /* Coloana unde incepe numele filtrului */
#define COL_FILTER_SEPARATOR 34 /* Separator vertical dupa numele filtrului */
#define COL_FILTER_USAGE 35 /* Coloana unde incepe numarul de utilizari */

#define ENTER_KEY 10 /* Tasta enter */

#define CONFIRMATION_WINDOW_LENGHT 7 /* Inaltimea ferestrei de confirmare */
#define CONFIRMATION_WINDOW_WIDTH 21 /* Latimea ferestrei de confirmare */

#define MAX_CLIENTS_WINDOW_LENGHT 6 /* Inaltimea ferestrei pentru schimbarea numarului maxim */
#define MAX_CLIENTS_WINDOW_WIDTH 38 /* Latimea ferestrei pentru schimbarea numarului maxim */

#define MAIN_MENU_ITEMS_NUMBER 6 /* Numarul de optiuni din main menu */
#define CONFIG_MENU_ITEMS      3 /* Numarul de optiuni din meniul de configurare */
#define SERVER_USAGE_ITEMS     3 /* Numarul de optiuni din meniul server usage */

#define CASE_EXIT 5 /* Cazul de exit */

#define CONNECT_WAIT_SECS 3  /* Secunde de asteptare la conectare */
#define PAD_LINE_WIDTH    72 /* Latimea liniei orizontale din pad */
#define HALF_DIV          2  /* Impartitor pentru jumatate */
#define QUARTER_DIV       4  /* Impartitor pentru sfert */

void main_menu(WINDOW *, int); /* Meniul principal cu cele 6 optiuni */
void monitor_and_control(WINDOW *, int); /* Interfata de vizualizare si eliminare a clientiilor */
void filter_statistics(WINDOW *, int); /* Interfata cu statistica filtrelor */
void change_max_clients(WINDOW *, int, ServerConfiguration *); /* Pop-up pentru schimbarea numarului maxim de clienti */
void configuration(WINDOW *, int); /* Interfata pentru configurarea serverului */
void view_logs(WINDOW *, int); /* Interfata cu log-urile serverului */
void server_usage(WINDOW *, int); /* Interfata pentru usage-ul de resurse al serverului */
int confirmation_window(); /* Pop-up pentru confirmarea eliminarii unui client */

int connect_to_server(); /* Conectarea la serverul UNIX */
int send_requests(int, size_t); /* Trimite un Header de request */
int kill_client(int, ClientInfo *); /* Trimite comanda de kill pentru un client */
int ban_client(int, ClientInfo *); /* Trimite comanda de ban pentru un client */
int send_server_configuration(int, ServerConfiguration *); /* Trimite configuratia serverului */

ClientInfo *get_clients_info(int, int *); /* Cere si primeste lista de clienti */
Filter *get_filters_info(int, int *); /* Cere si primeste lista de filtre */
ServerConfiguration get_server_config(int); /* Cere si primeste configuratia serverului */
LogEntry *get_logs(int, int *); /* Cere si primeste logurile de la server */
SysInfo get_system_info(int); /* Cere si primeste informatiile de sistem */
int check_timeout_notification(int); /* Verifica daca serverul a trimis notificare de timeout */

int main() {
    initscr();   /* Initializeaza ecranul si intra in modul ncurses */
    noecho();    /* Nu adauga caractere unde este cursorul atunci cand apasam o tasta */
    cbreak();    /* Nu mai face buffer cu inputul de la user, deci nu mai trebuie apasat ENTER */
    curs_set(0); /* Face cursorul invizibil */

    refresh(); /* Actualizeaza ecranul standard */
    int yMax, xMax; 
    getmaxyx(stdscr, yMax, xMax); /* Stocheaza latimea si lungimea terminalului in cele doua variabile */

    WINDOW *adminPanel = newwin(yMax / HALF_DIV, WINDOW_WIDTH, yMax / QUARTER_DIV, xMax / QUARTER_DIV); /* Creaza fereastra principala cu inaltimea cat jumatatea terminalului, latimea de lungime 70, pornind din sfertul de sus si cel din stanga al terminalului */
    box(adminPanel, 0, 0); /* Creaza frame-ul din jurul meniului */

    keypad(adminPanel, true); /* Permite utilizarea altor caractere (ne trebuie pentru sageti)*/

    mvwprintw(adminPanel, 2, 2, "Connecting to server...");
    wrefresh(adminPanel); /* Trimite continutul lui AdminPanel pe ecran, actioneaza doar pe fereastra precizata */

    sleep(CONNECT_WAIT_SECS); /* Se asteapta 3 secunde */
    
    int fd = connect_to_server(); /* Se incearca conectarea la server, daca da eroare returneaza -1 */

    if(fd != -1) /* Daca nu primim eroare */
        main_menu(adminPanel, fd); /* Intram in meniul principal */
    else {
        werase(adminPanel); /* Sterge continutul ferestrei */
        box(adminPanel, 0, 0); /* Creaza frame-ul din jurul meniului */
        mvwprintw(adminPanel, 1, 1, "Failed to connect to the server:"); /* Afisam mesaj de eroare */
        wrefresh(adminPanel); /* Trimite continutul lui AdminPanel pe ecran, actioneaza doar pe fereastra precizata */
        sleep(CONNECT_WAIT_SECS); /* Mai asteptam 3 secunde :3 */
    }

    endwin(); /* Restaureaza terminalul si iese din modul curses */
    return 0;
}

/* Afiseaza mesajul de timeout si asteapta ca userul sa apese o tasta */
void display_timeout_message(WINDOW *adminPanel) {
    werase(adminPanel);
    box(adminPanel, 0, 0);
    mvwprintw(adminPanel, 2, 2, "You have been disconnected by the server");
    mvwprintw(adminPanel, 3, 2, "for being AFK.");
    mvwprintw(adminPanel, 5, 2, "Press any key to exit...");
    wrefresh(adminPanel);
    wgetch(adminPanel);
}

/* Afiseaza meniul principal avand 5 optiuni prin care se navigheaza folosind sagetile si enter pentru a selecta optiunea */
void main_menu(WINDOW *adminPanel, int fd) {
    char *menu[MAIN_MENU_ITEMS_NUMBER] = {"Monitor and control", "Configuration", "Statistics", "Logs", "Server usage", "Exit"}; /* Optiunile meniului */
    int hightlight = 0; /* Indeul optiunii selectate */

    while (1) {
        // verifica daca serverul a trimis notificare de timeout
        if (check_timeout_notification(fd)) {
            display_timeout_message(adminPanel);
            return;
        }
        
        werase(adminPanel); /* Sterge continutul ferestrei */
        box(adminPanel, 0, 0); /* Creaza frame-ul din jurul meniului */

        for (size_t i = 0; i < MAIN_MENU_ITEMS_NUMBER; i++) { /* Itereaza si deseneaza optiunile, cea selectata este afisata cu culorile inversate */
            if (i == hightlight)
                wattron(adminPanel, A_REVERSE); /* Activeza atributul de inversare */
            mvwprintw(adminPanel, i + 1, 1,"%s", menu[i]); /* Printeaza optiunea */
            wattroff(adminPanel, A_REVERSE); /* Dezactiveza atributul de inversare */
        }

        wrefresh(adminPanel); /* Trimitem continutul pe ecran */
        int key = wgetch(adminPanel); /* Se asteapta o tasta si o stocheaza in alegere */

        switch (key) { /* Daca tasta este: */
        case KEY_UP: /* Sageata SUS */
            hightlight--; /* Mergem la optiunea anterioara */ 
            if (hightlight <= -1) /* Verificare pentru a nu depasi limita superioara */
                hightlight = 0;
            break;

        case KEY_DOWN: /* Sageata JOS */
            hightlight++; /* Mergem la urmatoarea optiune */
            if (hightlight >= MAIN_MENU_ITEMS_NUMBER) /* Verificare pentru a nu depasi limita inferioara */
                hightlight = MAIN_MENU_ITEMS_NUMBER - 1;
            break;

        case ENTER_KEY: /* ENTER */
            switch (hightlight) {
                case 0:
                    monitor_and_control(adminPanel, fd); /* Navigam in meniul de monitorizare */
                break;

                case 1:
                    configuration(adminPanel, fd); /* Navigam in meniul de configurare */
                break;

                case 2:
                    filter_statistics(adminPanel, fd);/* Navigam in meniul de filtre */
                break;

                case 3:
                    view_logs(adminPanel, fd); /* Navigam in meniul de filtre */
                break;

                case 4:
                    server_usage(adminPanel, fd); /* Navigam in meniul de usage */
                break;

                case CASE_EXIT:
                    return; /* Iesim din functie */
                break;

                default:
                break;
            }

            default:
            break;
        }
    }
}

/* Afiseaza lista clientiilor si detaliile acestora, permite navigarea folosind sagetile si eliminarea unui client folosind ENTER */
void monitor_and_control(WINDOW *adminPanel, int fd) {
    int clients_nr = 0;
    ClientInfo *clients = get_clients_info(fd, &clients_nr); /* Alocam dinamic lista de clienti */
    ServerConfiguration config = get_server_config(fd);
    
    int yMax, xMax;
    getmaxyx(stdscr, yMax, xMax);

    WINDOW *pad = newpad((clients_nr * ROWS_FOR_CLIENT) + 1, PAD_WIDTH); /* Facem un pad in interiorul ecranului pentru a putea face scroll */
    int hightlight = 0; /* Indexul clientului selectat */
    int position = 1; /* Pozitia scoll-ului in pad */
    
    while (1) {
        //getmaxyx(stdscr, yMax, xMax);
        werase(adminPanel); /* Sterge continutul ferestrei */
        box(adminPanel, 0, 0); /* Creaza frame-ul din jurul meniului */

        /* Toate acestea deseneaza headerul tabelului fiind fix */
        mvwprintw(adminPanel, 1, COL_IP, "Client IP");
        
        mvwvline(adminPanel, 1, COL_IP_SEPARATOR, ACS_VLINE, 1);
        mvwprintw(adminPanel, 1, COL_JOBID, "Job ID");

        mvwvline(adminPanel, 1, COL_JOBID_SEPARATOR, ACS_VLINE, 1);
        mvwprintw(adminPanel, 1, COL_PID, "Workers PID");

        mvwvline(adminPanel, 1, COL_PID_SEPARATOR, ACS_VLINE, 1);
        mvwprintw(adminPanel, 1, COL_CPU, "CPU");

        mvwvline(adminPanel, 1, COL_CPU_SEPARATOR, ACS_VLINE, 1);
        mvwprintw(adminPanel, 1, COL_RAM, "RAM");

        mvwvline(adminPanel, 1, COL_RAM_SEPARATOR, ACS_VLINE, 1);
        mvwprintw(adminPanel, 1, COL_STATUS, "Status");
        
        mvwhline(adminPanel, 2, 1, ACS_HLINE, PAD_WIDTH);

        werase(pad); /* Sterge continutul pad-ului inainte de a-l redesena */
        int y = 1;

        for (size_t i = 0; i < clients_nr; i++){
            if(i == hightlight) 
                    wattron(pad, A_REVERSE); /* Evidentiaza linia clientului selectat */

            for (size_t j = 0; j < 4; j++) {
                if (j == 0) { /* IP-ul si Job ID-ul se afiseaza doar pe primul rand al fiecarui client */
                    mvwprintw(pad, y, COL_IP - 1, "%s", clients[i].ip);
                    mvwvline(pad, y, COL_IP_SEPARATOR - 1, ACS_VLINE, 4);
                    mvwprintw(pad, y, COL_JOBID - 1, "%d", clients[i].job_id);
                }

                mvwvline(pad, y, COL_JOBID_SEPARATOR - 1, ACS_VLINE, 1);
                mvwprintw(pad, y, COL_PID - 1, "%d", clients[i].P[j].pid);

                mvwvline(pad, y, COL_PID_SEPARATOR - 1, ACS_VLINE, 1);
                mvwprintw(pad, y, COL_CPU - 1, "%d %%",
                    strcmp(clients[i].P[j].status, "IDLE") == 0 ? 0 : clients[i].P[j].cpu);

                mvwvline(pad, y, COL_CPU_SEPARATOR - 1, ACS_VLINE, 1);
                mvwprintw(pad, y, COL_RAM - 1, "%d MB", clients[i].P[j].ram);

                mvwvline(pad, y, COL_RAM_SEPARATOR - 1, ACS_VLINE, 1);
                mvwprintw(pad, y, COL_STATUS - 1, "%s", clients[i].P[j].status);

                y++;
            }
            wattroff(pad, A_REVERSE); /* Oprim atributul de REVERSE */
            mvwhline(pad, y++, 0, ACS_HLINE, PAD_LINE_WIDTH); /* Linie orizontala pentru a separa clientii */
        }

        mvwhline(adminPanel, yMax / HALF_DIV - 5, 1, ACS_HLINE, PAD_WIDTH);
        mvwprintw(adminPanel, yMax / HALF_DIV - 4, 1, "Clienti conectati: %d/%d ", clients_nr, config.max_clients_number);
        mvwprintw(adminPanel, yMax / HALF_DIV - 3, 1, "Press ENTER to disconnect or ban the highlighted client");
        mvwprintw(adminPanel, yMax / HALF_DIV - 2, 1, "Press X to go back to main menu");

        wrefresh(adminPanel); /* Trimitem continutul pe ecran */

        /* Aceasta afiseaza doar o portiune din pad pe ecran */
        prefresh(pad,
             position, 0,
             yMax / QUARTER_DIV + 3,
             xMax / QUARTER_DIV + 1,
             yMax / HALF_DIV + yMax / QUARTER_DIV - ROWS_FOR_CLIENT - 1,
             xMax / QUARTER_DIV + PAD_WIDTH);

        int key = wgetch(adminPanel); /* Se asteapta o tasta si o stocheaza in alegere */

        if(position < 1) /* Verificam ca pozitia sa nu depaseaza limita superioara */
            position = 1;

        switch (key){
            case KEY_UP:
                if (position > 1)
                    position -= ROWS_FOR_CLIENT; /* Deruleaza pad-ul in sus cu un client (5 lini) */
                
                if(hightlight > 0) /* Nu se depaseste limita superioara */
                    hightlight--;
            break;

            case KEY_DOWN:
                position += ROWS_FOR_CLIENT; /* Deruleaza pad-ul in jos cu un client (5 lini) */
                if(hightlight < clients_nr - 1) /* Nu se depaseste limita inferioara */
                    hightlight++;
            break;

            case ENTER_KEY:
                if(clients_nr == 0) /* Daca lista este goala nu se poate elimina nimic */
                    break;

                /* Deschide fereastra de confirmare iar daca alege sa elimine clientul rearanjam lista*/
                int action = confirmation_window();
                if(action == 1) {
                    if(kill_client(fd, &clients[hightlight]) < 0)
                            break; /* Eroare de trimitere */
    
                    for(int i = hightlight; i < clients_nr - 1; i++)
                        clients[i] = clients[i+1];
                    clients_nr--;

                    if(clients_nr == 0) /* Daca lista este goala resetam highlight-ul */
                        hightlight = 0;

                    else if(hightlight >= clients_nr) /* Daca clientul eliminat era ultimul, mutam highlight-ul la noul ultim client */
                        hightlight = clients_nr - 1;
                        
                } else if(action == 2) {
                    if(ban_client(fd, &clients[hightlight]) < 0)
                            break; /* Eroare de trimitere */
    
                    for(int i = hightlight; i < clients_nr - 1; i++)
                        clients[i] = clients[i+1];
                    clients_nr--;

                    if(clients_nr == 0) /* Daca lista este goala resetam highlight-ul */
                        hightlight = 0;

                    else if(hightlight >= clients_nr) /* Daca clientul eliminat era ultimul, mutam highlight-ul la noul ultim client */
                        hightlight = clients_nr - 1;
                }
                wrefresh(adminPanel);
                keypad(adminPanel, true);

            break;
        
            case 'x':
                delwin(pad); /* Eliberam memoria pad-ului */
                free(clients); /* Eliberam memoria listei de clienti */
                clear(); /* Sterge tot ecranul */
                refresh(); /* Facem refresh la stdscr */
                return; /* Iesim din functie */
            break;

            default:
            break;
        }
    }
}

/* Afiseaza lista filtrelor si utilizariile acestora, permite navigarea folosind sagetile */
void filter_statistics(WINDOW *adminPanel, int fd) {
    int filters_nr = 0;
    Filter *filters = get_filters_info(fd, &filters_nr); /* Alocam dinamic lista de filtre */
    int yMax, xMax;
    getmaxyx(stdscr, yMax, xMax);

    WINDOW *pad = newpad((filters_nr * ROWS_FOR_FILTER) + 1 , PAD_WIDTH);  /* Facem un pad in interiorul ecranului pentru a putea face scroll */

    int position = 1; /* Pozitia scoll-ului in pad */
    while (1) {
        werase(adminPanel);
        box(adminPanel, 0, 0); /* Creaza frame-ul din jurul meniului */

        /* Toate acestea deseneaza headerul tabelului fiind fix */
        mvwprintw(adminPanel, 1, COL_FILTER_NAME, "Filter name");
        
        mvwvline(adminPanel, 1, COL_FILTER_SEPARATOR, ACS_VLINE, 1);
        mvwprintw(adminPanel, 1, COL_FILTER_USAGE, "Usages");

        mvwhline(adminPanel, 2, 1, ACS_HLINE, PAD_WIDTH);

        werase(pad); /* Sterge continutul pad-ului inainte de a-l redesena */

        int y = 1;

        for (size_t i = 0; i < filters_nr; i++){
            mvwprintw(pad, y, COL_FILTER_NAME - 1, "%s", filters[i].name);
            mvwvline(pad, y, COL_FILTER_SEPARATOR - 1, ACS_VLINE, 1);
            mvwprintw(pad, y, COL_FILTER_USAGE - 1, "%d", filters[i].uses);
            mvwhline(pad, ++y, 0, ACS_HLINE, PAD_LINE_WIDTH);
            y++;
        }

        mvwhline(adminPanel, yMax / HALF_DIV - 3, 1, ACS_HLINE, PAD_WIDTH);
        mvwprintw(adminPanel, yMax / HALF_DIV - 2, 1, "Press X to go back to main menu");

        wrefresh(adminPanel);  /* Actualizeaza fereastra principala cu textul de mai sus */

        /* Aceasta afiseaza doar o portiune din pad pe ecran */
        prefresh(pad,
             position, 0,
             yMax / QUARTER_DIV + 3,
             xMax / QUARTER_DIV + 1,
             yMax / HALF_DIV + yMax / QUARTER_DIV - ROWS_FOR_FILTER,
             xMax / QUARTER_DIV + PAD_WIDTH);

        int key = wgetch(adminPanel); /* Se asteapta o tasta si o stocheaza in alegere */

        if(position < 1)
            position = 1;

        switch (key){
            case KEY_UP:
                if (position > 1)
                    position -= ROWS_FOR_FILTER; /* Deruleaza pad-ul in sus cu un filtru (2 lini) */

            break;

            case KEY_DOWN:
                position += ROWS_FOR_FILTER; /* Deruleaza pad-ul in jos cu un filtru (2 lini) */
            break;

            case 'x':
                delwin(pad); /* Eliberam memoria pad-ului */
                free(filters); /* Eliberam memoria listei de filtre */
                clear(); /* Sterge tot ecranul */
                refresh(); /* Facem refresh la stdscr */
                return; /* Iesim din functie */
            break;

            default:
            break;
        }
    }
}

/* Afiseaza meniul de configurarea a serverului si permite deconectarea tuturor clientilor, comutarea starii conexiunilor si modificarea numarului maxim de clienti */
void configuration(WINDOW *adminPanel, int fd) {
    ServerConfiguration config = get_server_config(fd); /* Citeste configuratia de la server */

    char *menu[CONFIG_MENU_ITEMS] = {"Connection Status: ", "Maximum number of clients: ", "Back to main menu"};
    int hightlight = 0; /* Indexul optiuni selectate */

    while (1) {
        werase(adminPanel); /* Sterge continutul ferestrei */
        box(adminPanel, 0, 0); /* Creaza frame-ul din jurul meniului */

        /* Afisam meniul si informatiile */
        for (size_t i = 0; i < CONFIG_MENU_ITEMS; i++) {
            if (i == hightlight)
                wattron(adminPanel, A_REVERSE);
            if(i == 0)
                mvwprintw(adminPanel, i + 1, 1,"%s%s", menu[i], config.status);
            else if(i == 1)
                mvwprintw(adminPanel, i + 1, 1,"%s%d", menu[i], config.max_clients_number);
            else
                mvwprintw(adminPanel, i + 1, 1,"%s", menu[i]);
            wattroff(adminPanel, A_REVERSE);
        }

        wrefresh(adminPanel); /* Trimitem continutul pe ecran */
        int key = wgetch(adminPanel); /* Se asteapta o tasta si o stocheaza in alegere */

        switch (key) {
        case KEY_UP:
            hightlight--;
            if (hightlight <= -1)
                hightlight = 0;
            break;

        case KEY_DOWN:
            hightlight++;
            if (hightlight >= CONFIG_MENU_ITEMS)
                hightlight = CONFIG_MENU_ITEMS - 1;
            break;

        case ENTER_KEY:
            switch (hightlight) {
                case 0:
                    /* Comuta starea conexiuniilor si trimite noua configuratie la server */
                    if(strcmp(config.status, "OPEN") == 0)
                        memcpy(config.status, "CLOSED", strlen("CLOSED"));
                    else if(strcmp(config.status, "CLOSED") == 0)
                        memcpy(config.status, "OPEN", strlen("OPEN"));
                    send_server_configuration(fd, &config);
                break;

                case 1:
                    change_max_clients(adminPanel, fd, &config); /* Deschide fereastra pentru alegerea numarului maxim de clienti */
                    
                    wrefresh(adminPanel);
                    keypad(adminPanel, true);
                
                break;

                case 2:
                    return; /* Inapoi la meniul principal */
                break;

                default:
                break;
            }

            default:
            break;
        }
    }
}

void view_logs(WINDOW *adminPanel, int fd) {
    int logs_nr = 0;
    LogEntry *logs = get_logs(fd, &logs_nr); /* Alocam dinamic lista de filtre */
    int yMax, xMax;
    getmaxyx(stdscr, yMax, xMax);

    WINDOW *pad = newpad((logs_nr * ROWS_FOR_LOG) + 1 , PAD_WIDTH);  /* Facem un pad in interiorul ecranului pentru a putea face scroll */

    int position = 1; /* Pozitia scoll-ului in pad */
    while (1) {
        werase(adminPanel);
        box(adminPanel, 0, 0); /* Creaza frame-ul din jurul meniului */
        
        werase(pad); /* Sterge continutul padului inainte de a-l redesena */
        int y = 1;

        for (size_t i = 0; i < logs_nr; i++)
            mvwprintw(pad, y++, 0, "%s", logs[i].message);

        mvwhline(adminPanel, yMax / HALF_DIV - 3, 1, ACS_HLINE, PAD_WIDTH);
        mvwprintw(adminPanel, yMax / HALF_DIV - 2, 1, "Press X to go back to main menu");

        wrefresh(adminPanel);  /* Actualizeaza fereastra principala cu textul de mai sus */

        /* Aceasta afiseaza doar o portiune din pad pe ecran */
        prefresh(pad,
             position, 0,
             yMax / QUARTER_DIV + 1,
             xMax / QUARTER_DIV + 1,
             yMax / HALF_DIV + yMax / QUARTER_DIV - ROWS_FOR_FILTER - 1,
             xMax / QUARTER_DIV + PAD_WIDTH);

        int key = wgetch(adminPanel); /* Se asteapta o tasta si o stocheaza in alegere */

        if(position < 1)
            position = 1;

        switch (key){
            case KEY_UP:
                if (position > 1)
                    position -= ROWS_FOR_LOG; /* Deruleaza pad-ul in sus cu un filtru (2 lini) */
                
            break;

            case KEY_DOWN:
                position += ROWS_FOR_LOG; /* Deruleaza pad-ul in jos cu un filtru (2 lini) */
            break;
        
            case 'x':
                delwin(pad); /* Eliberam memoria pad-ului */
                free(logs); /* Eliberam memoria listei de filtre */
                clear(); /* Sterge tot ecranul */
                refresh(); /* Facem refresh la stdscr */
                return; /* Iesim din functie */
            break;

            default:
            break;
        }
    }
}

void server_usage(WINDOW *adminPanel, int fd) {
    SysInfo info = get_system_info(fd); /* Citeste informatiile de sistem de la server */
    char *menu[SERVER_USAGE_ITEMS] = {"CPU usage: ", "RAM usage: ", "Uptime: "};
    int yMax, xMax_unused;
    getmaxyx(stdscr, yMax, xMax_unused);
    (void)xMax_unused;

    while (1) {
        werase(adminPanel); /* Sterge continutul ferestrei */
        box(adminPanel, 0, 0); /* Creaza frame-ul din jurul meniului */

        /* Afisam meniul si informatiile */
        for (size_t i = 0; i < SERVER_USAGE_ITEMS; i++) {
            if(i == 0)
                mvwprintw(adminPanel, i + 1, 1,"%s%d%%", menu[i], info.CPU_usage);
            else if(i == 1)
                mvwprintw(adminPanel, i + 1, 1,"%s%ldMB / %ldMB", menu[i], info.RAM_usage, info.RAM_max);
            else if(i == 2)
                mvwprintw(adminPanel, i + 1, 1,"%s%s", menu[i], info.uptime);
        }

        mvwhline(adminPanel, yMax / HALF_DIV - 3, 1, ACS_HLINE, PAD_WIDTH);
        mvwprintw(adminPanel, yMax / HALF_DIV - 2, 1, "Press X to go back to main menu");

        wrefresh(adminPanel);  /* Actualizeaza fereastra principala cu textul de mai sus */

        int key = wgetch(adminPanel); /* Se asteapta o tasta si o stocheaza in alegere */

        switch (key) {
            case 'x':
                return;
            break;

            default:
            break;
        }
    }
}

/* Afiseaa un pop-up de confirmare inainte de a elimina un client */
int confirmation_window() {
    clear();
    refresh();

    noecho();    /* Nu adauga caractere unde este cursorul atunci cand apasam o tasta */
    cbreak();    /* Nu mai face buffer cu inputul de la user, deci nu mai trebuie apasat ENTER */
    curs_set(0); /* Face cursorul invizibil */

    int highlight = 0; /* 0 = Disconnect, 1 = Ban, 2 = Exit */

    int yMax, xMax;
    getmaxyx(stdscr, yMax, xMax);

    WINDOW *win = newwin(CONFIRMATION_WINDOW_LENGHT,
                         CONFIRMATION_WINDOW_WIDTH,
                         yMax / 2 - CONFIRMATION_WINDOW_LENGHT,
                         xMax / 2 - CONFIRMATION_WINDOW_WIDTH); /* Creaza noua fereastra pe ecran */

    box(win, 0, 0); /* Creaza frame-ul din jurul ferestrei */ 
    keypad(win, true); /* Permite utilizarea altor caractere (ne trebuie pentru sageti)*/

    const char *options[3] = {"Disconnect", "Ban", "Exit"};

    int position = 3;
    while (1) {
        werase(win); /* Sterge continutul ferestrei */
        box(win, 0, 0); /* Creaza frame-ul din jurul ferestrei */

        mvwprintw(win, 1, 2, "Select an action: ");

        /* Print options */
        for (int i = 0; i < 3; i++) {
            if (i == highlight) { /* Face REVERSE la optiunea selectata */
                wattron(win, A_REVERSE);
            }

            mvwprintw(win, position + i, 3, "%s", options[i]);

            if (i == highlight) {
                wattroff(win, A_REVERSE);
            }
        }

        wrefresh(win); /* Trimitem continutul pe ecran */

        int key = wgetch(win); /* Se asteapta o tasta si o stocheaza */

        switch (key) {
            case KEY_UP:
                highlight--;
                if (highlight < 0) highlight = 0;
                break;

            case KEY_DOWN:
                highlight++;
                if (highlight > 2) highlight = 2;
                break;

            case ENTER_KEY:
                delwin(win); /* Distruge fereastra */
                clear(); /* Curata ecranul complet pentru a elimina fereastra anterioara */
                refresh(); /* Curata ecranul complet pentru a elimina fereastra anterioara */

                /*
                   0 = Disconnect -> return 1
                   1 = Ban        -> return 2
                   2 = Exit       -> return 0
                */
                if (highlight == 0) return 1;
                if (highlight == 1) return 2;
                if (highlight == 2) return 0;

            default:
                break;
        }
    }

    return 0;
}

/* Afiseaza fereastra de alegerea a numarului maxim de clienti */
void change_max_clients(WINDOW *adminPanel, int fd, ServerConfiguration *config) {
    clear(); /* Curata ecranul complet pentru a elimina fereastra anterioara */
    refresh(); /* Curata ecranul complet pentru a elimina fereastra anterioara */
    int new_max = config->max_clients_number; /* Ia numarul maxim actual de clienti*/

    int yMax, xMax;
    getmaxyx(stdscr, yMax, xMax);
    WINDOW *win = newwin(MAX_CLIENTS_WINDOW_LENGHT, MAX_CLIENTS_WINDOW_WIDTH, yMax / 2 - MAX_CLIENTS_WINDOW_LENGHT, xMax / 2 - MAX_CLIENTS_WINDOW_WIDTH); /* Face o noua fereastra */
    box(win, 0, 0); /* Creaza frame-ul din jurul ferestrei */
    keypad(win, true); /* Permite utilizarea altor caractere (ne trebuie pentru sageti)*/

    while(1) {
        werase(win); /* Sterge continutul ferestrei */
        box(win, 0, 0); /* Creaza frame-ul din jurul ferestrei */

        mvwprintw(win, 1, 1,"What number do you desire: %d", new_max);
        
        mvwprintw(win, 3, 1,"Press UP/DOWN to change the number");
        mvwprintw(win, 4, 1,"Press ENTER to confirm, X to go back");

        wrefresh(win); /* Pune continutul pe ecran */
        int key = wgetch(win); /* Se asteapta o tasta si o stocheaza */

        switch (key) {
            case KEY_UP:
                new_max++; /* Creste numarul de clienti */
            break;

            case KEY_DOWN:
                if(new_max > 1)
                    new_max--; /* Scade numarul numarul de clienti daca este mai mare decat 1 */
            break;
            
            case ENTER_KEY:
                config->max_clients_number = new_max; /* Actualizeaza numarul maxim al structurii */
                send_server_configuration(fd, config); /* Trimite noua configuratie catre server */
                delwin(win); /* Distruge fereastra */
                clear(); /* Curata ecranul complet pentru a elimina fereastra anterioara */
                refresh(); /* Curata ecranul complet pentru a elimina fereastra anterioara */
                return;
            break;

            case 'x':
                delwin(win); /* Distruge fereastra */
                clear(); /* Curata ecranul complet pentru a elimina fereastra anterioara */
                refresh(); /* Curata ecranul complet pentru a elimina fereastra anterioara */
                return;
            break;

            default:
            break;
        }

    }
    return;
}

/* Creaza un socket UNIX, se conecteaza la server si returneaza file descriptor-ul conexiunii sau -1 in caz de eroare */
int connect_to_server() {
    /* Formam socketul */
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if(fd < 0) {
        perror("Socket creation");
        return -1;
    }

    struct sockaddr_un server_addr;
    server_addr.sun_family = AF_UNIX;
    memcpy(server_addr.sun_path, UNIXSOCKET, strlen(UNIXSOCKET) + 1); /* Copiaza calea socket-ului */

    /* Se incearca conectarea la server */
    if((connect(fd, (struct sockaddr *)&server_addr, sizeof(server_addr))) < 0) {
        perror("Server connection failed");
        close(fd);
        return -1;
    }

    return fd; /* Returneaza fd-ul */
}

/* Trimite un header de request catre server fara payload */
int send_requests(int fd, size_t request_type) {
    Header header = {request_type, 0, 0};
    if(send(fd, &header, sizeof(header), 0) < 0) {
        perror("Send request error");
        return -1;
    }
    return 0;
}

/* Returneaza lista de ClientInfo primita de la server */
ClientInfo *get_clients_info(int fd, int *count) {
    if(send_requests(fd, REQUEST_CLIENTS) < 0) /* Trimite REQUEST pentru clienti */
        return NULL;

    Header response; /* Headerul raspuns */
    if(recv(fd, &response, sizeof(Header), 0) < 0) { /* Citeste raspunsul */
        perror("Recv response error");
        return NULL;
    }

    if(response.type != SEND_CLIENTS || response.count < 0) /* Daca tipul nu este corect sau numarul este 0 */
        return NULL;
    
    ClientInfo *clients = malloc(sizeof(ClientInfo) * response.count); /* Alocam spatiu */

    size_t received = 0; /* Bytes primiti */
    size_t total = sizeof(ClientInfo) * response.count; /* Bytes totali */

    /* Citim toti bytes trimisi */
    while(received < total) {
        /* Citeste de unde a ramas */
        ssize_t n = recv(fd, (char *)clients + received, total - received, 0);
        if(n < 0) {
            perror("Recv clients error");
            free(clients);
            return NULL;
        }
        received = received + (size_t)n;
    }

    *count = response.count; /* Setam count ca numarul de clienti */
    return clients; /* Returnam array-ul */
}

/* Returneaza lista de Filter primita de la server */
Filter *get_filters_info(int fd, int *count) {

    if(send_requests(fd, REQUEST_FILTERS) < 0) /* Trimite REQUEST pentru filtre*/
        return NULL;

    Header response; /* Headerul raspuns */
    
    if(recv(fd, &response, sizeof(Header), 0) < 0) { /* Citeste raspunsul */
        perror("Recv response error");
        return NULL;
    }

    if(response.type != SEND_FILTERS || response.count < 0) /* Daca tipul nu este corect sau numarul este 0 */
        return NULL;
    
    Filter *filters = malloc(sizeof(Filter) * response.count); /* Alocam spatiu */
    
    size_t received = 0; /* Bytes primiti */
    size_t total = sizeof(Filter) * response.count; /* Bytes totali */

    /* Citim toti bytes trimisi */
    while(received < total) {
        /* Citeste de unde a ramas */
        ssize_t n = recv(fd, (char *)filters + received, total - received, 0);
        if(n < 0) {
            perror("Recv clients error");
            free(filters);
            return NULL;
        }
        received = received + (size_t)n;
    }

    *count = response.count; /* Setam count ca numarul de clienti */
    return filters; /* Returnam array-ul */
}

/* Permite eliminarea unui client dorit */
int kill_client(int fd, ClientInfo *client) {
    toKill kill;
    /* Plasam informatiile necesare ale clientului in structura toKill*/
    kill.job_id = client->job_id;
    for(size_t i = 0; i < PROCESS_COUNT; i++)
        kill.childrens[i] = client->P[i].pid;

    if(send_requests(fd, KILL_CLIENT) < 0) { /* Trimitem headerul de request pentru kill */
        perror("Kill request error");
        return -1;
    }

    if(send(fd, &kill, sizeof(kill), 0) < 0){ /* Trimitem obiectul toKill */
        perror("Kill send error");
        return -1;
    }
    return 0;
}

int ban_client(int fd, ClientInfo *client) {
    toKill ban;
    /* Plasam informatiile necesare ale clientului in structura toKill*/
    ban.job_id = client->job_id;
    for(size_t i = 0; i < PROCESS_COUNT; i++)
        ban.childrens[i] = client->P[i].pid;

    if(send_requests(fd, BAN_CLIENT) < 0) { /* Trimitem headerul de request pentru ban */
        perror("Ban request error");
        return -1;
    }

    if(send(fd, &ban, sizeof(ban), 0) < 0){ /* Trimitem obiectul toKill */
        perror("Kill send error");
        return -1;
    }
    return 0;
}

/* Permite trimiterea configuratiei catre server */
int send_server_configuration(int fd, ServerConfiguration *config) {
    if(send_requests(fd, SEND_SERVER_CONFIGURATION) < 0) /* Trimitem REQUEST de trimitere a configuratiei */
        return -1;

    if(send(fd, config, sizeof(ServerConfiguration), 0) < 0) { /* Trimitem configuratia */
        perror("Sending configuration error");
        return -1;
    }
    return 0;
}

/* Returneaza lista de ClientInfo primita de la server */
LogEntry *get_logs(int fd, int *count) {
    if(send_requests(fd, REQUEST_LOGS) < 0) /* Trimite REQUEST pentru clienti */
        return NULL;

    Header response; /* Headerul raspuns */
    if(recv(fd, &response, sizeof(Header), 0) < 0) { /* Citeste raspunsul */
        perror("Recv response error");
        return NULL;
    }

    if(response.type != SEND_LOGS || response.count < 0) /* Daca tipul nu este corect sau numarul este 0 */
        return NULL;
    
    LogEntry *logs = malloc(sizeof(LogEntry) * response.count); /* Alocam spatiu */

    size_t received = 0; /* Bytes primiti */
    size_t total = sizeof(LogEntry) * response.count; /* Bytes totali */

    /* Citim toti bytes trimisi */
    while(received < total) {
        /* Citeste de unde a ramas */
        ssize_t n = recv(fd, (char *)logs + received, total - received, 0);
        if(n < 0) {
            perror("Recv clients error");
            free(logs);
            return NULL;
        }
        received = received + (size_t)n;
    }

    *count = response.count; /* Setam count ca numarul de clienti */
    return logs; /* Returnam array-ul */
}

SysInfo get_system_info(int fd) {
    SysInfo info = {0, 0, 0, "Server is UP for 0d 0h 0min 0sec"};
    if(send_requests(fd, REQUEST_SYS_INFO) < 0) /* Trimitem REQUEST pentru informatiile de sistem */
        return info;

    Header response; /* Headerul de raspuns */
    
    if(recv(fd, &response, sizeof(Header), 0) < 0) { /* Citim raspunsul */
        perror("Recv response error");
        return info;
    }

    if(response.type != SEND_SYS_INFO) /* Daca tipul raspunsului nu este corect */
        return info;

    if(recv(fd, &info, sizeof(SysInfo), 0) < 0) { /* Citim informatiile de sistem */
        perror("Recv config error");
        return info;
    }
    return info;
}

/* Returneaza structura cu configuratia serverului */
ServerConfiguration get_server_config(int fd) {
    ServerConfiguration config = {"CLOSED", 0}; /* Facem o configurare de baza */
    if(send_requests(fd, REQUEST_SERVER_CONFIGURATION) < 0) /* Trimitem REQUEST pentru configuratie */
        return config;

    Header response; /* Headerul de raspuns */
    
    if(recv(fd, &response, sizeof(Header), 0) < 0) { /* Citim raspunsul */
        perror("Recv response error");
        return config;
    }

    if(response.type != SEND_SERVER_CONFIGURATION) /* Daca tipul raspunsului nu este corect */
        return config;
    
    if(recv(fd, &config, sizeof(ServerConfiguration), 0) < 0) { /* Citim configuratia */
        perror("Recv config error");
        return config;
    }
    return config;
}

/* Verifica daca serverul a trimis notificare de timeout (cu recv non-blocant) */
int check_timeout_notification(int fd) {
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 0;  // timeout 0 pentru verificare non-blocanta
    
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(fd, &readfds);
    
    // verifica daca sunt date disponibile fara a bloca
    int ready = select(fd + 1, &readfds, NULL, NULL, &tv);
    if (ready > 0) {
        Header notif;
        ssize_t n = recv(fd, &notif, sizeof(Header), MSG_PEEK); // citeste fara a consuma
        if (n > 0 && notif.type == ADMIN_TIMEOUT_DISCONNECT) {
            recv(fd, &notif, sizeof(Header), 0); // consuma mesajul
            return 1; // timeout detectat
        }
    }
    return 0; // nu e timeout
}