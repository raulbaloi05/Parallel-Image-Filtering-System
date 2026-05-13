================================================================================
   PIF — PARALLEL IMAGE FILTERING
   GHID COMPLET PENTRU INCEPATORI
================================================================================

Acest document explica de la zero cum functioneaza intregul proiect.
Nu necesita cunostinte prealabile de retele sau sisteme de operare.


--------------------------------------------------------------------------------
0. CE FACE PROIECTUL (pe scurt)
--------------------------------------------------------------------------------

Proiectul implementeaza un sistem client-server care aplica filtre pe imagini.

Ideea simpla:
  1. Clientul trimite o imagine (JPEG/PNG) catre server.
  2. Serverul imparte imaginea in 4 bucati si le proceseaza IN PARALEL.
  3. Serverul trimite inapoi imaginea filtrata.
  4. Clientul salveaza rezultatul pe disc.

Filtrele disponibile:
  - grayscale  → transforma imaginea in alb/negru
  - blur       → face imaginea neclara (cetoasa)
  - sharpen    → accentueaza detaliile
  - edge       → detecteaza contururile (marginile obiectelor)
  - negative   → inverseaza culorile (ca un negativ foto)


--------------------------------------------------------------------------------
1. STRUCTURA DIRECTORULUI
--------------------------------------------------------------------------------

make/
├── server/              ← codul serverului
│   ├── server.c         ← serverul principal (SOAP/HTTP)
│   ├── unix_server.c    ← server secundar (socket UNIX, pentru admin)
│   ├── processing.c     ← procesarea efectiva a imaginilor
│   ├── admin.c          ← panoul de administrare (TUI cu ncurses)
│   ├── dataTypes.h      ← tipurile de date comune server/admin
│   ├── processing.h     ← header pentru processing.c
│   └── pif.h / soapH.h  ← cod generat automat de gSOAP (nu modifica manual)
│
├── clients/
│   ├── client/          ← clientul in linie de comanda (C)
│   │   ├── main.c       ← punctul de intrare al clientului
│   │   ├── client.c     ← operatiile de retea (connect, filter, bye)
│   │   ├── config.c     ← incarcarea configuratiei
│   │   ├── proto.c      ← protocolul de comunicatie de nivel jos
│   │   └── client.cfg   ← fisierul de configuratie
│   │
│   └── web/             ← clientul web (browser)
│       ├── index.html   ← interfata grafica
│       ├── scripts/
│       │   ├── app.js   ← logica UI (butoane, preview, drag&drop)
│       │   └── soap.js  ← comunicarea cu serverul (SOAP over HTTP)
│       └── assets/
│           └── style.css← stilizarea interfetei
│
├── build/               ← fisierele compilate (generate de CMake, nu modifica)
├── CMakeLists.txt       ← configuratia de compilare pentru intregul proiect
└── GHID_PROIECT.txt     ← acest fisier


--------------------------------------------------------------------------------
2. ARHITECTURA GENERALA (cine vorbeste cu cine)
--------------------------------------------------------------------------------

                      ┌─────────────────────────────────────┐
                      │             SERVER                  │
                      │                                     │
    ┌──────────┐  TCP  │  ┌──────────────────────────────┐  │
    │ Client C │──────►│  │  Thread SOAP (port 18082)    │  │
    └──────────┘  HTTP │  │  Asculta cereri SOAP/HTTP    │  │
                      │  └──────────────┬─────────────────┘  │
    ┌──────────┐  TCP  │                │                     │
    │ Client   │──────►│         ┌──────▼──────┐             │
    │   Web    │  HTTP │         │ global_state│             │
    └──────────┘       │         │ (mutex)     │             │
                      │         └──────┬──────┘             │
                      │                │                     │
                      │  ┌─────────────▼──────────────────┐ │
    ┌──────────┐UNIX   │  │ Thread UNIX socket             │ │
    │  Admin   │──────►│  │ (fisier /tmp/unixds)           │ │
    │   TUI    │socket │  │ Asculta comenzi de la admin    │ │
    └──────────┘       │  └────────────────────────────────┘ │
                      └─────────────────────────────────────┘

Explicatie:
  - Clientul C si Clientul Web comunica cu serverul prin SOAP (un protocol
    bazat pe XML, trimis prin HTTP pe portul 18082).
  - Admin-ul comunica cu serverul printr-un UNIX socket (un "pipe" local,
    mai rapid, folosit doar pe aceeasi masina). Adminul NU merge prin retea.
  - Serverul are o singura stare globala (global_state) protejata de un mutex
    pentru a evita conflictele intre threaduri.


--------------------------------------------------------------------------------
3. CE ESTE SOAP? (explicatie pentru incepatori)
--------------------------------------------------------------------------------

SOAP (Simple Object Access Protocol) este un protocol de comunicatie.
Clientul trimite un mesaj XML catre server, serverul proceseaza si trimite
inapoi tot un XML.

  [ DE CE SOAP si nu REST/JSON? ]
  Cerinta proiectului impune explicit SOAP (MilestoneA.txt). In plus,
  gSOAP genereaza automat tot codul de serializare/deserializare din
  fisierul de definitie pif.h — nu scriem manual parsare XML. Cu REST
  am fi scris manual tot protocolul.

Exemplu de mesaj trimis de client catre server (cerere connect):

  <?xml version="1.0" encoding="UTF-8"?>
  <SOAP-ENV:Envelope xmlns:SOAP-ENV="...">
    <SOAP-ENV:Body>
      <ns1:connect>
      </ns1:connect>
    </SOAP-ENV:Body>
  </SOAP-ENV:Envelope>

Serverul raspunde:
  <connect>4231</connect>   ← clientID-ul unic pentru aceasta sesiune

In proiect, codul SOAP (soapH.h, soapC.c, soapServer.c, soapClient.c) este
GENERAT AUTOMAT de un tool numit gSOAP din fisierul de definitie pif.h.
Tu nu scrii acel cod — il genereaza gSOAP din descrierea interfetei.


--------------------------------------------------------------------------------
4. SERVERUL — server.c
--------------------------------------------------------------------------------

Serverul expune 5 operatii (endpoint-uri) prin SOAP:

  a) ns__connect(req, resp)
     ─────────────────────
     Clientul se conecteaza. Serverul genereaza un ID unic (1-10000)
     si il inregistreaza in lista de clienti activi.
     Returneaza: clientID (int)

     [ DE CE ID random 1-10000 si nu un contor simplu 1,2,3...? ]
     Un contor ar reutiliza ID-uri dupa deconectare. Daca un client
     vechi trimite inca mesaje cu ID=3 dupa ce s-a deconectat, un nou
     client cu ID=3 ar primi raspunsurile gresite. ID-ul random reduce
     sansa de coliziune fara a necesita persistenta pe disc.

     De retinut: daca serverul e "CLOSED" (inchis de admin) sau a
     atins limita de clienti (max_clients_number), refuza conexiunea.

  b) ns__echo(req, resp)
     ────────────────────
     Primeste un string, returneaza acelasi string.
     Folosit pentru a verifica daca serverul este online (ping).

  c) ns__applyFilter(req, resp)
     ───────────────────────────
     Operatia principala. Primeste:
       - imageData: imaginea ca bytes bruti
       - filterType: "grayscale", "blur", etc.
       - clientId: ID-ul sesiunii

     Apeleaza process_image() din processing.c, masoara timpul,
     returneaza imaginea procesata + timpul de procesare in ms.

  d) ns__bye(req, resp)
     ───────────────────
     Clientul se deconecteaza. Serverul sterge clientul din lista activa.

  e) ns__serverInfo(req, resp)
     ──────────────────────────
     Returneaza: numarul de clienti activi, statusul serverului
     (OPEN/CLOSED), etc.

Serverul are 2 threaduri:
  - Threadul principal ruleaza SOAP (bucla soap_accept → soap_serve)
  - Un thread separat (unix_main) ruleaza socket-ul UNIX pentru admin

  [ DE CE soap->keep_alive = 0? ]
  Cu keep-alive activ, soap_serve() intra intr-o bucla interna si
  asteapta urmatoarea cerere de la acelasi client inainte sa returneze.
  Serverul nostru e single-threaded pe SOAP — un singur client cu
  keep-alive ar bloca toti ceilalti clienti la nesfarsit.
  keep_alive = 0 forteaza inchiderea conexiunii dupa fiecare cerere.

  [ DE CE (void)req in ns__connect si ns__serverInfo? ]
  gSOAP impune semnatura functiei cu parametrul req. Aceste endpoint-uri
  nu au nevoie de date din cerere (connect nu primeste nimic, serverInfo
  nu are parametri de filtrare). (void) suprima warning-ul de compilator
  fara sa schimbe comportamentul.


--------------------------------------------------------------------------------
5. PROCESAREA IMAGINILOR — processing.c
--------------------------------------------------------------------------------

Aceasta este "inima" proiectului. Foloseste GraphicsMagick si fork().

Pasii procesarii:

  STEP 1: Primeste imaginea ca vector de bytes (blob)
          BlobToImage() → converteste bytes in structura Image

  STEP 2: Imparte imaginea in 4 zone (2x2):
          ┌────┬────┐
          │ 0  │ 1  │  zona 0: x=0,     y=0
          ├────┼────┤  zona 1: x=w/2,   y=0
          │ 2  │ 3  │  zona 2: x=0,     y=h/2
          └────┴────┘  zona 3: x=w/2,   y=h/2

  STEP 3: Creeaza 4 procese copil cu fork()

          Ce este fork()?
          fork() duplica procesul curent. Copilul primeste o copie a
          memoriei parintelui. Parintele si copilul ruleaza IN PARALEL.
          Copilul are pids[i] == 0, parintele are pids[i] == PID-ul copilului.

          [ DE CE fork() si nu thread-uri (pthread)? ]
          GraphicsMagick nu e thread-safe — mutex-urile interne se pot
          corupe daca mai multe threaduri apeleaza simultan functii de
          procesare pe aceeasi imagine. fork() creeaza procese separate
          cu memorie COMPLET separata: zero conflict, zero mutex partajat.
          Trade-off: overhead mai mare si comunicare prin /tmp in loc de
          memorie comuna.

  STEP 4: Fiecare copil:
          - Extrage zona sa din imagine (CropImage)
          - Aplica filtrul pe zona sa
          - Salveaza zona procesata in /tmp/part_PID_i.png
          - Se termina cu _exit(0)

          [ DE CE _exit(0) si nu exit(0)? ]
          exit() ruleaza toti handlerii atexit() si face flush la bufferele
          stdio — inclusiv cele COPIATE de la parinte la fork(). Asta poate
          produce dublu-flush (datele parintelui scrise de doua ori) si
          corupe mutex-urile interne GraphicsMagick. _exit() termina direct,
          fara niciun cleanup, evitand aceste probleme.

          [ DE CE /tmp pentru fisierele intermediare? ]
          Procesele copil au memorie separata de parinte — nu pot returna
          direct date prin variabile. /tmp e filesystem in RAM pe Linux
          (tmpfs), deci scrierea/citirea e rapida. Fisierele sunt sterse
          imediat dupa asamblare.

  STEP 5: Parintele asteapta toti copiii cu waitpid()
          In acest timp citeste statistici din /proc/PID/stat (CPU, RAM)

          [ DE CE citim /proc/PID/stat INAINTE de waitpid()? ]
          waitpid() recolteaza (sterge) procesul copil din sistem.
          Dupa waitpid(), /proc/PID/ nu mai exista — PID-ul e eliberat.
          Trebuie sa citim statisticile cat timp copilul inca exista.

  STEP 6: Parintele asambleaza imaginea finala:
          - Citeste cele 4 zone de pe disc
          - Le "lipeste" la pozitiile corecte cu CompositeImage()

  STEP 7: Converteste rezultatul inapoi in bytes cu ImageToBlob()
          Returneaza blob-ul catre server.c care il trimite clientului.


--------------------------------------------------------------------------------
6. ADMINUL — admin.c
--------------------------------------------------------------------------------

Un program separat (pif-admin sau similar) care ofera o interfata TUI
(Terminal User Interface) folosind biblioteca ncurses.

Communica cu serverul prin UNIX socket (nu prin retea), deci trebuie
rulat pe aceeasi masina cu serverul.

Permite:
  - Vizualizarea clientilor conectati (IP, Job ID, PID-urile workerilor,
    CPU%, RAM%)
  - Deconectarea fortata a unui client (KILL_CLIENT — trimite SIGKILL
    catre procesele copil ale clientului)
  - Vizualizarea filtrelor si de cate ori au fost folosite
  - Deschiderea/inchiderea conexiunilor noi (CHANGE_CONECTIONS)
  - Setarea numarului maxim de clienti
  - Vizualizarea log-urilor de activitate
  - Vizualizarea informatiilor de sistem (CPU total, RAM, uptime)

Protocolul UNIX socket foloseste o structura Header cu un camp "type"
care indica ce date urmeaza (REQUEST_CLIENTS, SEND_FILTERS, etc.).
Toate mesajele sunt structuri C serializate direct (nu XML, nu JSON).

  [ DE CE UNIX socket si nu TCP pentru admin? ]
  Adminul ruleaza pe aceeasi masina cu serverul — nu are nevoie de retea.
  UNIX socket: fara overhead TCP/IP, fara handshake, acces controlat
  prin permisiuni de fisier (nu oricine din retea poate accesa).
  Un atacator din exterior nu poate trimite comenzi de kill/close
  chiar daca portul SOAP 18082 ar fi expus public.

  [ DE CE structuri C serializate direct (nu XML/JSON)? ]
  Adminul si serverul sunt scrise in acelasi C, pe aceeasi masina,
  cu acelasi compilator. Nu e nevoie de un format portabil — trimiterea
  directa a struct-urilor e mai simpla si mai rapida. Daca am folosi
  JSON am adauga o dependenta externa doar pentru comunicarea locala.


--------------------------------------------------------------------------------
7. CLIENTUL C — clients/client/
--------------------------------------------------------------------------------

Un program de linie de comanda interactiv.

Configuratia se incarca in 4 straturi (fiecare suprascrie pe cel anterior):
  1. Valori implicite (hardcodate in config.c)
  2. Variabile de mediu (PIF_SERVER_HOST, PIF_FILTER, etc.)
  3. Fisierul client.cfg (sintaxa libconfig)
  4. Argumente CLI (-h host -p port -F filtru -i input -o output -v)

  [ DE CE 4 straturi si nu un singur mecanism? ]
  Conventie standard UNIX: CLI bate fisierul, fisierul bate env-ul,
  env-ul bate default-urile. Permite: rulare fara configuratie (default),
  suprascrierea in CI/CD fara a edita fisiere (env vars), configurare
  persistenta intre rulari (client.cfg), override rapid o singura data (CLI).

  [ DE CE libconfig si nu un fisier .ini sau JSON? ]
  Cerinta proiectului impune libconfig explicit (MilestoneA.txt: mandatory).
  Sintaxa key = value; e curata, suporta tipuri native (int, string, bool)
  si e mai usor de citit decat JSON pentru un fisier de configuratie.

Fluxul de executie:
  1. Incarca configuratia
  2. Creeaza context SOAP (soap_new)
  3. client_connect() → trimite mesaj SOAP "connect", primeste clientID
  4. Bucla interactiva:
       pif> blur input.jpg output.jpg
       pif> grayscale foto.jpg rezultat.jpg
       pif> exit
  5. La "exit": client_bye() → trimite SOAP "bye"
  6. Curata contextul SOAP si iese

Fisierele SOAP generate (soapClient.c, soapC.c, soapH.h, soapStub.h)
contin codul de serializare/deserializare XML generat de gSOAP.
Nu le modifica manual.

  [ DE CE soap->keep_alive = 0 si in client? ]
  Identic cu motivul de la server: evita bucle interne in gSOAP si
  asigura ca fiecare cerere e independenta. Fara asta, clientul ar
  putea ramane blocat asteptand o conexiune care a expirat pe server.





--------------------------------------------------------------------------------
8. STAREA GLOBALA A SERVERULUI — dataTypes.h
--------------------------------------------------------------------------------

Serverul tine totul intr-o structura globala:

  ServerState {
    clients[]           ← lista clientilor activi (max 10 implicit)
    active_clients_count← cati clienti sunt conectati acum
    filters[]           ← cele 5 filtre + de cate ori au fost folosite
    logs[]              ← istoric evenimente (connect, disconnect, filtrare)
    log_count           ← cate log-uri exista
    start_time          ← cand a pornit serverul (pentru calcul uptime)
    config {
      status            ← "OPEN" sau "CLOSED"
      max_clients_number← limita de clienti simultani
    }
  }

Fiecare client activ are:
  ClientInfo {
    job_id    ← ID-ul unic al sesiunii
    ip        ← adresa IP a clientului
    P[4]      ← informatii despre cele 4 procese copil (PID, CPU%, RAM%, status)
  }

Accesul la global_state este protejat de state_mutex (pthread_mutex_t).
Asta inseamna: inainte de orice citire/scriere, codul face
pthread_mutex_lock() si dupa pthread_mutex_unlock(). Fara asta, doua
threaduri ar putea modifica aceeasi data simultan → date corupte.

  [ DE CE un singur mutex pentru toata starea? ]
  Alternativa ar fi un mutex per camp (unul pentru clients[], unul pentru
  logs[], etc.) — mai multa paralelism dar risc de deadlock daca doua
  threaduri iau mutex-uri in ordine diferita. Cu un singur mutex, codul
  e simplu si corect: cel mult un thread in sectiunea critica odata.
  La volumul de clienti al acestui proiect (max DEFAULT_MAX_CLIENTS=10)
  nu exista bottleneck de performanta.

  [ DE CE constante cu nume (IP_LEN, STATUS_LEN) in loc de numere directe? ]
  Daca IP_LEN=16 e scris de 10 ori in cod si trebuie schimbat la 46 pentru
  IPv6, trebuie cautate si modificate toate aparitiile. Cu constanta,
  modifici un singur #define. In plus, clang-tidy (readability-magic-numbers)
  trateaza numerele hardcodate ca erori.

  [ DE CE memcpy/snprintf in loc de strcpy? ]
  strcpy nu verifica limita bufferului destinatie — poate scrie dincolo
  de sfarsitul array-ului (buffer overflow). clang-tidy blocheaza strcpy
  prin clang-analyzer-security.insecureAPI.strcpy. memcpy cu sizeof()
  cunoscut la compile-time e sigur; snprintf cu limita explicita e sigur
  pentru string-uri de lungime variabila.


--------------------------------------------------------------------------------
9. COMPILARE SI RULARE
--------------------------------------------------------------------------------

Proiectul foloseste CMake pentru compilare.

Pasi:
  cd make/
  mkdir -p build && cd build
  cmake ..
  make

Executabile generate:
  build/server/pif_server       ← serverul principal
  build/server/admin            ← panoul de administrare
  build/clients/client/pif-client ← clientul C

Rulare:

  1. Porneste serverul (intr-un terminal):
       ./build/server/pif_server
     Output asteptat:
       Server started with UNIX socket and SOAP threads
       [UNIX Thread] Unix Socket Server running on ...
       [SOAP Thread] Starting on port 18082...

  2. Ruleaza clientul C (in alt terminal):
       ./build/clients/client/pif-client
     Sau cu parametri:
       ./build/clients/client/pif-client -h 127.0.0.1 -F blur -i foto.jpg -o out.jpg

  3. Ruleaza adminul (optional, pe aceeasi masina cu serverul):
       ./build/server/admin

  4. Clientul web: deschide clients/web/index.html in browser.

Dependinte necesare:
  - libgsoap-dev    (pentru SOAP)
  - libgraphicsmagick1-dev (pentru procesarea imaginilor)
  - libncurses-dev  (pentru adminul TUI)
  - libconfig-dev   (pentru fisierul client.cfg)

Pe Ubuntu/Debian:
  sudo apt install libgsoap-dev libgraphicsmagick1-dev libncurses-dev libconfig-dev


--------------------------------------------------------------------------------
10. CLIENTUL WEB — clients/web/
--------------------------------------------------------------------------------

Interfata grafica accesibila din browser, fara instalare.

Fisiere:
  - index.html   ← structura paginii (upload, selectie filtru, preview)
  - scripts/app.js  ← logica UI: butoane, drag&drop, afisare rezultat
  - scripts/soap.js ← construieste si trimite mesaje SOAP (XML over HTTP)
  - assets/style.css← stilizarea vizuala

Cum functioneaza:
  1. Utilizatorul incarca o imagine si alege filtrul
  2. app.js apeleaza soap.js pentru a trimite cererea SOAP catre server
  3. Serverul proceseaza si returneaza imaginea filtrata ca base64
  4. app.js afiseaza rezultatul si ofera optiunea de download

Configurare server URL: editeaza scripts/soap.js, linia:
  serverUrl: 'http://localhost:18082'

Nu necesita server web separat — functioneaza direct din sistemul de fisiere
(file://) sau dintr-un server HTTP simplu (python3 -m http.server).


--------------------------------------------------------------------------------
11. PENTRU LAN (retea locala)
--------------------------------------------------------------------------------

Implicit serverul asculta pe toate interfetele (NULL in soap_bind = 0.0.0.0).
Clientii din LAN pot accesa serverul prin IP-ul masinii unde ruleaza serverul.

Cum sa afli IP-ul serverului (pe Linux):
  ip addr show   sau   hostname -I

Configureaza clientul C sa foloseasca IP-ul serverului:
  Metoda 1 — fisier client.cfg:
    server { host = "192.168.1.10"; port = 18082; }

  Metoda 2 — CLI:
    ./pif-client -h 192.168.1.10 -p 18082

  Metoda 3 — variabila de mediu:
    PIF_SERVER_HOST=192.168.1.10 ./pif-client

Clientul web: editeaza clients/web/scripts/soap.js, linia:
  serverUrl: 'http://localhost:18082'
Schimba 'localhost' cu IP-ul serverului.

Adminul NU merge prin retea (UNIX socket = doar local).

Firewall: asigura-te ca portul 18082 TCP este deschis pe masina server.
  sudo ufw allow 18082/tcp


--------------------------------------------------------------------------------
12. FLUXUL COMPLET AL UNUI REQUEST (de la click pana la imagine procesata)
--------------------------------------------------------------------------------

  [Browser/Client]          [Server SOAP Thread]       [processing.c]
  ─────────────────────────────────────────────────────────────────────
  1. Utilizatorul selecteaza
     o imagine si apasa
     "Proceseaza"
        │
  2. app.js apeleaza
     SOAP.connect()
     ─── XML SOAP ──────────► ns__connect()
                               Genereaza ID=4231
     ◄── clientID=4231 ───────
        │
  3. SOAP.applyFilter(
       base64, "blur", 4)
     ─── XML SOAP ──────────► ns__applyFilter()
                               Apeleaza process_image()
                                        │
                               ┌────────▼────────────┐
                               │ BlobToImage()        │
                               │ Imparte in 4 zone    │
                               │ fork() x4            │
                               │  copil0: blur zona0  │
                               │  copil1: blur zona1  │
                               │  copil2: blur zona2  │
                               │  copil3: blur zona3  │
                               │ waitpid() x4         │
                               │ Asambleaza imaginea  │
                               │ ImageToBlob()        │
                               └────────┬────────────┘
                               Returneaza blob + timp
     ◄── imagine procesata ───
        │
  4. app.js afiseaza
     rezultatul in browser
     si ofera optiunea
     de download
        │
  5. SOAP.bye()
     ─── XML SOAP ──────────► ns__bye()
                               Sterge clientul din lista


--------------------------------------------------------------------------------
13. GLOSAR TERMENI TEHNICI
--------------------------------------------------------------------------------

SOAP        ── protocol de comunicatie bazat pe XML, trimis prin HTTP
gSOAP       ── biblioteca C care implementeaza SOAP; genereaza cod din definitii
fork()      ── apel de sistem Linux care creeaza un proces copil (duplica procesul)
waitpid()   ── parintele asteapta terminarea unui proces copil specific
pthread     ── biblioteca pentru threaduri in C (POSIX threads)
mutex       ── mecanism de blocare: garanteaza ca doar un thread acceseaza
               o resursa la un moment dat
UNIX socket ── canal de comunicatie intre procese pe aceeasi masina (nu retea)
blob        ── Binary Large OBject; vector de bytes (de ex: o imagine)
GraphicsMagick ── biblioteca C pentru procesarea imaginilor (fork din ImageMagick)
ncurses     ── biblioteca pentru interfete TUI (text) in terminal
libconfig   ── biblioteca pentru fisiere de configuratie cu sintaxa key = value
CMake       ── sistem de build: genereaza Makefile-uri din CMakeLists.txt
base64      ── codificare: converteste bytes binari in caractere ASCII
              (folosit pentru a trimite imagini in XML/JSON)
endpoint    ── o functie expusa prin retea (ex: ns__applyFilter = endpoint SOAP)
thread      ── "fir de executie" in cadrul aceluiasi proces; partajeaza memoria
process     ── program in executie; are memorie separata de alte procese

================================================================================
   Sfarsit ghid. Bafta!
================================================================================