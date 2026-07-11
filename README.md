# Ponsal

**Pons** (Latein: Brücke) + **Salus** (Latein: Sicherheit, Rettung) — die Sicherheitsbrücke.

Ponsal ist ein offenes, verschlüsseltes Kommunikationsprotokoll für LoRa 868 MHz. Es ermöglicht Textkommunikation in kleinen Gruppen ohne Internet, ohne Mobilfunk, ohne App — genau dann wenn alles andere ausgefallen ist.

**Ponsal ist nicht das Gerät. Ponsal ist das Protokoll.**

---

## Warum Ponsal

Bei Katastrophen versagen Kommunikationssysteme in einer vorhersehbaren Reihenfolge. Mobilfunk-Basisstationen fallen aus sobald ihre Batteriepuffer leer sind — typisch nach 4–8 Stunden. BOS-Digitalfunk (TETRA) läuft weiter, aber ausschließlich für Einsatzkräfte. Privatpersonen sind auf sich gestellt.

Ponsal ist für genau diese Phase gebaut: Mobilfunk weg, Einsatzkräfte ausgelastet, Privatpersonen müssen untereinander kommunizieren ohne jede Infrastruktur.

Das Protokoll ist kein Ersatz für Notruf 112 und kein Einsatzkräfte-Tool. Es ist ein Werkzeug für Nachbarschaftskommunikation — die horizontale Ebene zwischen Privatpersonen, nicht die vertikale zwischen Bürger und Behörde.

### Warum nicht Meshtastic

Meshtastic ist das bekannteste Projekt in diesem Bereich — kostenlos, tausende Entwickler, aktive Community. Ponsal ist aus zwei Gründen ein eigenständiges Protokoll:

**Stabilität.** Meshtastic hatte seit 2022 die Versionen 2.0 bis 2.7, alle 2–3 Monate eine neue, mit Breaking Changes zwischen Major-Versionen. Ein Gerät das 2026 eingerichtet ins Regal gestellt wird, kommuniziert 2030 nicht mehr zuverlässig mit aktualisierten Geräten. Für ein Notfallgerät das jahrelang lagert ist das ein K.O.-Kriterium. Das Ponsal-Protokoll ist eingefroren — ein Gerät von 2026 kommuniziert mit einem Gerät von 2036.

**Zielgruppe.** Meshtastic erfordert Firmware flashen, App installieren, Bluetooth koppeln, Region einstellen. Für einen Nicht-Techniker unter Stress inakzeptabel. Ponsal ist für Oma im Kerzenlicht gebaut, nicht für Techniker.

```
Meshtastic → Techniker → App → Smartphone → aktive Pflege erforderlich
                    ↕
              [Lücke]
                    ↕
Ponsal      → Nicht-Techniker → Browser → jedes WLAN-Gerät → einmal einrichten, jahrelang bereit
```

---

## Protokoll — Kernmerkmale

### Paketformat

Pakete sind 61–255 Byte. Die Größe variiert nur mit dem Nachrichtentext — alle anderen Felder sind fix. Maximum ist die Hardware-Grenze des SX1262.

```
Header (8 Byte, Klartext):
  Version     1B   0x01
  NetzwerkID  2B   SHA-256(PSK)[0:2] — Schnellfilter vor Entschlüsselung
  NachrichtID 4B   NodeID(2B) + Sequenznummer(2B)
  Flags       1B   Bit 0=Chat, Bit 2=Infokanal, Rest reserviert

IV (12 Byte, Klartext):
  Zufällig pro Paket, nie wiederverwendet

Payload (AES-256-GCM verschlüsselt):
  Name       20B fix   Absendername, UTF-8, null-padded
  Trenner     1B fix   0x00
  Timestamp   4B fix   Unix-Zeit, little-endian
  Text      var        Max. 194 Byte, null-terminiert

GCM Tag (16 Byte):
  Authentifizierung — schlägt bei falschem PSK, Manipulation oder falschem IV fehl
```

### Verschlüsselung

AES-256-GCM verschlüsselt und authentifiziert jede Nachricht. Der komplette Header ist als AAD eingebunden — auch unverschlüsselte Felder können nicht manipuliert werden ohne dass der GCM-Tag fehlschlägt. Der Netzwerkschlüssel (PSK) ist 32 Byte Zufall und definiert die Netzwerkzugehörigkeit. Wer ihn nicht kennt, kann weder lesen noch senden.

### Mesh

Flooding mit Duplikatfilter — jede Nachricht wird einmal weitergeleitet, Duplikate innerhalb von 60 Minuten werden verworfen. Kein zentraler Knoten, kein Store-and-Forward. Fällt ein Gerät aus, läuft der Rest weiter.

Nur Geräte die einen Kanal entschlüsseln können leiten dessen Nachrichten weiter — kein fremder Duty-Cycle-Verbrauch, keine unbeabsichtigten Relays.

### Pairing

Curve25519 ECDH über LoRa — das vorhandene Funkmedium für den Schlüsseltausch. Beide Geräte tauschen öffentliche Schlüssel aus, berechnen unabhängig einen gemeinsamen Schlüssel und leiten daraus eine 6-stellige PIN ab die auf beiden Geräten angezeigt wird. Nur wenn der Nutzer die PINs vergleicht und bestätigt, wird der PSK verschlüsselt übertragen. Der PSK kommt nie im Klartext über die Luft.

### Funk-Profile

| Profil | SF | BW | Frequenz | Duty Cycle |
|---|---|---|---|---|
| Standard | SF9 | 125 kHz | 868.0 MHz | 1% |
| Reichweite | SF12 | 125 kHz | 868.0 MHz | 1% |
| Organisation | SF9 | 125 kHz | 869.525 MHz | 10% |
| Stadt | SF7 | 125 kHz | 868.0 MHz | 1% |

Geräte mit verschiedenen Profilen können nicht miteinander kommunizieren — bewusste Trennung z.B. zwischen Privat- und Organisationsnetz.

### Was Ponsal bewusst nicht kann

Sprachnachrichten, Bilder, GPS-Tracking, Internet-Bridge, automatische Updates, große Netzwerke (optimal 3–20 Geräte, maximum ~50). Das sind keine Versehen — das sind Entscheidungen.

---

## ProjektX — Referenzimplementierung

ProjektX ist das erste Ponsal-kompatible Gerät. Hardware-Basis: Heltec WiFi LoRa 32 V3 (ESP32-S3 + SX1262 + 128×64 OLED). Heltec WiFi LoRa 32 V4 (gleicher Funkchip, ESP32-S3 mit PSRAM) wird ebenfalls unterstützt.

Kernversprechen: Strom rein — sofort einsatzbereit. Nutzer verbinden sich per WLAN und chatten über den Browser. Keine App, kein Konto, kein Internet.

### Hardware-Agnostizität bewiesen

ESP32-WROOM-32U + SX1276 ↔ Heltec WiFi LoRa 32 V3 ↔ Heltec WiFi LoRa 32 V4 (beide ESP32-S3 + SX1262) — mehrere Mikrocontroller-Generationen, zwei verschiedene LoRa-Chip-Familien, ein Protokoll.

### Schnellstart

**Hardware:**
- Heltec WiFi LoRa 32 V3 (~25€) — empfohlen, am längsten im Feld erprobt
- USB-C Kabel + Stromversorgung

**Firmware flashen:**
```bash
git clone https://github.com/DannysIobroker/Ponsal_Release
cd Ponsal_Release
pio run -e heltec_v3 -t upload
pio run -e heltec_v3 -t uploadfs
```

Heltec WiFi LoRa 32 V4 wird ebenfalls unterstützt (`heltec_v4_dev`-Environment) — Pinbelegung für LoRa, OLED, Button und Akkuspannung ist gegen echte Hardware verifiziert, aber neuer als V3 und noch ohne eigenes Production-Environment.

**Benutzen:**
1. Gerät einschalten
2. WLAN-Zugangsdaten (SSID + Passwort) vom Display ablesen — das Gerät zeigt
   sie 60 Sekunden lang nach jedem Start an
3. Browser öffnet Chat-Interface
4. Fertig

---

## Protokoll-Spezifikation

Die vollständige Spezifikation liegt in [`docs/ponsal-protokoll-spec.md`](docs/ponsal-protokoll-spec.md).

Das Protokoll ist hardware-agnostisch und eingefroren. Drittanbieter können eigene konforme Geräte bauen — alle kommunizieren miteinander.

Was nie geändert wird:
```
Maximale Paketgröße:     255 Byte
Verschlüsselung:         AES-256-GCM
PSK:                     32 Byte Zufall
NachrichtID-Format:      NodeID(2B) + Sequenz(2B)
Feldgrößen Payload:      Name 20B / Trenner 1B / Timestamp 4B — fix
Keine Fragmentierung
Kein Store-and-Forward
Rückwärtskompatibilität: oberstes Gebot
```

---

## Entwicklung

Protokolldesign, Firmware und Dokumentation wurden mit Unterstützung von
[Claude](https://claude.ai) und [Claude Code](https://claude.ai/code) (Anthropic) entwickelt.

---

## Lizenz

**Protokoll-Spezifikation** — [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/)
Frei verwendbar, auch kommerziell. Namensnennung erforderlich:
> „Basiert auf dem Ponsal-Protokoll — © 2026 Danny Sonntag, CC BY 4.0"

**Firmware (ProjektX)** — [Apache 2.0](https://www.apache.org/licenses/LICENSE-2.0)
Frei verwendbar, auch kommerziell. Namensnennung erforderlich. Enthält expliziten Patentschutz.

Abhängigkeiten: siehe [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)

---

© 2026 Danny Sonntag
