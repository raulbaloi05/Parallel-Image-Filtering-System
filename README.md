# PIF — Parallel Image Filtering

Sistem client-server care aplică filtre pe imagini, **procesându-le în paralel** (4 procese simultan).

1. Clientul trimite o imagine (JPEG/PNG) către server
2. Serverul împarte imaginea în 4 zone și le procesează în paralel
3. Serverul trimite înapoi imaginea filtrată
4. Clientul o salvează / o afișează

| Filtru | Efect |
|--------|-------|
| `grayscale` | Alb-negru |
| `blur` | Încețoșare |
| `sharpen` | Accentuare detalii |
| `edge` | Detectare contururi |
| `negative` | Inversare culori |

> Proiectul rulează pe **Linux** (sau **WSL** pe Windows). Folosește `fork()`, socket-uri UNIX și gSOAP — nu compilează nativ pe Windows.

---

## 🚀 Rulare rapidă (prima oară)

Toate comenzile se rulează într-un terminal Linux/WSL, din directorul proiectului.

### Pasul 1 — Instalează dependențele (o singură dată)

```bash
# Ubuntu / Debian / WSL
sudo apt install build-essential cmake libgsoap-dev gsoap \
                 libgraphicsmagick1-dev libncurses-dev libconfig-dev

# Fedora / RHEL
sudo dnf install gcc cmake gsoap-devel GraphicsMagick-devel ncurses-devel libconfig-devel
```

### Pasul 2 — Generează codul gSOAP (o singură dată)

Fișierele `soapC.c`, `soapServer.c`, `soapClient.c` **nu sunt în repo** — le generează `soapcpp2` din definițiile `pif.h`:

```bash
cd server && soapcpp2 -c -S pif.h && cd ..
cd clients/client && soapcpp2 -c -C pif.h && cd ../..
```

> Repetă pasul doar dacă modifici `pif.h`.

### Pasul 3 — Compilează

```bash
mkdir -p build && cd build
cmake ..
make
cd ..
```

Rezultă 4 executabile:

| Executabil | Rol |
|------------|-----|
| `build/server/pif_server` | Serverul principal |
| `build/server/admin` | Panou de administrare (TUI) |
| `build/clients/client/pif-client` | Client SOAP în linie de comandă |
| `build/clients/tcp_client/pif-tcp-client` | Client TCP (transfer binar rapid) |

### Pasul 4 — Pornește serverul

```bash
./build/server/pif_server
```

Lasă terminalul deschis. Serverul ascultă pe:
- **18082** — SOAP/HTTP (clientul C și clientul web)
- **18083** — TCP binar (clientul TCP)

### Pasul 5 — Folosește un client

**Varianta cea mai simplă — clientul web.** Într-un al doilea terminal:

```bash
cd clients/web
python3 -m http.server 8780
```

Deschide în browser: **http://localhost:8780**

1. Apasă **Conectare** (adresa serverului e deja completată)
2. Trage o imagine în zona de upload (sau dă click pe ea)
3. Alege un filtru
4. Apasă **Procesează** — rezultatul apare în dreapta
5. **Descarcă rezultatul** dacă vrei fișierul

Gata. 🎉

---

## Ceilalți clienți

### Client TCP (o singură comandă, bun pentru imagini mari)

```bash
./build/clients/tcp_client/pif-tcp-client 127.0.0.1 18083 blur poza.jpg rezultat.jpg
```

### Client SOAP în linie de comandă (interactiv)

```bash
./build/clients/client/pif-client -h 127.0.0.1 -p 18082
```

La promptul `pif>` scrie:

```
grayscale poza.jpg rezultat.jpg
```

(`help` pentru lista de comenzi.) Parametrii se pot da și prin `clients/client/client.cfg` sau variabila de mediu `PIF_SERVER_HOST`.

### Panoul de administrare

```bash
./build/server/admin
```

Interfață în terminal (ncurses) pentru monitorizare și control:

| Tastă | Acțiune |
|-------|---------|
| `o` | Deschide serverul (acceptă clienți noi) |
| `c` | Închide serverul (refuză clienți noi) |
| `r` | Refresh manual |
| `q` | Ieșire |

> Adminul comunică prin socket UNIX (`/tmp/unixds`) → funcționează **doar pe aceeași mașină** cu serverul.

---

## Probleme frecvente

| Problemă | Cauză / Soluție |
|----------|-----------------|
| `soapcpp2: command not found` | Lipsește pachetul `gsoap` — vezi Pasul 1 |
| CMake: `soapC.c not found` | Nu ai rulat Pasul 2 (generarea gSOAP) |
| Browserul arată altceva pe portul ales | Portul e ocupat de altă aplicație — schimbă portul (ex. `python3 -m http.server 8790`) |
| `Connection refused` în client | Serverul nu rulează — pornește Pasul 4 |
| Clientul web nu se conectează din alt PC | Schimbă adresa din câmpul de sus în `http://IP-UL-SERVERULUI:18082` și deschide portul în firewall |

---

## Cum funcționează (pe scurt)

```
                      ┌─────────────────────────────────────┐
                      │              SERVER                 │
   ┌────────────┐SOAP │  ┌────────────────────────────┐     │
   │ Client C / │─────►  │ Thread SOAP  (port 18082)  │     │
   │ Client Web │ HTTP│  └─────────────┬──────────────┘     │
   └────────────┘     │                │                    │
   ┌────────────┐ TCP │  ┌─────────────▼──────────────┐     │
   │ Client TCP │─────►  │ Thread TCP   (port 18083)  │     │
   └────────────┘     │  └─────────────┬──────────────┘     │
                      │         ┌──────▼──────┐             │
                      │         │ global_state│ (mutex)     │
                      │         └──────┬──────┘             │
   ┌────────────┐UNIX │  ┌─────────────▼──────────────┐     │
   │ Admin TUI  │─────►  │ Thread UNIX (/tmp/unixds)  │     │
   └────────────┘socket  └────────────────────────────┘     │
                      └─────────────────────────────────────┘
```

- **4 thread-uri** în server: SOAP, TCP, UNIX socket și worker-ul cozii de joburi; toate partajează `global_state`, protejat de un mutex.
- **Clientul C** și **clientul web** vorbesc SOAP (XML peste HTTP). **Clientul TCP** folosește un protocol binar propriu — fără overhead base64, potrivit pentru imagini mari.
- Serverul are suport **CORS** încorporat → clientul web funcționează direct din browser.

### Procesarea în paralel

Funcția `process_image()` din `server/processing.c`:

```
1. BlobToImage()   → decodifică imaginea (GraphicsMagick)
2. fork() × 4      → 4 procese copii, fiecare cu propria zonă:
                     stânga-sus, dreapta-sus, stânga-jos, dreapta-jos
3. fiecare copil   → aplică filtrul pe zona lui, salvează în /tmp
4. waitpid() × 4   → părintele așteaptă copiii
5. CompositeImage()→ lipește zonele înapoi
6. ImageToBlob()   → re-codifică rezultatul
```

**De ce procese (`fork`) și nu thread-uri?** GraphicsMagick nu e thread-safe. Procesele au memorie separată — fiecare copil lucrează pe propria copie, fără corupere de date.

### Procesarea asincronă (tichete + polling)

Clienții **nu așteaptă blocant** rezultatul. Fluxul:

```
client                          server
  │ submitJob(imagine, filtru)    │
  │──────────────────────────────►│  pune jobul în coada FIFO
  │◄────────── tichet ────────────│  răspunde imediat
  │                               │
  │ jobStatus(tichet)             │  (worker thread procesează în fundal)
  │──────────────────────────────►│
  │◄───────── PENDING ────────────│  clientul reîncearcă la ~200ms
  │ jobStatus(tichet)             │
  │──────────────────────────────►│
  │◄── DONE + imagine + timp ─────│  tichetul devine invalid (one-shot)
```

Avantaj: serverul rămâne responsiv — o imagine mare în procesare nu blochează
ceilalți clienți. Coada (`server/jobs.c`) are 32 de sloturi; `serverInfo`
raportează mărimea ei în `queueSize`.

### Operațiile SOAP expuse

| Operație | Rol |
|----------|-----|
| `connect` | Înregistrează clientul, returnează un ID unic (1–10000) |
| `echo` | Ping / verificare server online |
| `submitJob` | Primește imagine + filtru, pune jobul în coadă, returnează **tichet** |
| `jobStatus` | Polling după tichet: PENDING / RUNNING / DONE (+imagine) / ERROR |
| `applyFilter` | Varianta sincronă veche (păstrată pentru compatibilitate) |
| `bye` | Deconectează clientul |
| `serverInfo` | Statistici: clienți activi, status OPEN/CLOSED, mărimea cozii |

Protocolul TCP binar are echivalentele `TCP_SUBMIT_JOB` / `TCP_JOB_STATUS`.

> Codul SOAP (`soapC.c`, `soapServer.c`, `soapClient.c`) este **generat automat** de `soapcpp2` din `pif.h` — nu se modifică manual.

---

## Structura proiectului

```
├── server/
│   ├── server.c          # Serverul principal + endpoint-urile SOAP
│   ├── tcp_server.c      # Serverul TCP binar (port 18083)
│   ├── unix_server.c     # Socket UNIX pentru admin
│   ├── jobs.c            # Coada FIFO de joburi cu tichete + worker thread
│   ├── processing.c      # Procesarea paralelă a imaginilor (fork × 4)
│   ├── admin.c           # Panoul de administrare (ncurses)
│   ├── dataTypes.h       # Tipuri și constante comune
│   └── pif.h             # Definiția interfeței gSOAP (server)
│
├── clients/
│   ├── client/           # Client SOAP CLI (main.c, client.c, config.c, pif.h)
│   ├── tcp_client/       # Client TCP binar (tcp_client.c)
│   └── web/              # Client web: index.html + scripts/ + assets/
│
├── CMakeLists.txt
└── README.md
```

---

## Rulare în rețea (LAN)

Serverul ascultă implicit pe toate interfețele (`0.0.0.0`) — clienții din LAN se conectează direct la IP-ul mașinii server.

```bash
# află IP-ul serverului
hostname -I

# deschide porturile în firewall
sudo ufw allow 18082/tcp
sudo ufw allow 18083/tcp
```

Apoi, pe client:
- **web**: scrie `http://IP-SERVER:18082` în câmpul de adresă din pagină
- **CLI**: `./pif-client -h IP-SERVER -p 18082`
- **TCP**: `./pif-tcp-client IP-SERVER 18083 blur in.jpg out.jpg`

> Adminul rămâne local-only (socket UNIX).

---

## Glosar

| Termen | Descriere |
|--------|-----------|
| **SOAP** | Protocol de comunicație bazat pe XML, transportat prin HTTP |
| **gSOAP** | Bibliotecă C pentru SOAP; generează codul de serializare din `pif.h` |
| **fork()** | Apel de sistem care creează un proces copil (copie a procesului curent) |
| **mutex** | Lacăt: garantează că un singur thread accesează o resursă la un moment dat |
| **UNIX socket** | Canal de comunicație între procese de pe aceeași mașină |
| **GraphicsMagick** | Bibliotecă C de procesare a imaginilor |
| **CORS** | Mecanism prin care browserul permite cereri către alt port/domeniu |
| **blob** | Vector de bytes (aici: imaginea codificată) |
