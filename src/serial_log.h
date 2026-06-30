#pragma once

// #ifdef DEVELOPMENT schützt alle Serial-Ausgaben und den /debug/setseq-Endpunkt.
// Ohne dieses Flag: kein Serial.begin(), alle log*-Aufrufe sind leere Makros
// (keine Laufzeitkosten). Production-Build entsteht durch Entfernen von
// -DDEVELOPMENT aus platformio.ini vor Auslieferung. NICHT VERGESSEN.
// Fehlerkanal in Production: OLED-Fehlerzustände (LoRa-Init, NVS).

#ifdef DEVELOPMENT
void logInit();
void logPrintf(const char *fmt, ...);
void logDrain();
#else
#define logInit()      do {} while(0)
#define logPrintf(...) do {} while(0)
#define logDrain()     do {} while(0)
#endif
