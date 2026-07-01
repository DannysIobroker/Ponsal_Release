#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// ── Sende-Ergebnis ──────────────────────────────────────────────
enum LoraSendResult {
    LORA_SEND_OK = 0,
    LORA_SEND_DUTYCYCLE,   // Pflichtpause läuft noch
    LORA_SEND_CHANNEL_BUSY, // Kanal belegt nach 3 CAD-Versuchen
    LORA_SEND_ERROR        // Hardware-/Transmit-Fehler
};

// ── LoRa-Presets ────────────────────────────────────────────────
// Index muss zu storage (lora_preset, uint8) passen
enum LoraPreset {
    PRESET_STANDARD    = 0,  // SF9,  125kHz, CR4/5, 868.0 MHz
    PRESET_REICHWEITE  = 1,  // SF12, 125kHz, CR4/5, 868.0 MHz (Test)
    PRESET_ORGANISATION = 2, // SF9,  125kHz, CR4/5, 869.525 MHz (Subband P, 10% DC)
    PRESET_STADT       = 3,  // SF7,  125kHz, CR4/5, 868.0 MHz ("Schnelle Nachrichten")
    PRESET_COUNT       = 4
};

// ── Initialisierung ─────────────────────────────────────────────
// preset: gewünschtes Preset beim Start (aus NVS geladen)
bool  loraInit(uint8_t preset);

// Preset zur Laufzeit wechseln (z.B. nach Config-Änderung)
// Erfordert radio.begin() neu mit anderen Parametern — macht intern
// einen Re-Init. Bestehende Duty-Cycle-Sperre bleibt erhalten.
bool  loraSetPreset(uint8_t preset);

// ── Senden ──────────────────────────────────────────────────────
// Prüft vor dem Senden:
//  1. Duty-Cycle-Sperre (Pflichtpause aus letzter Sendung)
//  2. Channel Activity Detection (3 Versuche, Backoff 20-500ms)
// Aktualisiert nach erfolgreichem Senden die Duty-Cycle-Sperre
// anhand der gemessenen Airtime: Toff = Ton * (1/dutycycle - 1)
LoraSendResult loraSend(const uint8_t *data, size_t len);

// Letzte gemessene Airtime in Millisekunden (vom letzten loraSend-Aufruf)
unsigned long loraLastAirtimeMs();

// Verbleibende Pflichtpause in Millisekunden (0 = keine Sperre)
unsigned long loraDutyCycleRemainingMs();

// ── Empfangen ───────────────────────────────────────────────────
// timeoutMs=0: SF-abhängiger Standard (SF9=1500ms, SX1262 auto)
// timeoutMs>0: explizit kurzer Timeout für Polling-Loops
int   loraReceive(uint8_t *buf, size_t maxLen, uint32_t timeoutMs = 0);

// RSSI des zuletzt empfangenen Pakets
float loraRSSI();

// SNR des zuletzt empfangenen Pakets (ganzzahlig, dB)
int loraLastSNR();

// Aktuell aktives Preset
uint8_t loraGetPreset();

// Duty-Cycle-Limit des aktuell aktiven Presets in Prozent
// (Standard/Reichweite/Stadt = 1, Organisation = 10)
uint8_t loraGetDutyCycleLimit();