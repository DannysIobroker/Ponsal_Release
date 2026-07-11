# Building ProjektX

## Voraussetzungen

- [PlatformIO](https://platformio.org/) CLI oder VS Code Extension
- Python 3

## Environments

| Environment | Hardware |
|---|---|
| `heltec_v3` | Heltec WiFi LoRa 32 V3 (ESP32-S3 + SX1262 + OLED) |
| `heltec_v3_prod` | Heltec WiFi LoRa 32 V3 — Production (kein Serial, kein Debug) |
| `esp32_sx1276` | ESP32-WROOM-32U + RFM95W (SX1276) |
| `heltec_v4_dev` | Heltec WiFi LoRa 32 V4 (ESP32-S3 + SX1262 + OLED) — kein Production-Environment bisher |

## Firmware flashen

```bash
pio run -e heltec_v3 -t upload --upload-port /dev/ttyUSB0
```

## Web-Interface flashen (SPIFFS)

Nur nötig wenn `data/config.html` geändert wurde:

```bash
pio run -e heltec_v3 -t uploadfs --upload-port /dev/ttyUSB0
```

## Production-Build

Der `heltec_v3_prod`-Build enthält kein `-DDEVELOPMENT` — Serial-Ausgaben und
Debug-Endpunkte sind kompiliert deaktiviert. Für echte Geräte immer dieses
Environment verwenden.

## WLAN-Passwort ändern

Das Standard-WLAN-Passwort (`WIFI_PASS_DEFAULT` in `src/main.cpp`) vor dem
ersten Flash auf einen eigenen Wert setzen, oder nach dem ersten Start über
die Konfigurationsoberfläche unter `http://192.168.4.1/config.html` ändern.

## Partitionsschema

`partitions.csv` **nicht ändern** nach dem ersten Flash — sonst geht der
NVS-Inhalt (PSK, Gerätename, Einstellungen, Chatverlauf) verloren.
