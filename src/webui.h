#pragma once
#include <ESPAsyncWebServer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "storage.h"

#define MAX_MESSAGES 20

struct Message {
    char  sender[21];
    char  text[183];
    char  channel[CHANNEL_NAME_MAX + 1];
    bool  valid;
    bool  isOwn;
    float rssi;
    int   snr;
    unsigned long airtimeMs;
    uint32_t timestamp;
};

// Ergebnis des letzten Sendeversuchs — wird von loop() gesetzt,
// von /messages an den Client gemeldet (asynchron, da loop() und
// webTask getrennt laufen).
enum SendStatus {
    SEND_STATUS_NONE = 0,    // kein Ergebnis seit letztem Poll
    SEND_STATUS_OK,
    SEND_STATUS_DUTYCYCLE,
    SEND_STATUS_CHANNEL_BUSY,
    SEND_STATUS_ERROR,
    SEND_STATUS_NO_CHANNEL   // kein PSK konfiguriert
};

struct OutgoingMsg {
    char text[183];
    char channel[CHANNEL_NAME_MAX + 1];
    bool pending;
    volatile SendStatus lastResult;
    uint32_t timestamp;
};

// Grund der aktuellen Duty-Cycle-Pause — von main.cpp gesetzt, von /messages gelesen
enum DutyCycleReason {
    DC_REASON_NONE  = 0,
    DC_REASON_OWN   = 1,  // eigene Nachricht hat DC-Pause ausgelöst
    DC_REASON_RELAY = 2   // Weiterleitung hat DC-Pause ausgelöst
};

// Unread-Counter: inkrementiert in mesh.cpp bei eingehender Nachricht,
// resettet in /messages-Handler (Browser hat Nachrichten gesehen)
extern volatile uint32_t unreadMessages;

void setupWebUI(
    AsyncWebServer &server,
    Message *messages,
    uint8_t *msgCount,
    uint8_t *msgHead,
    uint32_t *msgTotalCount,
    SemaphoreHandle_t msgMutex,
    OutgoingMsg *outgoing,
    SemaphoreHandle_t outMutex,
    const char *deviceName,
    volatile DutyCycleReason *dcReason,
    const char *activeChannel
);