#pragma once
#include <stdint.h>
#include "hardware_config.h"

// ═══════════════════════════════════════════════════════════════
// display_hal.h
// Display-Abstraktion. Kein Code in ProjektX kennt den
// Chip oder die Library. Kein OLED = alle Funktionen sind No-Ops.
// ═══════════════════════════════════════════════════════════════

// ── Zustandsmaschine ─────────────────────────────────────────
enum DisplayState {
    DISP_STATE_BOOT = 0,   // Boot-Sequenz (60s, 3 Screens à 5s, 4× Wiederholung)
    DISP_STATE_NO_CHANNEL, // Kein Kanal konfiguriert
    DISP_STATE_NORMAL,     // Normalbetrieb
    DISP_STATE_ERROR_LORA, // LoRa-Init fehlgeschlagen (dauerhaft)
    DISP_STATE_ERROR_NVS,  // NVS nicht lesbar (dauerhaft)
};

// Kontext den Aufrufer bei jedem Tick übergeben muss
struct DisplayContext {
    const char   *ssid;
    const char   *wifiPass;
    const char   *activeChannel;
    bool          pskLoaded;
    uint32_t      unreadMessages;
    unsigned long dutyCycleMs;
    // Boot-Sequenz: PIN-Wert und Anzeigetoggles (aus NVS geladen, RAM-gecacht)
    uint32_t      settingsPin;
    bool          showWifiPw;      // false → Platzhaltertext auf Screen 1
    bool          showSettingsPin; // false → Platzhaltertext auf Screen 2
};

// ── API ───────────────────────────────────────────────────────
void displayInit();
void displaySetState(DisplayState state);
void displaySetOverride(bool active);   // verhindert Tick-Updates während Button-Screens
void displayForceRefresh();             // nächster Tick zeichnet sofort neu
void displayTick(const DisplayContext &ctx);

// Direkter Schreibzugriff — nur für Fehler/Reset/Button-Screens
void displayStatus(const char *line1, const char *line2, const char *line3, const char *line4 = nullptr);
void displayClear();
