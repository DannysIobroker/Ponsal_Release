# Changelog

Alle wesentlichen Änderungen werden in dieser Datei dokumentiert.
Format: [Keep a Changelog](https://keepachangelog.com/de/1.1.0/)

---

## [Unveröffentlicht] — fix/wifi-random-password

### Sicherheit
- WLAN-Passwort wird jetzt beim ersten Boot zufällig generiert (esp_random(),
  12 Zeichen) und in NVS gespeichert — kein statischer Default mehr im Code
- Einstellungs-PIN wird jetzt beim ersten Boot zufällig generiert (6-stellig,
  esp_random()) — nicht mehr aus MAC-Adresse abgeleitet
- Werksreset generiert beide Werte neu statt sie aus der MAC zu berechnen

### Geändert
- Boot-Sequenz zeigt jetzt 3 Screens für 60 Sekunden (vorher 2 Screens/30s):
  WLAN-Zugang → Einstellungs-PIN → IP-Adresse, je 5 Sekunden, 4× wiederholt
- Gehäuse-Aufkleber mit Gerätedaten entfällt — Display ist alleinige Quelle
  für WLAN-Passwort und PIN (beide ändern sich bei jedem Werksreset)
- Einstellungs-PIN und WLAN-Passwort können in config.html unabhängig
  voneinander auf dem Display deaktiviert werden (Default: sichtbar)

### Behoben
- Display blieb nach Werksreset / erstem Boot ohne konfigurierten Kanal
  dauerhaft dunkel (displayTick() wurde im !pskLoaded-Zweig nie aufgerufen)
- Button-Reaktion und Display-Updates waren auf Heltec V3 bis zu 7,5 Sekunden
  verzögert (loraReceive() auf SX1262 blockierte ohne Timeout-Parameter bis
  zu 500% der LoRa-Airtime)

---

## [0.1.0] — 2026-06-28 — Erster öffentlicher Release

### Protokoll
- Ponsal-Protokoll v1: AES-256-GCM, festes Paketformat (61–255 Byte),
  NetzwerkID als Schnellfilter, NachrichtID aus NodeID + Sequenznummer
- Flooding-Mesh mit Duplikatfilter (60 Minuten, 256 Einträge)
- Curve25519 ECDH über LoRa für PSK-Übertragung, 6-stellige Verifikations-PIN
- 4 Funk-Profile: Standard (SF9), Reichweite (SF12), Organisation (SF9/869.525),
  Stadt (SF7)
- Duty-Cycle-Einhaltung: 1% auf 868 MHz, 10% auf 869.525 MHz

### Firmware (ProjektX)
- Referenzimplementierung für Heltec WiFi LoRa 32 V3 (ESP32-S3 + SX1262)
- Zweite Hardware-Variante: ESP32-WROOM-32U + SX1276 (beweist Hardware-Agnostizität)
- WLAN Access Point ohne Internet, Chat-Interface im Browser, kein App-Download
- Captive Portal (Best-Effort: Android, Windows, macOS — nicht iOS HTTPS)
- Werksreset: 10s Taster halten, 2-Schritt-Bestätigung, löscht alles
- OLED-Zustandsmaschine: Boot-Sequenz, Dauerbetrieb, Fehlerzustände,
  Pairing-Overlay, kein Kanal konfiguriert
- Chatverlauf-Persistenz: NVS-Ringspeicher, 50 Nachrichten pro Gerät
- Sequenznummer-Persistenz: überlebt Neustart, wraparound-sicher
- Kanalspezifische Absendernamen, bis zu 8 Kanäle, Lock-Flag
- Pairing: ECDH-Schlüsseltausch, Rename-UI bei Namenskollision, Re-ACK
- RSSI/SNR-Ampel im Debug-Modus (localStorage px_debug=1)
- Autoreply auf "antworte": RSSI/SNR-Messwerte als Antwort, zufälliges Delay
  zur Kollisionsvermeidung
