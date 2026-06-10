/**
 * soap.js — strat de comunicatie SOAP/HTTP intre clientul web si serverul PIF.
 *
 * Serverul este generat cu gSOAP (document/literal, namespace
 * http://tempuri.org/ns.xsd, schema form "unqualified", SOAPAction "").
 * Aici construim manual plicul SOAP 1.1 si parsam raspunsul cu DOMParser,
 * fara nicio biblioteca externa.
 *
 * Operatii expuse (5 endpoint-uri ale serverului):
 *   - connect()                         -> aloca un clientID
 *   - echo(text)                        -> ping / healthcheck
 *   - applyFilter(b64, filtru, id)      -> trimite imaginea, primeste rezultatul
 *   - bye(id)                           -> inchide sesiunea
 *   - serverInfo()                      -> statistici server (clienti, status)
 *
 * Toate functiile sunt asincrone (Promise / fetch) -> clientul nu blocheaza UI-ul
 * cat timp serverul proceseaza imaginea.
 */

const SOAP = (() => {
    // URL-ul serverului SOAP. Pentru LAN, schimba localhost cu IP-ul masinii server.
    const config = {
        serverUrl: 'http://localhost:18082',
    };

    const NS = 'http://tempuri.org/ns.xsd';
    const ENV = 'http://schemas.xmlsoap.org/soap/envelope/';

    /* Impacheteaza un corp de operatie intr-un plic SOAP 1.1 complet. */
    function envelope(bodyXml) {
        return (
            '<?xml version="1.0" encoding="UTF-8"?>' +
            '<SOAP-ENV:Envelope ' +
            'xmlns:SOAP-ENV="' + ENV + '" ' +
            'xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" ' +
            'xmlns:xsd="http://www.w3.org/2001/XMLSchema" ' +
            'xmlns:ns1="' + NS + '">' +
            '<SOAP-ENV:Body>' + bodyXml + '</SOAP-ENV:Body>' +
            '</SOAP-ENV:Envelope>'
        );
    }

    /* Trimite un plic SOAP la server si returneaza Document-ul XML al raspunsului. */
    async function call(bodyXml) {
        const res = await fetch(config.serverUrl, {
            method: 'POST',
            headers: {
                'Content-Type': 'text/xml; charset=utf-8',
                'SOAPAction': '""',
            },
            body: envelope(bodyXml),
        });

        const text = await res.text();
        const doc = new DOMParser().parseFromString(text, 'text/xml');

        // Eroare de parsare XML
        if (doc.querySelector('parsererror')) {
            throw new Error('Raspuns XML invalid de la server');
        }

        // SOAP Fault -> aruncam mesajul de eroare
        const fault = byLocal(doc, 'Fault');
        if (fault) {
            const reason =
                text.match(/<faultstring>([\s\S]*?)<\/faultstring>/)?.[1] ||
                'SOAP Fault';
            throw new Error(reason);
        }

        if (!res.ok) {
            throw new Error('HTTP ' + res.status + ' ' + res.statusText);
        }

        return doc;
    }

    /* Cauta primul element dupa numele local, ignorand prefixul de namespace. */
    function byLocal(node, localName) {
        const all = node.getElementsByTagName('*');
        for (let i = 0; i < all.length; i++) {
            if (all[i].localName === localName) return all[i];
        }
        return null;
    }

    function textOf(doc, localName) {
        const el = byLocal(doc, localName);
        return el ? el.textContent : null;
    }

    // ---- Endpoint-uri ----

    /* ns:connect -> { clientId } */
    async function connect() {
        const doc = await call('<ns1:connect/>');
        const id = textOf(doc, 'connect');
        if (id === null) throw new Error('Serverul nu a returnat un clientID');
        return { clientId: parseInt(id, 10) };
    }

    /* ns:echo -> string (folosit ca ping) */
    async function echo(text) {
        const doc = await call(
            '<ns1:echo><echoRequest>' + escapeXml(text) + '</echoRequest></ns1:echo>'
        );
        return textOf(doc, 'echo');
    }

    /* ns:applyFilter -> { imageBase64, processingTime } */
    async function applyFilter(imageBase64, filterType, clientId) {
        const body =
            '<ns1:applyFilter>' +
            '<imageData>' + imageBase64 + '</imageData>' +
            '<filterType>' + escapeXml(filterType) + '</filterType>' +
            '<processCount>4</processCount>' +
            '<clientId>' + (clientId | 0) + '</clientId>' +
            '</ns1:applyFilter>';
        const doc = await call(body);
        const out = textOf(doc, 'imageData');
        if (!out) throw new Error('Serverul a returnat o imagine goala');
        return {
            imageBase64: out,
            processingTime: parseInt(textOf(doc, 'processingTime') || '0', 10),
        };
    }

    /* ns:bye -> status (int) */
    async function bye(clientId) {
        const doc = await call(
            '<ns1:bye><byeRequest><id>' + (clientId | 0) + '</id></byeRequest></ns1:bye>'
        );
        return parseInt(textOf(doc, 'status') || '0', 10);
    }

    /* ns:serverInfo -> { clients, activeJobs, status, memory, queueSize } */
    async function serverInfo() {
        const doc = await call('<ns1:serverInfo/>');
        return {
            clients: parseInt(textOf(doc, 'clients') || '0', 10),
            activeJobs: parseInt(textOf(doc, 'activeJobs') || '0', 10),
            status: textOf(doc, 'uptime') || '?', // serverul pune statusul OPEN/CLOSED in "uptime"
            memory: textOf(doc, 'memory') || '?',
            queueSize: parseInt(textOf(doc, 'queueSize') || '0', 10),
        };
    }

    /* Escapare minimala XML pentru valorile text introduse de utilizator. */
    function escapeXml(s) {
        return String(s)
            .replace(/&/g, '&amp;')
            .replace(/</g, '&lt;')
            .replace(/>/g, '&gt;');
    }

    return { config, connect, echo, applyFilter, bye, serverInfo };
})();
