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
  #define HAS_BATTERY   1
#endif

// ── Sanity Check ───────────────────────────────────────────────
#if !defined(HARDWARE_ESP32_SX1276) && !defined(HARDWARE_HELTEC_V3)
  #error "Kein Hardware-Target definiert. platformio.ini prüfen."
#endif