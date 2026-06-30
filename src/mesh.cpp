#include "mesh.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "lora_hal.h"
#include "crypto.h"
#include "webui.h"
#include "serial_log.h"

extern ChannelPsk channelPsks[];
extern uint8_t channelPskCount;
extern uint8_t nodeId[2];
extern bool hasTimeReference;
extern uint32_t timeRefUnixSec;
extern unsigned long timeRefMillis;
extern bool autoReplyEnabled;
extern char wifiSSID[32];
extern void addMessage(const char *sender, const char *text, bool isOwn,
                       float rssi, int snr, unsigned long airtimeMs, uint32_t timestamp,
                       const char *channel);

// ── Verzögertes Autoreply ────────────────────────────────────
struct PendingAutoReply {
    char         text[183];
    char         channel[CHANNEL_NAME_MAX + 1];
    unsigned long sendAfter;
    bool         valid;
};
static PendingAutoReply pendingAutoReply = {};

struct DedupEntry {
    uint32_t msgId;
    unsigned long seenAt;
};
#define DEDUP_SIZE       3600
#define DEDUP_MAX_AGE_MS (60UL * 60UL * 1000UL)

static DedupEntry dedupBuffer[DEDUP_SIZE];
static uint16_t dedupHead = 0;

static long isDuplicateAge(uint32_t msgId) {
    if (msgId == 0) return -1;
    unsigned long now = millis();
    for (int i = 0; i < DEDUP_SIZE; i++) {
        if (dedupBuffer[i].msgId == msgId) {
            unsigned long age = now - dedupBuffer[i].seenAt;
            if (age < DEDUP_MAX_AGE_MS) return (long)(age / 1000);
        }
    }
    return -1;
}

void meshInit() {
    memset(dedupBuffer, 0, sizeof(dedupBuffer));
    dedupHead = 0;
}

bool meshIsDuplicate(uint32_t msgId) {
    return isDuplicateAge(msgId) >= 0;
}

void meshAddToDedup(uint32_t msgId) {
    dedupBuffer[dedupHead].msgId  = msgId;
    dedupBuffer[dedupHead].seenAt = millis();
    dedupHead = (uint16_t)((dedupHead + 1) % DEDUP_SIZE);
}

void meshHandleIncoming(uint8_t *buf, int len) {
    if (len < PACKET_MIN || len > PACKET_MAX) {
        logPrintf("[Mesh] Ungültige Paketgröße %d\n", len);
        return;
    }
    if (buf[0] != PROTOCOL_VERSION) {
        logPrintf("[Mesh] Unbekannte Version, verworfen\n");
        return;
    }

    uint32_t msgId = ((uint32_t)buf[3] << 24) | ((uint32_t)buf[4] << 16) |
                     ((uint32_t)buf[5] <<  8) |  (uint32_t)buf[6];
    bool isOwnMessage = (buf[3] == nodeId[0] && buf[4] == nodeId[1]);

    logPrintf("[Mesh] Paket empfangen, NachrichtID: %08lX, RSSI: %.0fdBm%s\n",
        msgId, loraRSSI(), isOwnMessage ? " (eigenes Echo)" : "");

    {
        long age = isDuplicateAge(msgId);
        if (age >= 0) {
            logPrintf("[Mesh] Duplikat (Alter: %lds) — verworfen\n", age);
            return;
        }
    }

    int payloadLen = len - 8 - 12 - 16;
    if (payloadLen < 2) {
        logPrintf("[Mesh] Payload zu kurz\n");
        return;
    }

    int matchedIdx = -1;
    uint8_t plaintext[payloadLen];
    for (int i = 0; i < channelPskCount; i++) {
        if (buf[1] == channelPsks[i].networkId[0] && buf[2] == channelPsks[i].networkId[1]) {
            if (decryptPayload(channelPsks[i].psk, buf+8, buf, 8, buf+20, payloadLen, buf+20+payloadLen, plaintext)) {
                matchedIdx = i;
                break;
            }
        }
    }

    if (matchedIdx < 0) {
        logPrintf("[Mesh] Kein passender Kanal — verworfen\n");
        return;
    }

    meshAddToDedup(msgId);

    logPrintf("[Mesh] Entschlüsselt auf Kanal '%s'\n", channelPsks[matchedIdx].name);

    if (payloadLen < 25) return;

    char senderName[21] = {0};
    char msgText[183]   = {0};
    uint32_t msgTimestamp = 0;
    memcpy(senderName, plaintext, 20);
    msgTimestamp = (uint32_t)plaintext[21] | ((uint32_t)plaintext[22] << 8) |
                  ((uint32_t)plaintext[23] << 16) | ((uint32_t)plaintext[24] << 24);
    int textLen = min((int)(payloadLen - 25), 182);
    if (textLen > 0) memcpy(msgText, plaintext + 25, textLen);

    if (msgTimestamp != 0 && hasTimeReference) {
        uint32_t nowEst = timeRefUnixSec + (millis() - timeRefMillis) / 1000;
        uint32_t delta = (msgTimestamp > nowEst) ? (msgTimestamp - nowEst) : (nowEst - msgTimestamp);
        if (delta > 3600) {
            logPrintf("[Mesh] Timestamp unplausibel (delta=%lus), ersetze mit Schätzzeit\n", delta);
            msgTimestamp = nowEst;
        }
    }

    int pktSNR = loraLastSNR();
    logPrintf("[Mesh] Von %s auf '%s' (ts=%lu, RSSI=%.0f, SNR=%+d): %s\n",
        senderName, channelPsks[matchedIdx].name, msgTimestamp,
        loraRSSI(), pktSNR, msgText);
    unreadMessages++;
    addMessage(senderName, msgText, false, loraRSSI(), pktSNR, 0, msgTimestamp,
               channelPsks[matchedIdx].name);

    if (isOwnMessage) {
        logPrintf("[Mesh] Eigene Nachricht — keine Weiterleitung\n");
    } else {
        enqueueSend(buf, len, QUEUE_RELAY);
    }

    if (autoReplyEnabled && strcasecmp(msgText, "antworte") == 0) {
        // Format: XXXX,<RSSI>,<+/-SNR>  (letzte 4 Zeichen der SSID)
        int ssidLen = strlen(wifiSSID);
        const char *suffix = ssidLen >= 4 ? wifiSSID + ssidLen - 4 : wifiSSID;
        int rssiInt = (int)loraRSSI();
        char replyText[183];
        snprintf(replyText, sizeof(replyText), "%.4s,%d,%+d", suffix, rssiInt, pktSNR);
        unsigned long delayMs = 500 + (esp_random() % 2001);
        pendingAutoReply.valid = true;
        pendingAutoReply.sendAfter = millis() + delayMs;
        strncpy(pendingAutoReply.text, replyText, 182);
        pendingAutoReply.text[182] = '\0';
        strncpy(pendingAutoReply.channel, channelPsks[matchedIdx].name, CHANNEL_NAME_MAX);
        pendingAutoReply.channel[CHANNEL_NAME_MAX] = '\0';
        logPrintf("[AutoReply] Geplant in %lums: %s\n", delayMs, replyText);
    }
}

void meshAutoReplyTick() {
    if (!pendingAutoReply.valid) return;
    if (millis() < pendingAutoReply.sendAfter) return;
    pendingAutoReply.valid = false;
    uint32_t ts = hasTimeReference
        ? timeRefUnixSec + (millis() - timeRefMillis) / 1000
        : 0;
    if (enqueueOwnMessage(pendingAutoReply.text, pendingAutoReply.channel, ts)) {
        logPrintf("[AutoReply] Eingereiht: %s\n", pendingAutoReply.text);
    } else {
        logPrintf("[AutoReply] Einreihen fehlgeschlagen\n");
    }
}
