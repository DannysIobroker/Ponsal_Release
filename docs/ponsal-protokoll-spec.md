# Ponsal — Protokoll-Spezifikation

---

## 1. Paketstruktur

Pakete sind **unterschiedlich groß je nach Textlänge**, aber mit **fixen Feldgrößen** für Name, Trenner und Timestamp. Maximum ist 255 Byte (Hardware-Grenze SX1262). Keine Fragmentierung.

Kein Längenfeld im Header nötig — LoRa überträgt die Paketlänge auf Hardware-Ebene. Der Empfänger liest sie über `radio.getPacketLength()`.

**Validierung beim Empfang:**
```
Paketlänge < 61 Byte (PACKET_MIN: Header 8 + IV 12 + Name 20 + Trenner 1 + Timestamp 4 + GCM 16) → verwerfen
Paketlänge > 255 Byte → verwerfen (sollte nie vorkommen)
```

**Motivation für variable Gesamtlänge (nur beim Textfeld):** Airtime-Reduktion. Bei SF12 kostet ein 255-Byte-Paket ~5 Sekunden Sendezeit. Bei 1% Duty Cycle sind das maximal ~7 Nachrichten pro Stunde. Nur das Textfeld ist variabel — Name, Trenner und Timestamp sind fix, damit der Empfänger über feste Offsets parsen kann, ohne den Trenner suchen zu müssen.

```
Offset   Länge     Feld          Beschreibung
──────────────────────────────────────────────────────────────
0        1B        Version       Protokollversion, aktuell: 0x01
1        2B        NetzwerkID    SHA-256(PSK), erste 2 Byte
3        4B        NachrichtID   NodeID(2B) + Sequenz(2B)
7        1B        Flags         Bit 0: Chat, Bit 1: reserviert, Bit 2: Infokanal, Bit 3–7: reserviert
──────────────────────────────────────────────────────────────
8        12B       IV            Zufälliger AES-GCM Initialisierungsvektor
──────────────────────────────────────────────────────────────
20       variabel  Payload       AES-256-GCM verschlüsselt
  20     20B       Name          Absendername, UTF-8, null-padded — FIX, immer 20 Byte
  40     1B        Trenner       0x00 — FIX, immer Offset 40
  41     4B        Timestamp     Unix-Zeit (Sekunden), vom Browser, little-endian — FIX, immer Offset 41
  45     variabel  Text          Nachrichtentext, UTF-8, null-terminiert, max. 194 Byte
──────────────────────────────────────────────────────────────
variabel 16B       GCM Tag       Authentifizierungstag
──────────────────────────────────────────────────────────────
Gesamt: 61–255 Byte (61 = leerer Text, 255 = maximaler Text)
```

**Parsing beim Empfang:** Über feste Offsets, NICHT über Trennersuche. Name = Byte 0–19, Trenner = Byte 20, Timestamp = Byte 21–24, Text = ab Byte 25 bis zum ersten `0x00` oder Payload-Ende.

> **Hinweis (2026-06-21):** Diese fixen Offsets waren von Anfang an spezifiziert, wurden in der Implementierung aber bis zum 19.06. nicht korrekt umgesetzt — der Name wurde dort variabel (per `strnlen`) statt fix 20 Byte gesendet, mit Trennersuche statt fixem Offset beim Parsing. Das blieb lange unbemerkt, weil GCM-Auth nur die Integrität der tatsächlich gesendeten Bytes prüft, nicht die Konformität zum spezifizierten Format — zwei Geräte mit identischem (fehlerhaftem) Code kommunizieren GCM-korrekt, auch wenn beide vom Protokoll abweichen. Sichtbar wurde der Fehler erst bei kurzen Gerätenamen, wo die Differenz zwischen gepaddeter und tatsächlicher Länge groß genug wurde, dass der Timestamp an der falschen Stelle gelesen wurde. Korrigiert und getestet (Grenzfälle: 4-Zeichen-Name, 20-Zeichen-Name, leerer Text auf Protokollebene). Alle 3 Geräte laufen seither auf dem korrigierten, fixen Format.

---

## 2. Felder im Detail

### Version (1 Byte)

Aktuell: `0x01`.

Empfänger mit unbekannter Version ignorieren das Paket still. Rückwärtskompatibilität ist oberstes Gebot.

### NetzwerkID (2 Byte)

```
NetzwerkID = SHA-256(PSK)[0:2]
```

Wird **nie übertragen, nie gespeichert, nie im QR-Code**. Jedes Gerät berechnet sie lokal aus dem PSK.

Zweck: Schnellfilter vor der Entschlüsselung.

```
Paket empfangen
→ NetzwerkID im Header == eigene NetzwerkID?
   NEIN → sofort verwerfen
   JA  → Entschlüsselung versuchen
```

Kein Sicherheitsmerkmal — liegt im Klartext. Die echte Sicherheit liegt beim GCM Tag.

### NachrichtID (4 Byte)

```
[NodeID 2B][Sequenznummer 2B]
```

Eindeutig pro Nachricht im Netz. Wird vom Duplikatfilter und der Echo-Vermeidung verwendet.

### Flags (1 Byte)

```
Bit 0: Chat      — normale Textnachricht
Bit 1:           — reserviert (war ACK, verworfen 2026-06-13)
Bit 2: Infokanal — Nachricht mit Ed25519-Signatur (siehe Abschnitt 13)
Bit 3–7:         — reserviert, müssen 0 sein
```

### IV (12 Byte)

Zufällig generiert pro Nachricht. `esp_random()` auf ESP32, `crypto.getRandomValues` im Browser. Nie wiederverwendet, nie abgeleitet.

### Payload (verschlüsselt, fixe Feldgrößen außer Text)

```
Name:      20 Byte, UTF-8, null-padded — FIX, immer 20 Byte
Trenner:   1 Byte,  0x00 — FIX
Timestamp: 4 Byte,  Unix-Zeit in Sekunden, little-endian — FIX
Text:      variabel, UTF-8, null-terminiert, maximal 194 Byte
```

Maximale Textlänge: 255 − 8 (Header) − 12 (IV) − 16 (GCM Tag) − 20 (Name) − 1 (Trenner) − 4 (Timestamp) = **194 Byte**.

Name wird vor dem Senden auf 20 Byte hart abgeschnitten, kürzere Namen werden mit `0x00` aufgefüllt (null-padded).

### Timestamp (4 Byte)

Unix-Zeit in Sekunden (32 Bit, gültig bis Jahr 2106), little-endian.

**Quelle:** Browser liefert `Date.now()` (ms) via `ts`-Parameter beim POST /send. ESP rechnet um: `ts / 1000` (ganzzahlig, abschneiden). Bei `ts=0` oder fehlendem Parameter: Timestamp 0 im Payload (Empfänger behandelt als "unbekannt").

**Plausibilitätsprüfung beim Empfang** (nur wenn eigene Zeitbasis vorhanden):
```
Delta zwischen empfangenem Timestamp und eigener Schätzzeit > 3600s
→ unplausibel → Schätzzeit verwenden statt rohem Wert
```

### GCM Tag (16 Byte)

AES-256-GCM liefert Verschlüsselung und Authentifizierung. Tag schlägt fehl bei Manipulation, falschem PSK oder falschem IV → Paket verwerfen.

---

## 3. NodeID

```
NodeID = SHA-256(MAC-Adresse)[0:2]
```

Bei jedem Start neu berechnet, nicht gespeichert.

**Kollisionsrisiko:** 2 Byte = 65.535 mögliche Werte. Bei 20 Geräten ~1,2% Kollisionswahrscheinlichkeit. Kollision führt zu gelegentlich verworfenen Nachrichten (Duplikatfilter). Graceful degradation, kein Datenverlust.

---

## 4. Sequenznummer

2 Byte, 0–65.535, Wraparound definiert. Persistent in NVS — Rotations-Ringpuffer (16 Slots). Bei jedem Send sofort persistiert, bei Boot höchster Wert +1 als Startwert.

**Wraparound-sichere Maximalwert-Ermittlung:**
```
(uint16_t)(a - b) < 32768 → a ist neuer als b
```

---

## 5. Verschlüsselung

**Algorithmus:** AES-256-GCM

**Schlüssel:** PSK, 32 Byte, direkt als AES-Schlüssel. Kein PBKDF2 — PSK ist bereits volle Entropie.

**IV:** 12 Byte, zufällig pro Nachricht.

**Tag:** 16 Byte, am Ende des Pakets.

**AAD:** Kompletter Header (8 Byte). Header wird nicht verschlüsselt, aber authentifiziert — Manipulation des Headers schlägt beim GCM Tag fehl.

**Wichtig:** GCM-Auth schützt die Integrität der gesendeten Bytes, nicht die Konformität zum spezifizierten Paketformat. Format-Konformität muss separat über Tests sichergestellt werden.

---

## 6. PSK

32 Byte Zufallsdaten. Generiert mit `esp_fill_random()` auf dem Gerät beim Erstellen eines Kanals.

**QR-Format normaler Kanal:**
```json
{"n": "Familie", "k": "<PSK base64url>", "v": 1}
```

**QR-Format Infokanal:**
```json
{"n": "Dorfnachrichten", "k": "<PSK base64url>", "p": "<public key base64url>", "v": 1, "t": "i"}
```

Kodiert als: `ponsal://join#<base64url(json)>`

---

## 7. NetzwerkID-Berechnung

```
NetzwerkID = SHA-256(PSK)[0:2]
```

Lokal berechnet, nie gespeichert, nie übertragen.

---

## 8. Duplikatfilter

```
Paket empfangen
→ NachrichtID in Liste UND Alter < 60 Min? → verwerfen
→ sonst: verarbeiten, weiterleiten, in Liste eintragen
```

Ringpuffer im RAM. `DEDUP_SIZE=3600` Einträge, 8 Byte/Eintrag (4B NachrichtID + 4B `millis()`-Zeitstempel). Speicherbedarf: ~28 KB.

`millis()`-Wraparound wird durch Differenzbildung korrekt behandelt.

---

## 9. ACK-System — verworfen

ACK wurde diskutiert und verworfen (2026-06-13).

**Begründung:** ACK kann genauso verloren gehen wie Nachrichten. "Nicht bestätigt" bedeutet dann nicht "nicht angekommen", sondern nur "ACK nicht empfangen" — im Notfall ist falsches Sicherheitsgefühl schlimmer als kein Feedback.

Flag-Bit 1 bleibt reserviert, wird nicht verwendet.

---

## 10. Speicherarchitektur

Zwei unabhängige NVS-Ringpuffer in der `msgstore`-Partition (40 KB):

| Ringpuffer | Slots | Zweck |
|---|---|---|
| Chatverlauf | 50 Slots (`msg_slot_0`…`msg_slot_49`) | Persistenz über Stromausfall |
| Sequenznummer | 16 Slots (`seq_slot_0`…`seq_slot_15`) | Wraparound-sichere Fortsetzung nach Neustart |

PSKs, Einstellungen und PIN bleiben in der regulären NVS-Partition.

---

## 11. Mesh-Routing

Kein gerichtetes Routing. Flooding mit Duplikatfilter und Echo-Vermeidung.

```
Paket empfangen
→ NetzwerkID prüfen → passt nicht → verwerfen
→ NachrichtID im 60-Min-Duplikatfilter? → verwerfen
→ entschlüsseln → GCM Tag fehlgeschlagen → verwerfen
→ für eigene Weboberfläche anzeigen
→ NodeID in NachrichtID == eigene NodeID? → NICHT weiterleiten (Echo-Vermeidung)
→ weiterleiten: Paket UNVERÄNDERT in die Mesh Send Queue einreihen (12 Slots, gemeinsam mit eigenen
  Nachrichten), zufälliger Delay 0–1000ms vor dem tatsächlichen Senden
```

**Korrektur 2026-07-21:** Vorherige Fassung dieses Abschnitts behauptete "Bei `LORA_SEND_DUTYCYCLE` oder `LORA_SEND_CHANNEL_BUSY`: Weiterleitung still verworfen, kein Retry" — das entsprach nicht (mehr) dem Code. Tatsächliches Verhalten über die Mesh Send Queue:
- `LORA_SEND_DUTYCYCLE` / `LORA_SEND_CHANNEL_BUSY` führen NICHT zum sofortigen Verwerfen — der Eintrag bleibt in der Queue und wird in der nächsten Verarbeitungsrunde erneut versucht.
- Weiterleitungen (RELAY) haben ein TTL von 5 Minuten ab Einreihung — erst wenn diese Zeit ohne erfolgreiches Senden verstreicht, wird der Eintrag verworfen.
- Bei voller Queue (12/12) wird bei Bedarf der älteste RELAY-Eintrag verdrängt, um Platz für Neues zu schaffen (auch das ohne je gesendet worden zu sein).
- Eigene Nachrichten (OWN) haben kein TTL und werden nie verdrängt — sie warten nötigenfalls unbegrenzt.

---

## 12. Duty Cycle

Europa, 868 MHz: 1% Duty Cycle gesetzlich vorgeschrieben. Maximale Sendeleistung: 25 mW = 14 dBm.

**Formel:** `Toff = Ton × (1/DC - 1)`. Bei 1% DC: `Toff = 99 × Airtime`.

**Funk-Profile (final):**

| Profil | SF | BW | Frequenz | Duty Cycle |
|---|---|---|---|---|
| 0 Standard | SF9 | 125 kHz | 868.0 MHz | 1% |
| 1 Reichweite | SF12 | 125 kHz | 868.0 MHz | 1% |
| 2 Organisation | SF9 | 125 kHz | 869.525 MHz | 10% |
| 3 Stadt | SF7 | 125 kHz | 868.0 MHz | 1% |

CAD (Channel Activity Detection) ist vor jedem Senden implementiert: 3 Versuche, Backoff 20–500ms zufällig. Ist der Kanal bei allen 3 Versuchen belegt, wird nicht gesendet (`LORA_SEND_CHANNEL_BUSY`, Retry über die Mesh Send Queue, siehe Abschnitt 11). Nur bei einem echten CAD-Scan-Fehler (weder "belegt" noch "frei" erkannt) wird trotzdem gesendet, damit ein CAD-Bug nicht das Gerät blockiert — das ist ein anderer Fall als "Kanal belegt".

---

## 13. Infokanal

Optionaler Kanaltyp für Einweg-Kommunikation, zurückgestellt. Flags-Bit 2 reserviert. Empfänger ohne Infokanal-Implementierung verwerfen still — rückwärtskompatibel.

---

## 14. Pairing-Protokoll

### Übersicht

Eigenständige Paketklasse — kein PSK, keine NetzwerkID, kein normaler GCM-Tag. Läuft über denselben LoRa-Funk, folgt aber einem eigenen Format.

Grundlage: Curve25519-ECDH (Diffie-Hellman auf Elliptischen Kurven via mbedTLS).

### Pairing-Paketformat

```
Offset  Länge  Feld
0       1B     Magic: 0x50
1       1B     Typ:
               0x01 = ECDH_INIT      Geber → öffentlicher Schlüssel
               0x02 = ECDH_RESP      Nehmer → öffentlicher Schlüssel
               0x03 = PSK_TRANSFER   Geber → verschlüsselter PSK
               0x04 = PSK_ACK        Nehmer → Empfangsbestätigung
               0x05 = NAME_TRANSFER  Geber → Name + Lock-Flag
               0x06 = NAME_ACK       Nehmer → Name empfangen
2       4B     Session-ID
6       var    Payload
```

PSK_TRANSFER-Verschlüsselung: AES-256-GCM, Key = SHA-256(rawSharedSecret), AAD = Session-ID (4B).

**Lock-Flag (letztes Byte von NAME_TRANSFER):**
- `0x00` = frei änderbar (Standard)
- `0x01` = gesperrt

Rückwärtskompatibel: NAME_TRANSFER ohne Lock-Byte wird als `0x00` interpretiert.

### Ablauf

```
Geber                           Nehmer
  │── ECDH_INIT ──────────────────►│
  │◄─────────────────── ECDH_RESP ─│
  │  [beide PIN berechnen + anzeigen]
  │  [Nutzer bestätigt am Geber]
  │── PSK_TRANSFER ───────────────►│
  │◄──────────────────── PSK_ACK ──│
  │── NAME_TRANSFER (Name+Lock) ───►│
  │◄─────────────────── NAME_ACK ──│
```

### PIN-Ableitung

```cpp
uint8_t hash[32];
mbedtls_sha256(rawSharedSecret, 32, hash, 0);
uint32_t pin = 0;
memcpy(&pin, hash, 4);
pin = pin % 1000000;  // 6-stellig
```

### Retransmission & Timeout

- Retransmit alle 30s
- Gesamttimeout: 3 Minuten
- NAME_ACK-Timeout: 60s nach NAME_TRANSFER

---

## Entscheidungen die nie geändert werden

```
Maximale Paketgröße:  255 Byte (Hardware-Grenze)
Verschlüsselung:      AES-256-GCM
PSK-Mechanismus:      32 Byte Zufall, Verteilung per Pairing
NachrichtID-Format:   NodeID(2B) + Sequenz(2B)
Kanalkonzept:         PSK definiert Netzwerkzugehörigkeit
Feldgrößen Payload:   Name 20B / Trenner 1B / Timestamp 4B — fix
Keine Fragmentierung
Kein Store-and-Forward
Rückwärtskompatibilität: oberstes Gebot
```
