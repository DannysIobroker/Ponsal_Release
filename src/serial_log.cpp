#include "serial_log.h"

#ifdef DEVELOPMENT
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <stdarg.h>

#define LOG_QUEUE_SIZE 32
#define LOG_MSG_MAX 192

static QueueHandle_t logQueue = nullptr;

void logInit() {
    logQueue = xQueueCreate(LOG_QUEUE_SIZE, LOG_MSG_MAX);
}

void logPrintf(const char *fmt, ...) {
    char buf[LOG_MSG_MAX];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, LOG_MSG_MAX, fmt, args);
    va_end(args);

    if (!logQueue) {
        Serial.print(buf);
        return;
    }
    xQueueSend(logQueue, buf, 0);
}

void logDrain() {
    if (!logQueue) return;
    char buf[LOG_MSG_MAX];
    while (xQueueReceive(logQueue, buf, 0) == pdTRUE) {
        Serial.print(buf);
    }
}
#endif
