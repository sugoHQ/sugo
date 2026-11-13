<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->
# Piano: Sugo Sync

Stato: bozza di lavoro (4 novembre 2026, ramo `dev` @ 7ce94c3). **Server:** fasi 2.1–2.4 e test fatti nel
repository `sugo-sync` (`worker/`, contratto dell'API in `docs/api.md`, che fa fede sui dettagli), ambiente
dev online su `https://sugo-sync-dev.minerale.workers.dev`; produzione su `api.sugo.stream` non ancora.
**Client:** Fase 1 fatta sul ramo `sync/phase1-core` (migrazione, timestamp, tombstone, formato e unione, cifratura, libsodium, test); già pronti `createSecretStore()` e `MacKeychainStore` (3.5, arrivati col port macOS, PR #1). Quello che dipende da servizi esterni o da comportamenti da provare è
segnato **[verificare]**.
Effort: **S** < mezza giornata, **M** 1–2 giorni, **L** più di 2 giorni.

## Scopo della prima versione

- **Cosa si sincronizza:** solo i **timer (preset)**, compreso quello predefinito, che ha lo stesso uuid
  (`"default"`) su ogni installazione. Restano locali: il timer selezionato, le impostazioni, lo storico.
- **Piano unico, gratuito:** un account, **2 dispositivi** collegati.
- **In arrivo** (mostrato così nell'interfaccia, non attivabile): storico delle sessioni e statistiche
  unite, note dei task, eventuale piano a pagamento con più dispositivi.
- **Identico su Linux e macOS:** stesse funzioni, stessa interfaccia, stesso formato, stessa libreria di
  cifratura. Nessun `#ifdef` nel codice del sync; l'unica parte diversa è il backend di `SecretStore`,
  invisibile all'utente.
- **Cifratura end-to-end:** il server conserva solo blob cifrati; la chiave non lascia mai i dispositivi.

Il database SQLite resta la fonte di verità su ogni computer: il sync legge e scrive il DB, non lo
sostituisce.

---

## Decisioni

| # | Domanda | Decisione |
|---|---|---|
| S1 | Dove vive il server (Worker, pagina di accesso, schema D1) | **Deciso:** repository privato `sugoHQ/sugo-sync` (TypeScript, `wrangler`), in locale in `~/Documents/Coding/sugo-sync`. Il client GPL parla solo un'API documentata. |
| S2 | Provider degli account | **Clerk** (email con codice, Google, GitHub). Il Worker emette i propri token per i dispositivi; il token di Clerk non entra mai nell'app. |
| S3 | Dominio | **Deciso:** `sugo.stream` (sito e pagina di accesso) e `api.sugo.stream` (Worker). L'URL dell'API è una costante del client, sovrascrivibile con `SUGO_SYNC_URL` per i test e l'ambiente `dev`. |
| S4 | Senza portachiavi di sistema | **Deciso:** stessa regola su Linux e macOS, **il sync non si attiva** ("Sugo Sync needs the system keyring") invece di tenere la chiave in un file. Su Mac richiede `MacKeychainStore` (piano macOS 5.1). |
| S5 | Uscita del sync | **Deciso (aggiornato):** la versione macOS esce prima, da sola. Il sync esce dopo, **nella stessa versione per Linux e macOS**, così nasce comunque con la parità; il codice si sviluppa e si testa su Linux da subito. |
| S6 | Piano Workers | Gratuito (100.000 richieste/giorno, 10 ms CPU) finché i numeri lo permettono; Paid (~5 $/mese) quando il limite giornaliero si avvicina, perché oltre il limite le richieste falliscono **[verificare]** tariffe attuali. |

---

## Architettura

```
 Sugo (Linux / macOS)                      Cloudflare
 ┌──────────────────────┐   HTTPS    ┌──────────────────────────────┐
 │ PresetModel ↔ DB     │  token del │ Worker  api.sugo.stream      │
 │ SyncController       │──dispositivo─▶  auth, dispositivi, limiti │──▶ R2 (privato): users/<id>/presets.bin
 │ SyncCrypto (sodium)  │            │                              │──▶ D1: utenti, dispositivi, codici
 │ chiave in SecretStore│            └──────────────────────────────┘
 └──────────────────────┘                     ▲
                                              │ sessione Clerk
 Browser / telefono ─ sugo.stream/link ───────┘  (login + inserimento del codice)
```

- Il bucket R2 non è pubblico e non ha chiavi nel client: solo il Worker lo legge e lo scrive, e solo
  sotto `users/<id dell'utente del token>/`.
- Il Worker non vede mai i preset in chiaro.

---

## Account e dispositivi

### Accesso con codice (device code, come T3 Connect)

Uguale su Linux e macOS, nessuno schema URL da registrare, funziona anche su un computer senza browser.

1. Sugo: `POST /v1/auth/device` con nome del dispositivo (`QSysInfo::machineHostName()`, modificabile),
   piattaforma e chiave pubblica X25519 → `{device_code, user_code, verification_uri, interval, expires_in}`.
2. Sugo mostra `user_code` (es. `K7Q-4MD`), il link e un pulsante "Open in browser".
3. Sulla pagina `sugo.stream/link` (Cloudflare Pages + Clerk) l'utente accede e inserisce il codice; la pagina
   chiama `POST /v1/auth/device/approve` con la sessione Clerk, che il Worker verifica (JWKS di Clerk).
   Il dispositivo nasce qui, con un controllo atomico del limite.
4. Sugo interroga `POST /v1/auth/token` (`{"grant_type":"device_code","device_code":…}`) ogni `interval`
   secondi → `{access_token, refresh_token, device_id, user_id}`. Errori di attesa (tutti `400`):
   `authorization_pending` (con `reason: "device_limit_reached"` se il sito ha trovato il limite pieno),
   `slow_down` (con un nuovo `interval`), `expired_token`, `invalid_grant`. Un dispositivo approvato ma mai
   ritirato sparisce dopo un'ora.
5. `access_token`: JWT firmato dal Worker, 1 ora. `refresh_token`: opaco, conservato in D1 solo come hash,
   dura **180 giorni** ed è ruotato a ogni uso (`{"grant_type":"refresh_token","refresh_token":…}`).
   **Riusare un refresh token già ruotato blocca il dispositivo** (va scollegato e ricollegato). Fa eccezione
   una finestra di **60 secondi** (`REFRESH_GRACE_MS` nel Worker): se la risposta di un rinnovo si perde in
   rete, lo stesso token ripresentato entro 60 secondi riceve una coppia nuova (quella persa è revocata), a
   patto che la coppia nata dal primo uso non sia stata usata. Dopo i 60 secondi, o dopo l'uso della coppia
   nuova, il riuso blocca il dispositivo come sopra: un'app chiusa o offline per più di un minuto subito dopo
   una risposta persa trova `invalid_grant` e perde le chiavi di questo computer (servono un altro
   dispositivo o il codice di recupero). Quindi il client salva il nuovo token in `SecretStore` prima di
   qualunque altra cosa e non fa mai due rinnovi in parallelo. Un rinnovo fallito per la rete non blocca
   l'app: il sync riprova più tardi e lo stato lo dice.

### Limite di 2 dispositivi

- Il limite si controlla quando il **sito approva il codice** (`/v1/auth/device/approve`), non quando
  l'app ritira i token: è un campo dell'utente (`device_limit`, oggi 2), così un piano futuro cambia solo
  un numero.
- Al terzo: è il **sito** a mostrare i dispositivi collegati con "Disconnect" (l'app senza token non
  potrebbe scollegarne uno). Sugo, mentre interroga, riceve `authorization_pending` con
  `reason: "device_limit_reached"` e mostra "Disconnect a device on sugo.stream"; appena si libera un posto
  e il sito approva, l'accesso prosegue.
- **Scollegare** (dall'app o dal sito) cancella il dispositivo e i suoi token, anche se è spento o perso,
  e libera il posto.
- Se un giorno il limite scende, i dispositivi già collegati restano; non se ne aggiungono altri.

### La chiave dei dati

- `K` = 32 byte casuali, creata dal **primo** dispositivo, salvata in `SecretStore` (`sync-data-key`).
- **Codice di recupero** creato insieme a `K`: 128 bit casuali in Base32 di Crockford a gruppi
  (`7KQ2-M4DX-…`). Il server conserva `K` cifrata con una chiave derivata dal codice (BLAKE2b/HKDF: il
  codice ha già entropia piena, non serve Argon2). L'utente lo salva o lo stampa; Sugo chiede di
  riscriverne un gruppo per confermare.
- **Secondo dispositivo**, due strade:
  1. *Approvazione:* il dispositivo già collegato scopre quello nuovo dall'header `Sugo-Pending-Devices`
     di `GET /v1/blob/presets` (nessuna chiamata in più), poi lo legge da `GET /v1/devices`, e mostra
     "New device: Mac mini — Approve?". Approvando, invia `K` con **`crypto_box` autenticato** (la sua chiave
     privata, la chiave pubblica del nuovo) a `PUT /v1/devices/{id}/key`. Il nuovo la ritira con
     `GET /v1/key`, che restituisce anche `from_device_id` e `from_public_key`. Entrambi mostrano lo
     **stesso codice di verifica** di 16 caratteri in Base32 di Crockford (`XXXX-XXXX-XXXX-XXXX`, 80 bit
     di BLAKE2b delle due chiavi pubbliche), e l'utente conferma **su entrambi**: sul dispositivo collegato
     approvando, sul nuovo con "Same code", accanto al nome del dispositivo che deve approvarlo. Il nuovo
     apre il box solo dopo quella conferma, e solo se viene dalla chiave pubblica del codice confermato; se
     il codice passa a un altro dispositivo (la lista viene riletta ogni minuto), la conferma decade. Senza
     la conferma sul nuovo dispositivo il server poteva elencare un dispositivo suo e consegnargli una `K'`
     sua senza che l'utente confrontasse niente (così nelle versioni precedenti, che restano esposte finché
     non si aggiornano: il server non può imporre il passo). Per sostituire una delle due chiavi un server
     compromesso deve quindi far coincidere i due codici: con 6 cifre (~20 bit) bastavano ~2000 coppie di
     chiavi per lato (paradosso del compleanno); con 80 bit ne servono ~2^40 per lato durante una sola
     approvazione. È un costo, non un'impossibilità. Un passo commit-then-reveal (ognuno impegna un hash della
     propria chiave prima di vedere l'altra) toglierebbe del tutto la ricerca, ma cambia il protocollo
     (`sugo-sync`): da fare. (Con un box anonimo,
     `crypto_box_seal`, il server potrebbe consegnare una sua `K'` e poi leggere i preset caricati dal nuovo
     dispositivo.)
  2. *Codice di recupero*, se nessun altro dispositivo è acceso o collegato.
- In entrambi i casi il nuovo dispositivo, avuta `K`, conferma con `DELETE /v1/key` e smette di essere
  "in attesa" (`pending`). `GET /v1/key` da solo non cancella la chiave sigillata.
- Chi perde tutti i dispositivi **e** il codice di recupero perde i dati sincronizzati (non quelli locali).
  Va detto chiaramente nell'interfaccia e sul sito.

---

## Formato e unione dei preset

### Blob su R2 (`users/<id>/presets.bin`)

```
"SUGO" | versione formato (1 byte) | nonce (24 byte) | XChaCha20-Poly1305(JSON)
dati associati (AD) = user_id + 0x00 + "presets" + 0x00 + versione formato
```

L'AD impedisce di scambiare un blob con quello di un altro utente o di un altro tipo.

JSON in chiaro:

```json
{
  "format": 1,
  "presets": [
    { "uuid": "…", "name": "…", "category": "…", "workSeconds": 1500, "shortBreakSeconds": 300,
      "longBreakSeconds": 900, "cyclesBeforeLong": 4, "options": { "iconName": "sugo-timer" },
      "createdAt": 1762250000000, "updatedAt": 1762251234567, "updatedBy": "<device_id>" }
  ],
  "tombstones": [ { "uuid": "…", "deletedAt": 1762252000000, "deletedBy": "<device_id>" } ]
}
```

- Le opzioni sconosciute (scritte da una versione più nuova) si conservano e si ripubblicano.
- Un `format` più alto di quello conosciuto: Sugo non scrive e mostra "Update Sugo to keep syncing".
- Valori fuori da `PresetLimits` vengono corretti come per l'importazione esistente.

### Regole di unione (funzione pura, testata a parte)

- Per uuid vince la versione con `updatedAt` più alto (a parità, `updatedBy` più alto in ordine lessicale);
  una cancellazione conta come versione con `deletedAt`.
- Il preset predefinito non si cancella mai: un tombstone per `"default"` viene ignorato.
- Tombstone più vecchi di 180 giorni si eliminano. Un dispositivo rimasto offline più a lungo, con un
  preset già cancellato altrove, lo rimanda sul server alla prima unione (nessun tombstone lo ricorda più) e
  il preset torna su tutti i dispositivi. È accettato: dopo 180 giorni anche il suo refresh token è scaduto,
  quindi deve comunque ricollegarsi, e un preset ricomparso si cancella di nuovo.
- Ordine: quello locale; i preset nuovi arrivati da altri dispositivi si aggiungono in fondo per `createdAt`.
- Il timer selezionato resta locale; se viene cancellato altrove, si torna a quello predefinito (come fa già
  `PresetModel::remove`).
- Orologi sfasati tra due computer possono solo far vincere la modifica "sbagliata" tra due fatte quasi
  insieme: accettabile per i preset.

### Ciclo di sync

- **Invio:** 3 s dopo l'ultimo salvataggio riuscito (`PresetPersistence`), un solo `PUT` con
  `If-Match: <etag>`. Con `412` si scarica, si unisce, si riprova (massimo 3 volte, poi più tardi).
- **Ricezione:** all'avvio, quando la finestra torna attiva (al massimo una volta al minuto) e ogni 5 minuti,
  `GET` con `If-None-Match`: `304` non costa quasi nulla. Ogni 5 e non 30: la stessa risposta porta
  `Sugo-Pending-Devices`, e un dispositivo lasciato acceso (nel vassoio, o con la finestra già davanti, che
  non torna attiva) deve chiedere presto di approvarne uno nuovo.
- Dopo un'unione che cambia qualcosa: `PresetModel::setPresets` (nessun nuovo invio se il risultato coincide
  col remoto) e salvataggio nel DB.
- Senza rete: tutto funziona come oggi, l'invio aspetta.
- `DataManager::restoreDatabase`: dopo un ripristino i preset ripristinati vengono marcati come modificati ora
  e ripubblicati (il ripristino è una scelta esplicita dell'utente).

---

## API del Worker (v1)

| Metodo e percorso | Auth | Cosa fa |
|---|---|---|
| `POST /v1/auth/device` | — | Avvia l'accesso con codice; limite per IP. |
| `POST /v1/auth/device/approve` | sessione Clerk | Collega il codice all'utente (dalla pagina web), crea il dispositivo e controlla il limite in modo atomico. |
| `POST /v1/auth/token` | `device_code` o `refresh_token` | Emette o rinnova i token; restituisce anche `user_id` (serve all'AD del blob). |
| `GET /v1/devices` | token o sessione Clerk | Dispositivi dell'utente: nome, piattaforma, ultimo accesso, `public_key`, `pending`, `current`. |
| `DELETE /v1/devices/{id}` | token o sessione Clerk | Scollega: cancella il dispositivo e i suoi token. |
| `PUT /v1/devices/{id}/key` | token | Deposita `K` cifrata (`crypto_box`) per un dispositivo in attesa. |
| `GET /v1/key` | token | Il dispositivo nuovo ritira la sua `K` cifrata, con `from_device_id` e `from_public_key`. |
| `DELETE /v1/key` | token | Il dispositivo conferma di avere `K` e smette di essere in attesa. |
| `GET`/`PUT /v1/recovery` | token | `K` cifrata col codice di recupero (`PUT` solo dal primo dispositivo o dopo rigenerazione). |
| `GET`/`PUT /v1/blob/presets` | token | Blob con ETag e header `Sugo-Pending-Devices`. `PUT` con `If-Match` → `204`, o `If-None-Match: *` la prima volta → `201`; `412` con l'ETag attuale, `428` senza precondizione, `413` sopra 256 KB, `400 invalid_blob` se non inizia con `"SUGO"`. |
| `DELETE /v1/account` | sessione Clerk | Cancella utente, dispositivi e blob, e anche l'utente Clerk (dal Worker, con la secret key). |

D1: `user(id, clerk_id, device_limit, created_at)`, `device(id, user_id, name, platform, public_key,
created_at, claimed_at, key_ready_at, last_seen)` (scollegare cancella la riga), `refresh_token(hash,
device_id, expires_at, used_at)`, `device_code(…)`, `sealed_key(device_id, from_device_id, blob)`,
`recovery(user_id, blob)`. Dettagli e codici di errore: `docs/api.md` nel repository `sugo-sync`.

---

## Fase 1 — Base dati e logica pura (solo client, nessuna rete)

| ID | Cosa | File | Effort |
|---|---|---|---|
| 1.1 | ✅ **Migrazione 2** (in coda a `migrations()`): `preset.updated_at INTEGER NOT NULL DEFAULT 0`, `preset.updated_by TEXT NOT NULL DEFAULT ''`, tabella `preset_tombstone(uuid TEXT PRIMARY KEY, deleted_at INTEGER NOT NULL, deleted_by TEXT NOT NULL DEFAULT '')`. Gli autori (id del dispositivo, vuoto = questo computer) servono a conservare `updatedBy`/`deletedBy` arrivati da un altro dispositivo. I tempi dei preset diventano millisecondi: la migrazione converte `created_at` (secondi nella versione 1). | `src/db/Database.cpp:20` | S |
| 1.2 | ✅ `TimerPreset` con `createdAt`, `updatedAt`, `updatedBy`; `PresetModel` li imposta in `create/update/setOption/reset/duplicate` con `setNowProvider()` (wall clock, non il `Clock` monotono del timer). Una modifica che non cambia nulla lascia l'ora; una modifica fatta qui svuota l'autore. | `src/core/TimerPreset.h`, `src/core/PresetModel.cpp` | S–M |
| 1.3 | ✅ `PresetRepository`: legge e scrive tempi e autore; `saveAll(presets, received = {})` lascia un tombstone per ogni preset cancellato, quello ricevuto (con il suo `deletedAt`/`deletedBy`) o uno nuovo con l'ora locale; mai per il predefinito né per un preset presente, che perde il suo. `tombstones()`, scadenza a 180 giorni (`TombstoneLifetimeMs`), `setNowProvider()`. | `src/db/PresetRepository.{h,cpp}` | S–M |
| 1.4 | ✅ `PresetSyncFormat`: `toJson(data, deviceId)`, `fromJson(json) → {ok, format, data}`, `merge(local, remote, deviceId, nowMs, remoteFormat) → {result, changedLocal, changedRemote}`, `Format = 1`. | nuovi `src/sync/PresetSyncFormat.{h,cpp}` | M |
| 1.5 | ✅ `SyncCrypto`: involucro di libsodium (`encryptBlob`/`decryptBlob` XChaCha20-Poly1305, `generateKeyPair`/`publicKeyFor`, `box`/`openBox` con `crypto_box` autenticato, `verificationCode` indipendente dall'ordine (80 bit in Base32), `newRecoveryCode`/`formatRecoveryCode`/`parseRecoveryCode`, `sealWithRecovery`/`openWithRecovery`). Segreti in `SecretBytes`, cancellati con `sodium_memzero`. Il codice di recupero ha 12 bit di controllo: 28 caratteri in 7 gruppi da 4. | nuovi `src/sync/SyncCrypto.{h,cpp}` | M |
| 1.6 | ✅ **libsodium** (≥ 1.0.18) come dipendenza: `pkg_check_modules` in `CMakeLists.txt`; modulo 1.0.22 nel manifest Flatpak (verificato: non è né nel runtime KDE 6.11 né in freedesktop-sdk 25.08); `libsodium-dev` nello script AppImage, che controlla che la libreria sia nell'AppDir; job Linux della CI; pacchetti per distro in `docs/DEVELOPMENT.md`. macOS (libsodium universale e statica, DMG, job macOS della CI) nella PR #3. | `CMakeLists.txt:39`, `stream.sugo.app.yml`, `packaging/appimage/build.sh` | S–M |
| 1.7 | ✅ Test: `tst_presetsyncformat` (unione: modifica contro modifica, modifica contro cancellazione, predefinito, opzioni sconosciute, formato futuro), `tst_synccrypto` (vettori calcolati con PyNaCl, AD sbagliata rifiutata, box da mittente sbagliato rifiutato), migrazione in `tst_database`, tempi e tombstone in `tst_presetmodel` e `tst_presetrepository`. | `tests/` | M |

## Fase 2 — Server

| ID | Cosa | Effort |
|---|---|---|
| 2.1 | ✅ Progetto Worker (`wrangler`, Hono, `jose`), D1 con migrazioni, binding R2, ambienti `dev`/`prod`. D1 `sugo-sync-dev`/`sugo-sync-prod` e R2 `sugo-sync-blobs-dev`/`sugo-sync-blobs-prod` creati, migrazioni applicate. | S |
| 2.2 | ✅ Accesso con codice, verifica della sessione Clerk (JWKS con `jose`, senza `@clerk/backend`), JWT e refresh token ruotati, limite dispositivi all'approvazione. | M |
| 2.3 | ✅ Dispositivi: lista, scollegamento, chiavi cifrate, conferma (`DELETE /v1/key`), recupero. | M |
| 2.4 | ✅ Blob con ETag e `If-Match`, dimensione massima, limiti di frequenza (per token e per IP). | S–M |
| 2.5 | 🚧 Pagina `sugo.stream/link` (accesso, codice, dispositivi, cancellazione dell'account), privacy policy. In corso nel thread del sito. | M |
| 2.6 | Test del Worker: ✅ 37 test Vitest. Mancano CI e deploy automatico da `main`. | M |
| 2.7 | Produzione su `api.sugo.stream`: istanza di produzione di Clerk, `sugo.stream` su Cloudflare, deploy. | S–M |

## Fase 3 — Client di rete

| ID | Cosa | File | Effort |
|---|---|---|---|
| 3.1 | `SyncApi`: chiamate HTTP con `QNetworkAccessManager` secondo `docs/api.md` di `sugo-sync`; errori tipizzati (rete, `401`, `412`, `428`, `413`, `slow_down`, `device_limit_reached`, formato). **Rinnovo del token in un solo punto, mai due in parallelo, con il nuovo refresh token salvato in `SecretStore` prima di qualunque altra cosa** (un token ruotato riusato blocca il dispositivo). URL base iniettabile per i test. | nuovi `src/sync/SyncApi.{h,cpp}` | M |
| 3.2 | `SyncAccount`: accesso con codice (attesa con "Disconnect a device on sugo.stream" se il limite è pieno), chiave (creazione, approvazione con `crypto_box` e verifica di `from_public_key`, recupero, conferma con `DELETE /v1/key`), dispositivi in attesa da `Sugo-Pending-Devices`, lista e scollegamento. Token e chiave in `SecretStore` (`sync-refresh-token`, `sync-data-key`). | nuovi `src/sync/SyncAccount.{h,cpp}` | M–L |
| 3.3 | `SyncController` (singleton QML, `QML_ELEMENT`/`QML_SINGLETON`, `attach()` da `main.cpp` come gli altri): stato (`off`, `signingIn`, `waitingForApproval`, `idle`, `syncing`, `error`), `lastSync`, ritardo dell'invio, ricezione periodica e all'attivazione della finestra. | nuovi `src/sync/SyncController.{h,cpp}`, `src/main.cpp:174-201` | M |
| 3.4 | Collegamento a `PresetPersistence` (nuovo segnale `saved()`) e a `DataManager` (ripristino: oggi copia le colonne della versione 1, quindi i preset ripristinati arrivano con `updated_at` 0 e i tombstone restano quelli di prima; qui vanno marcati come modificati ora). | `src/db/PresetPersistence.{h,cpp}`, `src/core/DataManager.cpp` | S |
| 3.5 | ✅ `SecretStore` su macOS (`MacKeychainStore`, piano macOS 5.1) e factory comune `createSecretStore()`, usata da `CalendarSync` e dal sync. | `src/platform/SecretStore.{h,cpp}`, `src/core/CalendarSync.cpp:348` | M |
| 3.6 | Test con un finto server (`QTcpServer` locale, come i test di `CalendarSync`): accesso, limite, `412` e nuova unione, token scaduto, server irraggiungibile, formato futuro. Nessuna dipendenza da D-Bus: girano su Linux e macOS. | `tests/tst_synccontroller.cpp` | M–L |

## Fase 4 — Interfaccia

| ID | Cosa | File | Effort |
|---|---|---|---|
| 4.1 | Nuova scheda **"Sync"** nelle impostazioni, tra "Calendar" e "Advanced": accesso (codice grande, "Open in browser", copia), stato e ultimo sync, "Sync now", dispositivi con "Disconnect", codice di recupero ("Show", "Create a new one"), "Sign out". | `src/qml/pages/AppSettingsPage.qml:18-22` | M–L |
| 4.2 | Riga **"Coming soon"**: "Session history and statistics", "Task notes", con uno `StatusPill` e senza interruttore. | `AppSettingsPage.qml` | S |
| 4.3 | Dialoghi con `ConfirmDialog`: approvazione di un nuovo dispositivo (con il codice di verifica), limite raggiunto (rimanda al sito), conferma del codice di recupero, "Sign out" (i preset locali restano). | `src/qml/Main.qml`, `AppSettingsPage.qml` | M |
| 4.4 | Notifica (con `Notifier`) quando un dispositivo chiede l'approvazione e la finestra è nascosta. | `src/core/…` | S |
| 4.5 | Smoke test della scheda (`SUGO_SMOKE_PAGE`), stringhe in `po/sugo.pot`, voce nella checklist manuale, `Diagnostics` (stato del sync senza dati personali). | `tests/smoke_app.sh`, `po/`, `tests/manual-review-checklist.md`, `src/core/Diagnostics.cpp` | S–M |

## Fase 5 — Uscita

| ID | Cosa | Effort |
|---|---|---|
| 5.1 | Prova a porte chiuse con l'ambiente `dev` del Worker: `SUGO_SYNC_URL=https://sugo-sync-dev.minerale.workers.dev`. | S |
| 5.2 | Privacy policy e termini (dati conservati: email via Clerk, nomi dei dispositivi, blob cifrati; cancellazione dell'account), link dall'interfaccia. | M |
| 5.3 | README, `docs/DEVELOPMENT.md` (libsodium, `SUGO_SYNC_URL`, test), `CLAUDE.md` (`src/sync/`, `SyncController` tra i singleton). | S |
| 5.4 | Uscita in una stessa versione per Linux e macOS, dopo la prima versione macOS (S5). | — |

---

## Ordine consigliato delle PR

1. **Fase 1** (migrazione, timestamp, formato, unione, cifratura, libsodium): ~3–4 giorni, utile anche da sola.
2. **Fase 2** (server): 2.1–2.4 fatte; restano sito (2.5), CI e deploy (2.6) e produzione (2.7).
3. **Fase 3**: ~5–6 giorni.
4. **Fase 4**: ~3–4 giorni.
5. **Fase 5**, poi uscita: ~2 giorni più la prova.

Totale: circa **3–4 settimane** per una persona.

## Dopo ("In arrivo")

- **Storico e statistiche:** `session.uuid` (migrazione con riempimento delle righe esistenti) e
  `device_id`, file mensili solo in aggiunta per dispositivo (`users/<id>/sessions/<device>/2026-11.bin`).
  Le sessioni importate non emettono `workSessionRecorded`, così ogni sessione va sul calendario una sola volta.
- **Note dei task:** eventi di modifica con `updatedAt` per sessione.
- **Piano a pagamento:** `device_limit` più alto e storico; meglio un prezzo annuale (circa 10–12 €) che
  1 €/mese, per via di IVA e commissioni fisse; pagamenti con un merchant of record (Paddle, Lemon
  Squeezy) o con Clerk Billing **[verificare]**, verificati dal Worker tramite webhook.
- Altri provider (WebDAV, cartella) dietro la stessa interfaccia di trasporto.

## Da verificare

- ~~libsodium nel runtime KDE di Flatpak~~: non c'è, è un modulo del manifest. Sul Mac: universale e statica, PR #3.
- Limiti attuali di Workers, R2, D1 e del piano gratuito di Clerk. (Verificato: i rate limit dei Workers
  sono disponibili nel piano gratuito.)
- ~~Verifica della sessione Clerk dentro un Worker~~: fatta con JWKS e `jose`, senza `@clerk/backend`.
- Secret Service assente su alcuni desktop Linux (regola S4) e Portachiavi con build non firmate (piano macOS D1).
  Su macOS leggere un segreto che non esiste risponde "vuoto" anche senza Portachiavi (PR #3): per la regola S4 conta
  l'esito di una scrittura, non di una lettura.
- `QNetworkInformation` per riprovare quando torna la rete, su Linux e macOS.
