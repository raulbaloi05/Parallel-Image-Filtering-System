/**
 * app.js — logica de interfata a clientului web PIF.
 *
 * Gestioneaza:
 *   - conectarea / deconectarea la server (connect / bye);
 *   - incarcarea imaginii prin buton sau drag & drop;
 *   - selectarea filtrului si trimiterea la server (applyFilter);
 *   - previzualizarea inainte/dupa si descarcarea rezultatului;
 *   - panoul de status al serverului, actualizat periodic (serverInfo);
 *   - ping-ul serverului (echo).
 *
 * Tot dialogul cu serverul trece prin SOAP din soap.js si este asincron.
 */

(() => {
    'use strict';

    // ---- Stare aplicatie ----
    const state = {
        clientId: null,       // ID-ul de sesiune primit de la server
        inputBase64: null,    // imaginea sursa, base64 fara prefix data:
        inputMime: null,      // tipul MIME al imaginii sursa
        outputBlobUrl: null,  // object URL al rezultatului (pentru download)
        outputMime: null,
        filter: 'grayscale',  // filtrul selectat
        statusTimer: null,    // handle pentru polling-ul serverInfo
    };

    // ---- Scurtaturi DOM ----
    const $ = (id) => document.getElementById(id);

    const els = {
        serverUrl: $('serverUrl'),
        btnConnect: $('btnConnect'),
        btnBye: $('btnBye'),
        btnPing: $('btnPing'),
        connDot: $('connDot'),
        connText: $('connText'),
        drop: $('dropZone'),
        fileInput: $('fileInput'),
        filters: $('filters'),
        btnProcess: $('btnProcess'),
        btnDownload: $('btnDownload'),
        inImg: $('inputPreview'),
        outImg: $('outputPreview'),
        inMeta: $('inputMeta'),
        outMeta: $('outputMeta'),
        log: $('log'),
        statClients: $('statClients'),
        statJobs: $('statJobs'),
        statStatus: $('statStatus'),
        statQueue: $('statQueue'),
    };

    // ---- Utilitare ----

    function log(msg, kind = 'info') {
        const line = document.createElement('div');
        line.className = 'log-line log-' + kind;
        const t = new Date().toLocaleTimeString();
        line.textContent = '[' + t + '] ' + msg;
        els.log.prepend(line);
    }

    function setConnected(connected) {
        els.connDot.classList.toggle('on', connected);
        els.connText.textContent = connected
            ? 'Conectat (ID ' + state.clientId + ')'
            : 'Deconectat';
        els.btnConnect.disabled = connected;
        els.btnBye.disabled = !connected;
        els.btnPing.disabled = !connected;
        updateProcessBtn();
    }

    function updateProcessBtn() {
        els.btnProcess.disabled = !(state.clientId && state.inputBase64);
    }

    /* Detecteaza tipul MIME dupa magic bytes; fallback image/png. */
    function sniffMime(bytes) {
        if (bytes.length >= 3 && bytes[0] === 0xff && bytes[1] === 0xd8 && bytes[2] === 0xff)
            return 'image/jpeg';
        if (bytes.length >= 8 && bytes[0] === 0x89 && bytes[1] === 0x50)
            return 'image/png';
        if (bytes.length >= 6 && bytes[0] === 0x47 && bytes[1] === 0x49 && bytes[2] === 0x46)
            return 'image/gif';
        if (bytes.length >= 2 && bytes[0] === 0x42 && bytes[1] === 0x4d)
            return 'image/bmp';
        return 'image/png';
    }

    function base64ToBytes(b64) {
        const bin = atob(b64);
        const bytes = new Uint8Array(bin.length);
        for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
        return bytes;
    }

    // ---- Incarcare imagine ----

    function loadFile(file) {
        if (!file || !file.type.startsWith('image/')) {
            log('Fisier invalid: selecteaza o imagine.', 'err');
            return;
        }
        const reader = new FileReader();
        reader.onload = () => {
            const dataUrl = reader.result;             // data:image/...;base64,XXXX
            const comma = dataUrl.indexOf(',');
            state.inputBase64 = dataUrl.slice(comma + 1);
            state.inputMime = file.type;
            els.inImg.src = dataUrl;
            els.inImg.classList.add('has-img');
            els.inMeta.textContent =
                file.name + ' · ' + Math.round(file.size / 1024) + ' KB';
            log('Imagine incarcata: ' + file.name);
            updateProcessBtn();
        };
        reader.onerror = () => log('Eroare la citirea fisierului.', 'err');
        reader.readAsDataURL(file);
    }

    // ---- Operatii SOAP ----

    async function doConnect() {
        SOAP.config.serverUrl = els.serverUrl.value.trim() || SOAP.config.serverUrl;
        try {
            log('Conectare la ' + SOAP.config.serverUrl + ' ...');
            const { clientId } = await SOAP.connect();
            state.clientId = clientId;
            setConnected(true);
            log('Conectat. clientID=' + clientId, 'ok');
            startStatusPolling();
        } catch (e) {
            log('Connect esuat: ' + e.message, 'err');
        }
    }

    async function doBye() {
        if (!state.clientId) return;
        try {
            await SOAP.bye(state.clientId);
            log('Deconectat (ID ' + state.clientId + ').', 'ok');
        } catch (e) {
            log('Bye esuat: ' + e.message, 'err');
        } finally {
            state.clientId = null;
            setConnected(false);
            stopStatusPolling();
        }
    }

    async function doPing() {
        try {
            const r = await SOAP.echo('ping');
            log('Ping OK: server a raspuns "' + r + '".', 'ok');
        } catch (e) {
            log('Ping esuat: ' + e.message, 'err');
        }
    }

    async function doProcess() {
        if (!state.clientId || !state.inputBase64) return;
        els.btnProcess.disabled = true;
        els.btnProcess.classList.add('busy');
        log('Aplicare filtru "' + state.filter + '" ...');
        try {
            const { imageBase64, processingTime } =
                await SOAP.applyFilter(state.inputBase64, state.filter, state.clientId);

            const bytes = base64ToBytes(imageBase64);
            const mime = sniffMime(bytes);
            const blob = new Blob([bytes], { type: mime });

            if (state.outputBlobUrl) URL.revokeObjectURL(state.outputBlobUrl);
            state.outputBlobUrl = URL.createObjectURL(blob);
            state.outputMime = mime;

            els.outImg.src = state.outputBlobUrl;
            els.outImg.classList.add('has-img');
            els.outMeta.textContent =
                Math.round(bytes.length / 1024) + ' KB · ' + processingTime + ' ms';
            els.btnDownload.disabled = false;
            log('Procesat in ' + processingTime + ' ms.', 'ok');
        } catch (e) {
            log('Filtrare esuata: ' + e.message, 'err');
        } finally {
            els.btnProcess.classList.remove('busy');
            updateProcessBtn();
        }
    }

    function doDownload() {
        if (!state.outputBlobUrl) return;
        const ext = (state.outputMime || 'image/png').split('/')[1] || 'png';
        const a = document.createElement('a');
        a.href = state.outputBlobUrl;
        a.download = 'pif_' + state.filter + '.' + ext;
        a.click();
    }

    // ---- Polling status server ----

    async function refreshStatus() {
        try {
            const info = await SOAP.serverInfo();
            els.statClients.textContent = info.clients;
            els.statJobs.textContent = info.activeJobs;
            els.statStatus.textContent = info.status;
            els.statQueue.textContent = info.queueSize;
        } catch {
            /* server picat / indisponibil — lasam ultimele valori */
        }
    }

    function startStatusPolling() {
        refreshStatus();
        stopStatusPolling();
        state.statusTimer = setInterval(refreshStatus, 2000);
    }

    function stopStatusPolling() {
        if (state.statusTimer) clearInterval(state.statusTimer);
        state.statusTimer = null;
    }

    // ---- Legare evenimente ----

    function bind() {
        els.serverUrl.value = SOAP.config.serverUrl;

        els.btnConnect.addEventListener('click', doConnect);
        els.btnBye.addEventListener('click', doBye);
        els.btnPing.addEventListener('click', doPing);
        els.btnProcess.addEventListener('click', doProcess);
        els.btnDownload.addEventListener('click', doDownload);

        // upload prin click
        els.drop.addEventListener('click', () => els.fileInput.click());
        els.fileInput.addEventListener('change', (e) => {
            if (e.target.files[0]) loadFile(e.target.files[0]);
        });

        // drag & drop
        ['dragenter', 'dragover'].forEach((ev) =>
            els.drop.addEventListener(ev, (e) => {
                e.preventDefault();
                els.drop.classList.add('drag');
            })
        );
        ['dragleave', 'drop'].forEach((ev) =>
            els.drop.addEventListener(ev, (e) => {
                e.preventDefault();
                els.drop.classList.remove('drag');
            })
        );
        els.drop.addEventListener('drop', (e) => {
            if (e.dataTransfer.files[0]) loadFile(e.dataTransfer.files[0]);
        });

        // selectare filtru
        els.filters.addEventListener('click', (e) => {
            const btn = e.target.closest('button[data-filter]');
            if (!btn) return;
            state.filter = btn.dataset.filter;
            els.filters
                .querySelectorAll('button')
                .forEach((b) => b.classList.toggle('active', b === btn));
        });

        // deconectare best-effort la inchiderea paginii (fetch keepalive)
        window.addEventListener('pagehide', () => {
            if (!state.clientId) return;
            const body =
                '<?xml version="1.0" encoding="UTF-8"?>' +
                '<SOAP-ENV:Envelope xmlns:SOAP-ENV="http://schemas.xmlsoap.org/soap/envelope/" ' +
                'xmlns:ns1="http://tempuri.org/ns.xsd"><SOAP-ENV:Body>' +
                '<ns1:bye><byeRequest><id>' + state.clientId + '</id></byeRequest></ns1:bye>' +
                '</SOAP-ENV:Body></SOAP-ENV:Envelope>';
            try {
                fetch(SOAP.config.serverUrl, {
                    method: 'POST',
                    headers: { 'Content-Type': 'text/xml; charset=utf-8', 'SOAPAction': '""' },
                    body,
                    keepalive: true,
                });
            } catch { /* best-effort */ }
        });

        setConnected(false);
        els.btnDownload.disabled = true;
        log('Client web PIF gata. Apasa "Conectare".');
    }

    document.addEventListener('DOMContentLoaded', bind);
})();
