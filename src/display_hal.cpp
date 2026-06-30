#include "display_hal.h"
#include <Arduino.h>
#include "serial_log.h"

#if HAS_OLED
  #include <Wire.h>
  #include <Adafruit_GFX.h>
  #include <Adafruit_SSD1306.h>

  static Adafruit_SSD1306 oled(128, 64, &Wire, OLED_RST);
  static bool oledReady = false;
#endif

// ── State machine ─────────────────────────────────────────────
static DisplayState   currentState  = DISP_STATE_BOOT;
static bool           overrideActive = false;
static unsigned long  bootStart     = 0;
static unsigned long  lastChange    = 0;  // letzter Screen-Wechsel im Boot
static bool           bootScreenA   = true;
static unsigned long  lastUpdate    = 0;  // letzter vollständiger Redraw

// ── Akku (alle 10s neu gelesen, gecacht) ──────────────────────
static float         cachedBatV     = 0.0f;
static bool          cachedCharging = false;
static unsigned long lastBatRead    = 0;

// ─────────────────────────────────────────────────────────────

void displayInit() {
    logPrintf("[Display] Init start\n");
#if HAS_OLED
    // Vext (GPIO 36) steuert OLED-Stromversorgung auf Heltec V3.
    // Nach Framework-Update (Arduino ESP32 3.x / ESP-IDF 5.x) wird GPIO 36
    // nicht mehr automatisch LOW gesetzt — OLED bleibt sonst dunkel trotz
    // erfolgreicher I2C-Initialisierung. Muss explizit gesetzt werden.
    pinMode(36, OUTPUT);
    digitalWrite(36, LOW);
    delay(20);

    pinMode(OLED_RST, OUTPUT);
    digitalWrite(OLED_RST, LOW);
    delay(20);
    digitalWrite(OLED_RST, HIGH);
    delay(20);

    Wire.begin(OLED_SDA, OLED_SCL);
    Wire.setClock(400000);

    if (oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
        oledReady = true;
        oled.clearDisplay();
        oled.display();
        logPrintf("[Display] OK\n");
    } else {
        logPrintf("[Display] SSD1306 Init fehlgeschlagen\n");
    }
#endif
}

void displaySetState(DisplayState state) {
    currentState = state;
    lastUpdate   = 0;  // sofortiger Redraw beim nächsten Tick
    if (state == DISP_STATE_BOOT) {
        bootStart   = millis();
        lastChange  = 0;  // 0 = noch kein erster Draw
        bootScreenA = true;
    }
    logPrintf("[Display] State → %d\n", (int)state);
}

void displaySetOverride(bool active) {
    overrideActive = active;
    if (!active) lastUpdate = 0;  // nach Override: sofort neu zeichnen
}

void displayForceRefresh() {
    lastUpdate = 0;
}

// ── Direkter Schreibzugriff (Fehler, Reset, Button-Screens) ──
void displayStatus(const char *line1, const char *line2, const char *line3, const char *line4) {
#if HAS_OLED
    if (!oledReady) return;
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    if (line4) {
        if (line1) { oled.setCursor(0,  0); oled.print(line1); }
        if (line2) { oled.setCursor(0, 16); oled.print(line2); }
        if (line3) { oled.setCursor(0, 32); oled.print(line3); }
        oled.setCursor(0, 48); oled.print(line4);
    } else {
        if (line1) { oled.setCursor(0,  0); oled.print(line1); }
        if (line2) { oled.setCursor(0, 22); oled.print(line2); }
        if (line3) { oled.setCursor(0, 44); oled.print(line3); }
    }
    oled.display();
#endif
}

void displayClear() {
#if HAS_OLED
    if (!oledReady) return;
    oled.clearDisplay();
    oled.display();
#endif
}

// ── Akku-Messung ─────────────────────────────────────────────
#if HAS_OLED && defined(HAS_BATTERY)

static void updateBattery() {
    unsigned long now = millis();
    if (lastBatRead != 0 && now - lastBatRead < 10000) return;
    lastBatRead = now;

    // Spannungsteiler aktivieren (GPIO BAT_ADC_CTRL HIGH)
    pinMode(BAT_ADC_CTRL, OUTPUT);
    digitalWrite(BAT_ADC_CTRL, HIGH);
    delay(2);
    int raw = analogRead(BAT_ADC_PIN);
    digitalWrite(BAT_ADC_CTRL, LOW);

    // 12-bit ADC, 3.3V Ref, Teiler 1:2 → Vbat = ADC_V × 2
    cachedBatV = (raw / 4095.0f) * 3.3f * 2.0f;

    // TP4054 CHRG: open-drain, LOW = lädt
    pinMode(BAT_CHRG_PIN, INPUT_PULLUP);
    cachedCharging = (digitalRead(BAT_CHRG_PIN) == LOW);

    logPrintf("[Bat] %.2fV raw=%d chrg=%d\n", cachedBatV, raw, (int)cachedCharging);
}

static void renderBatteryLine(char *buf, size_t bufLen) {
    if (cachedBatV < 3.0f || cachedBatV > 4.3f) {
        buf[0] = '\0';
        return;
    }
    // 0% bei 3.0V, 100% bei 4.2V
    int pct = (int)(((cachedBatV - 3.0f) / (4.2f - 3.0f)) * 100.0f);
    if (pct > 100) pct = 100;
    if (pct < 0)   pct = 0;

    // 4-Zeichen-Balken, je '#' = 25%
    int bars = (pct + 12) / 25;
    if (bars > 4) bars = 4;
    char bar[5] = "    ";
    for (int i = 0; i < bars; i++) bar[i] = '#';

    if (cachedCharging) {
        snprintf(buf, bufLen, "Akku:[%s] %d%%^", bar, pct);
    } else {
        snprintf(buf, bufLen, "Akku:[%s] %d%%", bar, pct);
    }
}

#else
static void updateBattery() {}
static void renderBatteryLine(char *buf, size_t bufLen) { buf[0] = '\0'; }
#endif

// ── Display-Tick (aus loop() aufrufen) ───────────────────────
void displayTick(const DisplayContext &ctx) {
#if HAS_OLED
    if (!oledReady || overrideActive) return;

    unsigned long now = millis();

    switch (currentState) {

        case DISP_STATE_ERROR_LORA:
        case DISP_STATE_ERROR_NVS:
            // Einmalig via displayStatus() geschrieben — kein Tick nötig
            return;

        case DISP_STATE_BOOT: {
            // Nach 30s in Dauerbetrieb wechseln
            if (now - bootStart >= 30000) {
                currentState = ctx.pskLoaded ? DISP_STATE_NORMAL : DISP_STATE_NO_CHANNEL;
                logPrintf("[Display] Boot → %s\n", ctx.pskLoaded ? "NORMAL" : "NO_CHANNEL");
                lastUpdate = 0;
                displayTick(ctx);  // sofort neu zeichnen
                return;
            }
            // Erster Draw: Screen A zeigen, lastChange setzen
            if (lastChange == 0) {
                bootScreenA = true;
                lastChange  = now;
                displayStatus(ctx.ssid, ctx.wifiPass, "projektx.local");
                lastUpdate = now;
                return;
            }
            // Alle 3s wechseln
            if (now - lastChange >= 3000) {
                bootScreenA = !bootScreenA;
                lastChange  = now;
                if (bootScreenA) {
                    displayStatus(ctx.ssid, ctx.wifiPass, "projektx.local");
                } else {
                    displayStatus("Verbinden:", "192.168.4.1", "");
                }
            }
            return;
        }

        case DISP_STATE_NO_CHANNEL: {
            if (ctx.pskLoaded) {
                logPrintf("[Display] State → NORMAL (Kanal verfügbar)\n");
                currentState = DISP_STATE_NORMAL;
                lastUpdate = 0;
                displayTick(ctx);
                return;
            }
            if (lastUpdate != 0 && now - lastUpdate < 2000) return;
            lastUpdate = now;
            displayStatus("Kein Kanal", "Bitte konfig.", "192.168.4.1");
            return;
        }

        case DISP_STATE_NORMAL: {
            if (!ctx.pskLoaded) {
                logPrintf("[Display] State → NO_CHANNEL (kein Kanal)\n");
                currentState = DISP_STATE_NO_CHANNEL;
                lastUpdate = 0;
                displayTick(ctx);
                return;
            }
            updateBattery();
            if (lastUpdate != 0 && now - lastUpdate < 1000) return;
            lastUpdate = now;

            char unreadLine[22];
            snprintf(unreadLine, sizeof(unreadLine), "Neu: %lu",
                     (unsigned long)ctx.unreadMessages);

            char battLine[22] = "";
            renderBatteryLine(battLine, sizeof(battLine));

            char dcLine[22] = "";
            if (ctx.dutyCycleMs > 0) {
                snprintf(dcLine, sizeof(dcLine), "Pause: %lus",
                         ctx.dutyCycleMs / 1000);
            }

            displayStatus(ctx.ssid, unreadLine, battLine, dcLine);
            return;
        }
    }
#endif
}
