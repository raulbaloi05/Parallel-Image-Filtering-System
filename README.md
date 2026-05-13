# PIF — Parallel Image Filtering

**Ghid complet pentru începători**

---

Acest document explică de la zero cum funcționează întregul proiect.  
Nu necesită cunoștințe prealabile de rețele sau sisteme de operare.

---

## Cuprins

- [Ce face proiectul](#ce-face-proiectul)
- [Structura directorului](#structura-directorului)
- [Arhitectura generală](#arhitectura-generală)
- [Ce este SOAP?](#ce-este-soap)
- [Serverul](#serverul)
- [Procesarea imaginilor](#procesarea-imaginilor)
- [Clientul în linie de comandă](#clientul-în-linie-de-comandă)
- [Panoul de administrare](#panoul-de-administrare)
- [Compilare și rulare](#compilare-și-rulare)
- [Clientul web](#clientul-web)
- [Configurare pentru LAN](#configurare-pentru-lan)
- [Fluxul complet al unui request](#fluxul-complet-al-unui-request)
- [Glosar termeni tehnici](#glosar-termeni-tehnici)

---

## Ce face proiectul

Proiectul implementează un sistem client-server care aplică filtre pe imagini.

### Ideea simplă:

1. **Clientul** trimite o imagine (JPEG/PNG) către server
2. **Serverul** împarte imaginea în 4 bucăți și le procesează **ÎN PARALEL**
3. **Serverul** trimite înapoi imaginea filtrată
4. **Clientul** salvează rezultatul pe disc

### Filtre disponibile:

| Filtru | Descriere |
|--------|-----------|
| `grayscale` | Transformă imaginea în alb/negru |
| `blur` | Face imaginea neclară (cetoasă) |
| `sharpen` | Accentuează detaliile |
| `edge` | Detectează contururile (marginile obiectelor) |
| `negative` | Inversează culorile (ca un negativ foto) |

---

## Structura directorului

```
make/
├── server/                    # Codul serverului
│   ├── server.c              # Serverul principal (SOAP/HTTP)
│   ├── unix_server.c         # Server secundar (socket UNIX, pentru admin)
│   ├── processing.c          # Procesarea efectivă a imaginilor
│   ├── admin.c               # Panoul de administrare (TUI cu ncurses)
│   ├── dataTypes.h           # Tipurile de date comune server/admin
│   ├── processing.h          # Header pentru processing.c
│   └── pif.h / soapH.h       # Cod generat automat de gSOAP (nu modifica manual)
│
├── clients/
│   ├── client/               # Clientul în linie de comandă (C)
│   │   ├── main.c            # Punctul de intrare al clientului
│   │   ├── client.c          # Operațiile de rețea (connect, filter, bye)
│   │   ├── config.c          # Încărcarea configurației
│   │   ├── proto.c           # Protocolul de comunicație de nivel jos
│   │   └── client.cfg        # Fișierul de configurație
│   │
│   └── web/                  # Clientul web (browser)
│       ├── index.html        # Interfața grafică
│       ├── scripts/
│       │   ├── app.js        # Logica UI (butoane, preview, drag&drop)
│       │   └── soap.js       # Comunicarea cu serverul (SOAP over HTTP)
│       └── assets/
│           └── style.css     # Stilizarea interfeței
│
├── build/                    # Fișierele compilate (generate de CMake, nu modifica)
├── CMakeLists.txt            # Configurația de compilare pentru întreg proiect
└── README.md                 # Acest fișier
```

---

## Arhitectura generală

**Cine vorbește cu cine:**

```
                      ┌─────────────────────────────────────┐
                      │             SERVER                  │
                      │                                     │
    ┌──────────┐  TCP │  ┌──────────────────────────────┐  │
    │ Client C │──────►│  │  Thread SOAP (port 18082)    │  │
    └──────────┘  HTTP │  │  Ascultă cereri SOAP/HTTP    │  │
                      │  └──────────────┬─────────────────┘  │
    ┌──────────┐  TCP │                │                     │
    │ Client   │──────►│         ┌──────▼──────┐             │
    │   Web    │  HTTP │         │ global_state│             │
    └──────────┘       │         │ (mutex)     │             │
                      │         └──────┬──────┘             │
                      │                │                     │
                      │  ┌─────────────▼──────────────────┐ │
    ┌──────────┐UNIX   │  │ Thread UNIX socket             │ │
    │  Admin   │──────►│  │ (fișier /tmp/unixds)           │ │
    │   TUI    │socket │  │ Ascultă comenzi de la admin    │ │
    └──────────┘       │  └────────────────────────────────┘ │
                      └─────────────────────────────────────┘
```

### Explicație:

- **Clientul C** și **Clientul Web** comunică cu serverul prin **SOAP** (un protocol bazat pe XML, trimis prin HTTP pe portul 18082)
- **Admin-ul** comunică cu serverul printr-un **UNIX socket** (un "pipe" local, mai rapid, folosit doar pe aceeași mașină). Adminul **NU** merge prin rețea
- **Serverul** are o singură stare globală (`global_state`) protejată de un mutex pentru a evita conflictele între threaduri

---

## Ce este SOAP?

**SOAP (Simple Object Access Protocol)** este un protocol de comunicație.

Clientul trimite un mesaj XML către server, serverul procesează și trimite înapoi tot un XML.

### De ce SOAP și nu REST/JSON?

Cerința proiectului impune explicit SOAP. În plus, **gSOAP** generează automat tot codul de serializare/deserializare din fișierul de definiție `pif.h` — nu scriem manual parsare XML. Cu REST am fi scris manual tot protocolul.

### Exemplu de mesaj SOAP

**Cerere connect (trimisă de client):**

```xml
<?xml version="1.0" encoding="UTF-8"?>
<SOAP-ENV:Envelope xmlns:SOAP-ENV="...">
  <SOAP-ENV:Body>
    <ns1:connect>
    </ns1:connect>
  </SOAP-ENV:Body>
</SOAP-ENV:Envelope>
```

**Răspuns (de la server):**

```xml
<connect>4231</connect>   <!-- clientID-ul unic pentru această sesiune -->
```

> ** Important:** În proiect, codul SOAP (`soapH.h`, `soapC.c`, `soapServer.c`, `soapClient.c`) este **GENERAT AUTOMAT** de gSOAP din fișierul de definiție `pif.h`. Tu nu scrii acel cod — îl generează gSOAP din descrierea interfeței.

---

## Serverul

**Fișier:** `server/server.c`

Serverul expune **5 operații** (endpoint-uri) prin SOAP:

### `ns__connect(req, resp)`

Clientul se conectează. Serverul generează un ID unic (1-10000) și îl înregistrează în lista de clienți activi.

**Returnează:** `clientID` (int)

> ** De ce ID random 1-10000 și nu un contor simplu 1,2,3...?**  
> Un contor ar reutiliza ID-uri după deconectare. Dacă un client vechi trimite încă mesaje cu ID=3 după ce s-a deconectat, un nou client cu ID=3 ar primi răspunsurile greșite. ID-ul random reduce șansa de coliziune fără a necesita persistență pe disc.

**De reținut:** Dacă serverul e "CLOSED" (închis de admin) sau a atins limita de clienți (`max_clients_number`), refuză conexiunea.

### `ns__echo(req, resp)`

Primește un string, returnează același string.  
Folosit pentru a verifica dacă serverul este online (ping).

### `ns__applyFilter(req, resp)`

**Operația principală.** Primește:
- `imageData`: imaginea ca bytes bruți
- `filterType`: "grayscale", "blur", etc.
- `clientId`: ID-ul sesiunii

Apelează `process_image()` din `processing.c`, măsoară timpul, returnează imaginea procesată + timpul de procesare în ms.

### `ns__bye(req, resp)`

Clientul se deconectează. Serverul șterge clientul din lista activă.

### `ns__serverInfo(req, resp)`

Returnează: numărul de clienți activi, statusul serverului (OPEN/CLOSED), etc.

### Threaduri

Serverul are **2 threaduri:**

1. **Threadul principal** — rulează SOAP (bucla `soap_accept` → `soap_serve`)
2. **Thread separat** (`unix_main`) — rulează socket-ul UNIX pentru admin

> ** De ce `soap->keep_alive = 0`?**  
> Cu keep-alive activ, `soap_serve()` intră într-o buclă internă și așteaptă următoarea cerere de la același client înainte să returneze. Serverul nostru e single-threaded pe SOAP — un singur client cu keep-alive ar bloca toți ceilalți clienți la nesfârșit. `keep_alive = 0` forțează închiderea conexiunii după fiecare cerere.

---

## Procesarea imaginilor

**Fișier:** `server/processing.c`

Funcția principală: `process_image(blob, size, filter, out_size)`

### Fluxul de procesare:

```
1. BlobToImage()          → Decodifică bytes-urile în structură GraphicsMagick
2. GetImageWidth/Height   → Află dimensiunile
3. fork() × 4             → Creează 4 procese copii
   │
   ├─ Copil 0: procesează zona stânga-sus
   ├─ Copil 1: procesează zona dreapta-sus
   ├─ Copil 2: procesează zona stânga-jos
   └─ Copil 3: procesează zona dreapta-jos
   │
4. waitpid() × 4          → Părintele așteaptă terminarea copiilor
5. ImageToBlob()          → Re-codifică rezultatul în bytes
```

### De ce procese (fork) și nu threaduri?

**GraphicsMagick nu e thread-safe.** Dacă 4 threaduri ar modifica aceeași imagine simultan, memoria partajată ar genera corupție de date. Procesele copii au **memorie separată** — fiecare copil lucrează pe propria copie a imaginii, fără conflict.

### Procesarea zonei

Fiecare copil:
1. Extrage regiunea sa (`ExtractImageRegion`)
2. Aplică filtrul (`ModulateImage`, `BlurImage`, etc.)
3. Salvează rezultatul într-un fișier temporar (ex: `/tmp/pif_zone0_XXXXXX.miff`)
4. Termină

Părintele:
1. Așteaptă toți copiii
2. Reîncarcă cele 4 imagini procesate din `/tmp/`
3. Le lipește înapoi (`CompositeImage`)
4. Șterge fișierele temporare

---

## Clientul în linie de comandă

**Fișier:** `clients/client/`

### Utilizare:

```bash
./pif-client [opțiuni]

Opțiuni:
  -h <host>        Adresa serverului (ex: 127.0.0.1)
  -p <port>        Portul serverului (ex: 18082)
  -F <filter>      Tipul filtrului (grayscale, blur, sharpen, edge, negative)
  -i <input>       Fișierul imagine de intrare
  -o <output>      Fișierul imagine de ieșire
```

### Exemple:

```bash
# Folosind configurația din client.cfg
./pif-client

# Specificând parametri
./pif-client -h 127.0.0.1 -F blur -i foto.jpg -o out.jpg

# Folosind variabile de mediu
PIF_SERVER_HOST=192.168.1.10 ./pif-client
```

### Fișierul de configurație

**`clients/client/client.cfg`:**

```
server {
  host = "127.0.0.1";
  port = 18082;
}

filter {
  type = "grayscale";
}

files {
  input = "input.jpg";
  output = "output.jpg";
}
```

---

## Panoul de administrare

**Fișier:** `server/admin.c`

Interfață TUI (Text User Interface) cu **ncurses** pentru monitorizarea și controlul serverului.

### Funcții:

- **Afișare statistici:** număr clienți activi, clienți totali, cereri procesate
- **Control server:** OPEN/CLOSE (permite/blochează conexiuni noi)
- **Listă clienți:** afișează toți clienții conectați cu ID și timestamp
- **Refresh automat:** actualizare la fiecare 2 secunde

### Comenzi:

| Tastă | Acțiune |
|-------|---------|
| `o` | Deschide serverul (OPEN) |
| `c` | Închide serverul (CLOSED) |
| `r` | Refresh manual |
| `q` | Quit (ieșire) |

> ** Important:** Adminul comunică prin UNIX socket → rulează doar pe **aceeași mașină** cu serverul.

---

## Compilare și rulare

### Dependințe

```bash
# Ubuntu/Debian
sudo apt install libgsoap-dev libgraphicsmagick1-dev libncurses-dev libconfig-dev

# Fedora/RHEL
sudo dnf install gsoap-devel GraphicsMagick-devel ncurses-devel libconfig-devel
```

### Compilare

```bash
cd make/
mkdir -p build && cd build
cmake ..
make
```

### Executabile generate:

- `build/server/pif_server` — Serverul principal
- `build/server/admin` — Panoul de administrare
- `build/clients/client/pif-client` — Clientul C

### Rulare:

**1. Pornește serverul:**

```bash
./build/server/pif_server
```

**Output așteptat:**

```
Server started with UNIX socket and SOAP threads
[UNIX Thread] Unix Socket Server running on ...
[SOAP Thread] Starting on port 18082...
```

**2. Rulează clientul C:**

```bash
./build/clients/client/pif-client
# sau cu parametri:
./build/clients/client/pif-client -h 127.0.0.1 -F blur -i foto.jpg -o out.jpg
```

**3. Rulează adminul (opțional):**

```bash
./build/server/admin
```

**4. Clientul web:**

Deschide `clients/web/index.html` în browser.

---

## Clientul web

**Locație:** `clients/web/`

Interfață grafică accesibilă din browser, **fără instalare**.

### Fișiere:

- `index.html` — Structura paginii (upload, selecție filtru, preview)
- `scripts/app.js` — Logica UI (butoane, drag&drop, afișare rezultat)
- `scripts/soap.js` — Construiește și trimite mesaje SOAP (XML over HTTP)
- `assets/style.css` — Stilizarea vizuală

### Cum funcționează:

1. Utilizatorul încarcă o imagine și alege filtrul
2. `app.js` apelează `soap.js` pentru a trimite cererea SOAP către server
3. Serverul procesează și returnează imaginea filtrată ca base64
4. `app.js` afișează rezultatul și oferă opțiunea de download

### Configurare server URL:

Editează `scripts/soap.js`, linia:

```javascript
serverUrl: 'http://localhost:18082'
```

> ** Notă:** Nu necesită server web separat — funcționează direct din sistemul de fișiere (`file://`) sau dintr-un server HTTP simplu (`python3 -m http.server`).

---

## Configurare pentru LAN

Implicit serverul ascultă pe **toate interfețele** (`NULL` în `soap_bind` = `0.0.0.0`).  
Clienții din LAN pot accesa serverul prin IP-ul mașinii unde rulează serverul.

### Cum să afli IP-ul serverului (pe Linux):

```bash
ip addr show
# sau
hostname -I
```

### Configurare client C:

**Metoda 1 — Fișier `client.cfg`:**

```
server {
  host = "192.168.1.10";
  port = 18082;
}
```

**Metoda 2 — CLI:**

```bash
./pif-client -h 192.168.1.10 -p 18082
```

**Metoda 3 — Variabilă de mediu:**

```bash
PIF_SERVER_HOST=192.168.1.10 ./pif-client
```

### Configurare client web:

Editează `clients/web/scripts/soap.js`:

```javascript
serverUrl: 'http://192.168.1.10:18082'  // Schimbă localhost cu IP-ul serverului
```

> ** Adminul NU merge prin rețea** (UNIX socket = doar local).

### Firewall:

Asigură-te că portul **18082 TCP** este deschis pe mașina server:

```bash
sudo ufw allow 18082/tcp
```

---

## Fluxul complet al unui request

**De la click până la imagine procesată:**

```
[Browser/Client]          [Server SOAP Thread]       [processing.c]
────────────────────────────────────────────────────────────────────────

1. Utilizatorul selectează
   o imagine și apasă
   "Procesează"
      │
2. app.js apelează
   SOAP.connect()
   ─── XML SOAP ──────────► ns__connect()
                             Generează ID=4231
   ◄── clientID=4231 ───────
      │
3. SOAP.applyFilter(
     base64, "blur", 4231)
   ─── XML SOAP ──────────► ns__applyFilter()
                             Apelează process_image()
                                      │
                             ┌────────▼────────────┐
                             │ BlobToImage()        │
                             │ Împarte în 4 zone    │
                             │ fork() x4            │
                             │  copil0: blur zona0  │
                             │  copil1: blur zona1  │
                             │  copil2: blur zona2  │
                             │  copil3: blur zona3  │
                             │ waitpid() x4         │
                             │ Asamblează imaginea  │
                             │ ImageToBlob()        │
                             └────────┬────────────┘
                             Returnează blob + timp
   ◄── imagine procesată ───
      │
4. app.js afișează
   rezultatul în browser
   și oferă opțiunea
   de download
      │
5. SOAP.bye()
   ─── XML SOAP ──────────► ns__bye()
                             Șterge clientul din listă
```

---

## Glosar termeni tehnici

| Termen | Descriere |
|--------|-----------|
| **SOAP** | Protocol de comunicație bazat pe XML, trimis prin HTTP |
| **gSOAP** | Bibliotecă C care implementează SOAP; generează cod din definiții |
| **fork()** | Apel de sistem Linux care creează un proces copil (duplică procesul) |
| **waitpid()** | Părintele așteaptă terminarea unui proces copil specific |
| **pthread** | Bibliotecă pentru threaduri în C (POSIX threads) |
| **mutex** | Mecanism de blocare: garantează că doar un thread accesează o resursă la un moment dat |
| **UNIX socket** | Canal de comunicație între procese pe aceeași mașină (nu rețea) |
| **blob** | Binary Large Object; vector de bytes (ex: o imagine) |
| **GraphicsMagick** | Bibliotecă C pentru procesarea imaginilor (fork din ImageMagick) |
| **ncurses** | Bibliotecă pentru interfețe TUI (text) în terminal |
| **libconfig** | Bibliotecă pentru fișiere de configurație cu sintaxă `key = value` |
| **CMake** | Sistem de build: generează Makefile-uri din `CMakeLists.txt` |
| **base64** | Codificare: convertește bytes binari în caractere ASCII (folosit pentru imagini în XML/JSON) |
| **endpoint** | O funcție expusă prin rețea (ex: `ns__applyFilter` = endpoint SOAP) |
| **thread** | "Fir de execuție" în cadrul aceluiași proces; partajează memoria |
| **process** | Program în execuție; are memorie separată de alte procese |