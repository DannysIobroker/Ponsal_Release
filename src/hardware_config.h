#pragma once

// ═══════════════════════════════════════════════════════════════
// hardware_config.h
// Einzige hardware-spezifische Datei im Projekt.
// Wird über build_flags in platformio.ini gesetzt —
// hier nichts manuell ändern.
// ═══════════════════════════════════════════════════════════════

// ── ESP32-WROOM-32U + RFM95W (SX1276) ─────────────────────────
#ifdef HARDWARE_ESP32_SX1276
  // LoRa Pins
  #define LORA_NSS    32
  #define LORA_DIO0   33
  #define LORA_RST    14
  #define LORA_DIO1   -1
  #define LORA_SCK    25
  #define LORA_MOSI   26
  #define LORA_MISO   27
  // Kein OLED
  #define HAS_OLED    false
  // Pairing-Button
  #define BUTTON_PIN  4
#endif

// ── Heltec WiFi LoRa 32 V3 (ESP32-S3 + SX1262) ────────────────
#ifdef HARDWARE_HELTEC_V3
  // LoRa Pins
  #define LORA_NSS    8
  #define LORA_DIO1   14
  #define LORA_RST    12
  #define LORA_BUSY   13
  #define LORA_SCK    9
  #define LORA_MOSI   10
  #define LORA_MISO   11
  // OLED Pins
  #define HAS_OLED    1
  #define OLED_SDA    17
  #define OLED_SCL    18
  #define OLED_RST    21
  #define OLED_ADDR   0x3C
  // Pairing-Button (PRG-Taste auf dem Board)
  #define BUTTON_PIN  0
  // Akku-Messung (aus Heltec V3 Schaltplan)
  #define BAT_ADC_PIN   1   // GPIO 1, ADC1_CH0, Vbat/2 über Spannungsteiler
  #define BAT_ADC_CTRL  37  // GPIO 37, HIGH = Spannungsteiler einschalten
  #define BAT_CHRG_PIN  6   // TP4054 CHRG, active LOW = lädt
  #define BAT_DIVIDER_RATIO 2.0f  // 1:1-Teiler
  #define HAS_BATTERY   1
#endif

// ── Heltec WiFi LoRa 32 V4 (ESP32-S3 + SX1262) ────────────────
// Pin-Quelle: Espressif arduino-esp32 Upstream (variants/heltec_wifi_lora_32_V4/
// pins_arduino.h) — LoRa/OLED/Vext-Pins identisch zu V3. Vext (GPIO 36,
// hardcoded in display_hal.cpp, nicht hier) LOW=aktiv und ADC_CTRL HIGH=aktiv
// empirisch am Gerät bestätigt (2026-07-11, Standalone-Testsketch
// tools/v4_pin_test/). BUTTON_PIN=0 physisch bestätigt (2026-07-11).
// Akku: BAT_ADC_PIN=1/BAT_ADC_CTRL=37 identisch zu V3 — Heltec-PNG "V4-R2"
// zeigte abweichend GPIO2/GPIO46, aber GPIO46 auf diesem Modul nicht
// nutzbar (Nutzerangabe) — verworfen. Teiler-Ratio ×4.9 (100k/390k) gegen
// echten Akku + Multimeter verifiziert (2026-07-11), abweichend von V3s ×2.
// BAT_CHRG_PIN weiterhin unbekannt — keine Quelle, im Schaltplan nur bis
// zur Status-LED verfolgt, keine erkennbare GPIO-Verbindung. Ladeanzeige
// auf V4 deshalb nicht verfügbar (siehe display_hal.cpp, #ifdef BAT_CHRG_PIN).
#ifdef HARDWARE_HELTEC_V4
  // LoRa Pins
  #define LORA_NSS    8
  #define LORA_DIO1   14
  #define LORA_RST    12
  #define LORA_BUSY   13
  #define LORA_SCK    9
  #define LORA_MOSI   10
  #define LORA_MISO   11
  // OLED Pins
  #define HAS_OLED    1
  #define OLED_SDA    17
  #define OLED_SCL    18
  #define OLED_RST    21
  #define OLED_ADDR   0x3C
  // Pairing-Button (PRG-Taste), physisch bestätigt
  #define BUTTON_PIN  0
  // Akku-Messung — Pins identisch zu V3, Teiler-Ratio abweichend
  #define BAT_ADC_PIN   1
  #define BAT_ADC_CTRL  37
  #define BAT_DIVIDER_RATIO 4.9f  // (100k+390k)/100k, gegen echten Akku verifiziert
  #define HAS_BATTERY   1
  // Kein BAT_CHRG_PIN — keine Quelle, Ladeanzeige auf V4 nicht verfügbar
#endif

// ── Sanity Check ───────────────────────────────────────────────
#if !defined(HARDWARE_ESP32_SX1276) && !defined(HARDWARE_HELTEC_V3) && !defined(HARDWARE_HELTEC_V4)
  #error "Kein Hardware-Target definiert. platformio.ini prüfen."
#endif