#include "lora_hal.h"
#include "hardware_config.h"
#include <RadioLib.h>
#include <Arduino.h>
#include "serial_log.h"

// ── SX1276 (ESP32-WROOM-32U + RFM95W) ─────────────────────────
#ifdef HARDWARE_ESP32_SX1276
  static SPIClass loraSPI(VSPI);
  static SX1276 radio = new Module(LORA_NSS, LORA_DIO0, LORA_RST, LORA_DIO1, loraSPI);
#endif

// ── SX1262 (Heltec V3) ────────────────────────────────────────
#ifdef HARDWARE_HELTEC_V3
  static SX1262 radio = new Module(LORA_NSS, LORA_DIO1, LORA_RST, LORA_BUSY);
#endif

// ──────────────────────────────────────────────────────────────
// Preset-Definitionen
//
// Frequenz, SF, Bandbreite, Coding Rate
//
// PRESET_STANDARD     — SF9,  ausgewogen, Subband M (1% DC)
// PRESET_REICHWEITE   — SF12, maximale Reichweite zum Testen, Subband M (1% DC)
//                        WARNUNG: ~250s Pflichtpause pro Nachricht bei 55 Byte
// PRESET_ORGANISATION — SF9,  eigene Frequenz Subband P (10% DC) — bewusste
//                        Trennung vom Privatnetz. 869.525 MHz ist die Mitte
//                        von Subband P (869.40-869.65 MHz). Diese Frequenzwahl
//                        ist noch nicht strategisch final abgestimmt.
// PRESET_STADT        — SF7,  "Schnelle Nachrichten", kurze Airtime, Subband M (1% DC)
// ──────────────────────────────────────────────────────────────
struct PresetConfig {
    float   freqMHz;
    float   bwKHz;
    uint8_t sf;
    uint8_t cr;
    uint8_t dutyCycleLimitPercent;
};

static const PresetConfig PRESETS[PRESET_COUNT] = {
    /* STANDARD     */ { 868.0, 125.0,  9, 5, 1  },
    /* REICHWEITE   */ { 868.0, 125.0, 12, 5, 1  },
    /* ORGANISATION */ { 869.525, 125.0, 9, 5, 10 },
    /* STADT        */ { 868.0, 125.0,  7, 5, 1  },
};

static uint8_t currentPreset = PRESET_STANDARD;

// ── Duty-Cycle-Tracking ─────────────────────────────────────────
// Pflichtpause: Toff = Ton * (1/dutycycle - 1)
// Bei 1%:  Toff = Ton * 99
// Bei 10%: Toff = Ton * 9
static unsigned long dutyCycleReleaseTime = 0;  // millis() Zeitpunkt
static unsigned long lastAirtimeMs = 0;
// lastSNR: statische Variable — wird in loraReceive() nach jedem Empfang
// überschrieben. loraLastSNR() liefert den Wert des zuletzt empfangenen Pakets.
static int           lastSNR = 0;

// ──────────────────────────────────────────────────────────────
// Radio mit Preset-Parametern initialisieren/neu konfigurieren
// ──────────────────────────────────────────────────────────────
static bool spiInitialized = false;

static bool applyPreset(uint8_t preset) {
    if (preset >= PRESET_COUNT) return false;
    const PresetConfig &cfg = PRESETS[preset];

    int state;
#ifdef HARDWARE_ESP32_SX1276
    if (!spiInitialized) {
        loraSPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);
        spiInitialized = true;
    }
    state = radio.begin(cfg.freqMHz, cfg.bwKHz, cfg.sf, cfg.cr, 0x12, 14, 8, 0);
    if (state == RADIOLIB_ERR_NONE) {
        // explicitHeader() zwingend nach begin() — SX1276 sendet sonst mit
        // implizitem Header (feste Länge), was Pakete variabler Länge unmöglich macht.
        // SX1276 läuft im Polling-Modus: startReceive() NICHT aufrufen — verursacht
        // Tight-Loop, weil RadioLib dort keinen Interrupt, sondern Poll erwartet.
        radio.explicitHeader();
    }
#endif
#ifdef HARDWARE_HELTEC_V3
    state = radio.begin(cfg.freqMHz, cfg.bwKHz, cfg.sf, cfg.cr, 0x12, 14, 8);
    if (state == RADIOLIB_ERR_NONE) {
        // setDio2AsRfSwitch(true) zwingend nach begin() auf SX1262 (Heltec V3):
        // DIO2 steuert den internen RF-Switch; ohne diesen Aufruf sendet der
        // Chip nicht (kein Signal auf der Antenne, kein Fehler-Returncode).
        radio.setDio2AsRfSwitch(true);
    }
#endif

    if (state != RADIOLIB_ERR_NONE) {
        logPrintf("[LoRa HAL] Preset %d fehlgeschlagen, state=%d\n", preset, state);
        return false;
    }

    currentPreset = preset;
    logPrintf("[LoRa HAL] Preset %d aktiv: %.3f MHz, SF%d, BW%.1fkHz, CR4/%d, DC%d%%\n",
        preset, cfg.freqMHz, cfg.sf, cfg.bwKHz, cfg.cr, cfg.dutyCycleLimitPercent);

    return true;
}

// ──────────────────────────────────────────────────────────────
bool loraInit(uint8_t preset) {
    if (!applyPreset(preset)) return false;
    dutyCycleReleaseTime = 0;  // frisch starten
    return true;
}

bool loraSetPreset(uint8_t preset) {
    if (preset == currentPreset) return true;
    logPrintf("[LoRa HAL] Wechsle Preset %d -> %d\n", currentPreset, preset);
    bool ok = applyPreset(preset);
    if (ok) {
        // Duty-Cycle-Sperre bleibt erhalten — sie bezieht sich auf Subband,
        // nicht auf Preset. Bei Wechsel zwischen Subbändern (z.B. Standard
        // -> Organisation) wäre das eigentlich getrennt zu tracken.
        // Vereinfachung: eine globale Sperre, konservativ.
    }
    return ok;
}

// ──────────────────────────────────────────────────────────────
// Channel Activity Detection — prüft ob die Frequenz gerade
// von jemand anderem benutzt wird, bevor gesendet wird.
//
// Rückgabe: true = Kanal frei, false = Kanal belegt (auch nach Backoff)
// ──────────────────────────────────────────────────────────────
static bool channelClearToSend() {
    const int MAX_ATTEMPTS = 3;

    for (int attempt = 0; attempt < MAX_ATTEMPTS; attempt++) {
        int state = radio.scanChannel();

        if (state == RADIOLIB_LORA_DETECTED) {
            // Kanal belegt — zufälliger Backoff 20-500ms
            unsigned long backoff = 20 + (esp_random() % 481);
            logPrintf("[LoRa HAL] CAD: Kanal belegt (Versuch %d/%d), warte %lums\n",
                attempt + 1, MAX_ATTEMPTS, backoff);
            delay(backoff);
            continue;
        } else if (state == RADIOLIB_CHANNEL_FREE) {
            return true;
        } else {
            // Unerwarteter CAD-Fehler — im Zweifel senden lassen,
            // sonst blockiert ein CAD-Bug das ganze Gerät
            logPrintf("[LoRa HAL] CAD: Fehler state=%d, sende trotzdem\n", state);
            return true;
        }
    }

    logPrintf("[LoRa HAL] CAD: Kanal nach 3 Versuchen weiter belegt\n");
    return false;
}

// ──────────────────────────────────────────────────────────────
LoraSendResult loraSend(const uint8_t *data, size_t len) {
    // 1. Duty-Cycle-Sperre prüfen
    unsigned long now = millis();
    if (now < dutyCycleReleaseTime) {
        logPrintf("[LoRa HAL] Duty Cycle Pause aktiv — noch %lums\n",
            dutyCycleReleaseTime - now);
        return LORA_SEND_DUTYCYCLE;
    }

    // 2. Kanal auf Aktivität prüfen
    if (!channelClearToSend()) {
        radio.startReceive();  // CAD hinterlässt Chip im Standby → zurück in permanent RX
        return LORA_SEND_CHANNEL_BUSY;
    }

    // 3. Senden
    unsigned long sendStart = millis();
    int state = radio.transmit(data, len);
    unsigned long airtime = millis() - sendStart;
    lastAirtimeMs = airtime;

    radio.startReceive();  // TX hinterlässt Chip im Standby → zurück in permanent RX

    if (state != RADIOLIB_ERR_NONE) {
        logPrintf("[LoRa HAL] Transmit-Fehler state=%d\n", state);
        return LORA_SEND_ERROR;
    }

    // 4. Pflichtpause berechnen: Toff = Ton * (1/dutycycle - 1)
    uint8_t dcLimit = PRESETS[currentPreset].dutyCycleLimitPercent;
    unsigned long multiplier = (100 / dcLimit) - 1;  // 1% -> 99, 10% -> 9
    dutyCycleReleaseTime = millis() + (airtime * multiplier);

    logPrintf("[LoRa HAL] Gesendet, Airtime=%lums, Pflichtpause=%lums (DC=%d%%)\n",
        airtime, airtime * multiplier, dcLimit);

    return LORA_SEND_OK;
}

unsigned long loraLastAirtimeMs() {
    return lastAirtimeMs;
}

unsigned long loraDutyCycleRemainingMs() {
    unsigned long now = millis();
    if (now >= dutyCycleReleaseTime) return 0;
    return dutyCycleReleaseTime - now;
}

// ──────────────────────────────────────────────────────────────
int loraReceive(uint8_t *buf, size_t maxLen, uint32_t timeoutMs) {
#ifdef HARDWARE_ESP32_SX1276
    // Timeout muss bei hohen Spreading Factors deutlich länger sein —
    // bei SF12 dauert ein Symbol 32ms statt 4ms bei SF9. Ein zu kurzer
    // Timeout bricht den Empfang mitten in der laufenden Übertragung ab
    // und das Paket wird verpasst.
    // Grobe Faustregel: Timeout >= max. Airtime eines 255-Byte-Pakets.
    //   SF7:  ~500ms   SF9: ~1.500ms   SF12: ~6.000ms
    if (timeoutMs == 0) {
        switch (PRESETS[currentPreset].sf) {
            case 12: timeoutMs = 6000; break;
            case 11: timeoutMs = 4000; break;
            case 10: timeoutMs = 2500; break;
            case 9:  timeoutMs = 1500; break;
            default: timeoutMs = 1000; break; // SF7/SF8
        }
    }
    int state = radio.receive(buf, maxLen, timeoutMs);
#endif
#ifdef HARDWARE_HELTEC_V3
    // timeoutMs=0: RadioLib berechnet Timeout automatisch (500% der Airtime
    // bei 255B/SF9 ≈ 7500ms). Für Polling-Loops expliziten Timeout übergeben.
    int state;
    if (timeoutMs == 0) {
        state = radio.receive(buf, maxLen);
    } else {
        state = radio.receive(buf, maxLen, (RadioLibTime_t)timeoutMs);
    }
#endif

    if (state == RADIOLIB_ERR_NONE) {
        int len = (int)radio.getPacketLength();
        lastSNR = (int)round(radio.getSNR());
        logPrintf("[LoRa HAL] Empfangen, len=%d, RSSI=%.0f, SNR=%+d\n",
                  len, radio.getRSSI(), lastSNR);
        return len;
    } else if (state == RADIOLIB_ERR_RX_TIMEOUT) {
        return -1;
    } else {
        logPrintf("[LoRa HAL] Fehler state=%d\n", state);
        return -2;
    }
}

float loraRSSI() {
    return radio.getRSSI();
}

int loraLastSNR() {
    return lastSNR;
}

uint8_t loraGetPreset() {
    return currentPreset;
}

uint8_t loraGetDutyCycleLimit() {
    return PRESETS[currentPreset].dutyCycleLimitPercent;
}

// ── Interrupt-basierter Empfang ──────────────────────────────────

static volatile bool loraPacketFlag = false;

void IRAM_ATTR loraRxDoneISR() {
    loraPacketFlag = true;
}

void loraStartContinuousReceive() {
    loraPacketFlag = false;   // stale Flag aus Normalbetrieb löschen
    radio.setPacketReceivedAction(loraRxDoneISR);
    radio.startReceive();
}

void loraStopContinuousReceive() {
    radio.clearPacketReceivedAction();  // detachInterrupt
    radio.standby();
    loraPacketFlag = false;
}

bool loraPacketAvailable() {
    return loraPacketFlag;
}

int loraReadPacketNonBlocking(uint8_t *buf, size_t maxLen) {
    loraPacketFlag = false;
    int len = (int)radio.getPacketLength();
    // len==0 nach TxDone-Spurious-Fire oder leerem RxDone-Event
    if (len <= 0 || (size_t)len > maxLen) {
        radio.startReceive();
        return -1;
    }
    int state = radio.readData(buf, len);
    lastSNR = (int)round(radio.getSNR());
    logPrintf("[LoRa HAL] NonBlocking Empfangen, len=%d, RSSI=%.0f, SNR=%+d\n",
              len, radio.getRSSI(), lastSNR);
    radio.startReceive();
    return (state == RADIOLIB_ERR_NONE) ? len : -1;
}

void loraEnsureRxMode() {
    radio.startReceive();
}