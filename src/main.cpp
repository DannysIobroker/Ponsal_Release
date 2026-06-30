// ProjektX — Ponsal Referenzimplementierung
// Hardware-unabhängig — alle Hardware-Details in hardware_config.h

#define FW_VERSION  "Phase5-dev"
#define FW_CHANGES  "WebUI Navigation, Mehrkanal-Empfang, PSK-Cache, Mesh Send Queue"

#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include <nvs_flash.h>
#include "hardware_config.h"
#include "lora_hal.h"
#include "display_hal.h"
#include "webui.h"
#include "crypto.h"
#include "storage.h"
#include "serial_log.h"
#include "mesh.h"
#include "config_routes.h"
#include "pairing.h"

#define WIFI_PASS_DEFAULT  "changeme"

AsyncWebServer server(80);

// ── PSK-Cache — alle Kanäle im RAM ──────────────────────────────
ChannelPsk channelPsks[MAX_CHANNELS];
uint8_t    channelPskCount = 0;

// Laufzeit-Zustand
bool     pskLoaded     = false;
char     activeChannel[CHANNEL_NAME_MAX + 1] = {0};
uint8_t  nodeId[2];
volatile uint16_t seqNum = 0;
char     wifiSSID[32];
char     wifiPass[64] = "changeme";
char     deviceName[21] = "Unbekannt";
uint8_t  loraPreset    = 0;
uint32_t settingsPin   = 0;
bool     autoReplyEnabled = false;

// Zeitreferenz
uint32_t      timeRefUnixSec  = 0;
unsigned long timeRefMillis   = 0;
bool          hasTimeReference = false;

volatile DutyCycleReason dutyCycleReason = DC_REASON_NONE;

Message  messages[MAX_MESSAGES];
uint8_t  msgHead  = 0;
uint8_t  msgCount = 0;
uint32_t msgTotalCount = 0;
SemaphoreHandle_t msgMutex;

OutgoingMsg outgoing = {};
SemaphoreHandle_t outMutex;

// ── Send Queue ──────────────────────────────────────────────────
struct SendQueueEntry {
    uint8_t        data[255];
    size_t         len;
    QueueEntryType type;
    unsigned long  enqueuedAt;
    char           ownSender[21];
    char           ownText[183];
    char           ownChannel[CHANNEL_NAME_MAX + 1];
    uint32_t       ownTimestamp;
};

SendQueueEntry sendQueue[SEND_QUEUE_SIZE];
int sendQueueCount = 0;

// Vorwärtsdeklarationen
void addMessage(const char *sender, const char *text, bool isOwn, float rssi, int snr, unsigned long airtimeMs, uint32_t timestamp, const char *channel);
int buildPacket(const char *sender, const char *text, uint32_t timestamp, const uint8_t *usePsk, const uint8_t *useNetId, uint8_t *outBuf);
void webTask(void *pvParameters);
uint32_t pinFromMac();
void wifiPassFromMac(char *pass, size_t maxLen);
void factoryReset();
void reloadChannelPsks();

static void removeQueueFront() {
    if (sendQueueCount > 1) {
        memmove(&sendQueue[0], &sendQueue[1], (sendQueueCount - 1) * sizeof(SendQueueEntry));
    }
    sendQueueCount--;
}

// ----------------------------------------------------------------
// Send Queue — Einreihung
// ----------------------------------------------------------------
bool enqueueSend(const uint8_t *data, size_t len, QueueEntryType type) {
    if (type == QUEUE_OWN || type == QUEUE_PAIRING) {
        if (sendQueueCount >= SEND_QUEUE_SIZE) {
            logPrintf("[MeshQ] Queue voll — OWN kann nicht eingereiht werden\n");
            return false;
        }
        if (sendQueueCount > 0) {
            memmove(&sendQueue[1], &sendQueue[0], sendQueueCount * sizeof(SendQueueEntry));
        }
        memset(&sendQueue[0], 0, sizeof(SendQueueEntry));
        memcpy(sendQueue[0].data, data, len);
        sendQueue[0].len = len;
        sendQueue[0].type = type;
        sendQueue[0].enqueuedAt = millis();
        sendQueueCount++;
        logPrintf("[MeshQ] %s eingereiht (Queue: %d/%d)\n",
            type == QUEUE_PAIRING ? "PAIRING" : "OWN", sendQueueCount, SEND_QUEUE_SIZE);
        return true;
    }

    if (sendQueueCount < SEND_QUEUE_SIZE) {
        int idx = sendQueueCount;
        memset(&sendQueue[idx], 0, sizeof(SendQueueEntry));
        memcpy(sendQueue[idx].data, data, len);
        sendQueue[idx].len = len;
        sendQueue[idx].type = QUEUE_RELAY;
        sendQueue[idx].enqueuedAt = millis();
        sendQueueCount++;
        logPrintf("[MeshQ] RELAY eingereiht (DC-Pause: %lus, Queue: %d/%d)\n",
            loraDutyCycleRemainingMs() / 1000, sendQueueCount, SEND_QUEUE_SIZE);
        return true;
    }

    // Queue voll — älteste RELAY verdrängen
    int displaceIdx = -1;
    for (int i = 0; i < sendQueueCount; i++) {
        if (sendQueue[i].type == QUEUE_RELAY) {
            displaceIdx = i;
            break;
        }
    }
    if (displaceIdx < 0) {
        logPrintf("[MeshQ] Queue voll — kein RELAY verdrängt\n");
        return false;
    }
    if (displaceIdx < sendQueueCount - 1) {
        memmove(&sendQueue[displaceIdx], &sendQueue[displaceIdx + 1],
                (sendQueueCount - 1 - displaceIdx) * sizeof(SendQueueEntry));
    }
    sendQueueCount--;
    int idx = sendQueueCount;
    memset(&sendQueue[idx], 0, sizeof(SendQueueEntry));
    memcpy(sendQueue[idx].data, data, len);
    sendQueue[idx].len = len;
    sendQueue[idx].type = QUEUE_RELAY;
    sendQueue[idx].enqueuedAt = millis();
    sendQueueCount++;
    logPrintf("[MeshQ] Queue voll — älteste RELAY verworfen (Queue: %d/%d)\n",
        sendQueueCount, SEND_QUEUE_SIZE);
    return true;
}

// ----------------------------------------------------------------
// PIN aus MAC berechnen
// ----------------------------------------------------------------
uint32_t pinFromMac() {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    uint32_t raw = ((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) | mac[5];
    return raw % 1000000;
}

// ----------------------------------------------------------------
// WLAN-Passwort aus MAC berechnen
// ----------------------------------------------------------------
void wifiPassFromMac(char *pass, size_t maxLen) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(pass, maxLen, "ponsal%02x%02x%02x", mac[3], mac[4], mac[5]);
}

// ----------------------------------------------------------------
// Werksreset — alles löschen, PIN + WLAN auf MAC-Werte
// ----------------------------------------------------------------
void factoryReset() {
    logPrintf("[Reset] Werksreset gestartet\n");

    storageClearAllChannels();
    storageDeleteDeviceName();
    storageClearMessages();
    storageClearSeqSlots();

    uint32_t newPin = pinFromMac();
    storageStoreSettingsPin(newPin);

    char newPass[21];
    wifiPassFromMac(newPass, sizeof(newPass));
    storageStoreWifiPass(newPass);

    logPrintf("[Reset] Neue PIN: %06lu\n", newPin);
    logPrintf("[Reset] Neues WLAN-Passwort: %s\n", newPass);

    char pinLine[32];
    snprintf(pinLine, sizeof(pinLine), "PIN: %06lu", newPin);
    char passLine[32];
    snprintf(passLine, sizeof(passLine), "PW: %s", newPass);
    displayStatus("Reset OK", pinLine, passLine);

    delay(2000);
    ESP.restart();
}

// ----------------------------------------------------------------
// PSK-Cache laden/neuladen
// ----------------------------------------------------------------
void reloadChannelPsks() {
    char channelNames[MAX_CHANNELS][CHANNEL_NAME_MAX + 1];
    uint8_t count = 0;
    storageLoadChannelNames(channelNames, &count);

    ChannelPsk tempCache[MAX_CHANNELS];
    uint8_t tempCount = 0;

    for (int i = 0; i < count && tempCount < MAX_CHANNELS; i++) {
        uint8_t tempPsk[32];
        if (storageLoadPsk(channelNames[i], tempPsk)) {
            memset(&tempCache[tempCount], 0, sizeof(ChannelPsk));
            strncpy(tempCache[tempCount].name, channelNames[i], CHANNEL_NAME_MAX);
            tempCache[tempCount].name[CHANNEL_NAME_MAX] = '\0';
            memcpy(tempCache[tempCount].psk, tempPsk, 32);
            computeNetworkId(tempPsk, tempCache[tempCount].networkId);
            storageLoadChannelSenderName(channelNames[i], tempCache[tempCount].senderName, sizeof(tempCache[tempCount].senderName));
            storageLoadChannelNameLock(channelNames[i], &tempCache[tempCount].nameLocked);
            tempCount++;
        }
    }

    memcpy(channelPsks, tempCache, sizeof(tempCache));
    channelPskCount = tempCount;

    if (tempCount > 0) {
        strncpy(activeChannel, channelPsks[0].name, CHANNEL_NAME_MAX);
        activeChannel[CHANNEL_NAME_MAX] = '\0';
        pskLoaded = true;
        logPrintf("[Config] %d Kanäle geladen, aktiv: '%s'\n", tempCount, activeChannel);
        for (int i = 0; i < tempCount; i++) {
            logPrintf("[Config]   Kanal %d: '%s' NetworkID=%02X%02X\n",
                i, channelPsks[i].name, channelPsks[i].networkId[0], channelPsks[i].networkId[1]);
        }
    } else {
        activeChannel[0] = '\0';
        pskLoaded = false;
    }
}

// ----------------------------------------------------------------
// Button-Handler — 3s Pairing, 10s Reset mit Bestätigung
// ----------------------------------------------------------------
#ifdef BUTTON_PIN
static void restoreDisplay() {
    displaySetOverride(false);
    // Tick zeichnet beim nächsten loop()-Durchlauf sofort neu
}

void handleButtonTick() {
    static unsigned long btnStart = 0;
    static bool pairingShown = false;
    static bool resetShown = false;
    static bool confirmWait = false;
    static unsigned long confirmStart = 0;

    if (confirmWait) {
        if (millis() - confirmStart < 200) return;
        if (digitalRead(BUTTON_PIN) == LOW) {
            logPrintf("[Button] Reset bestätigt — Werksreset\n");
            factoryReset();
        }
        if (millis() - confirmStart >= 3000) {
            confirmWait = false;
            logPrintf("[Button] Reset nicht bestätigt — normaler Betrieb\n");
            restoreDisplay();
        }
        return;
    }

    if (digitalRead(BUTTON_PIN) == LOW) {
        if (btnStart == 0) btnStart = millis();
        unsigned long held = millis() - btnStart;
        if (!resetShown && held >= 10000) {
            resetShown = true;
            displaySetOverride(true);
            displayStatus("Werksreset?", "Nochmal drücken", "(3 Sekunden)");
            logPrintf("[Button] 10s — Reset-Bestätigung angezeigt\n");
        } else if (!pairingShown && held >= 3000) {
            pairingShown = true;
            displaySetOverride(true);
            displayStatus("Pairing", "Loslassen zum", "Starten");
            logPrintf("[Button] 3s — Pairing-Hinweis angezeigt\n");
        }
    } else {
        if (btnStart != 0) {
            if (resetShown) {
                confirmWait = true;
                confirmStart = millis();
                logPrintf("[Button] Losgelassen nach 10s — warte auf Bestätigung\n");
            } else if (pairingShown) {
                pairingRequestStartReceive();
                logPrintf("[Button] Long press (3-9s) — Pairing-Empfang gestartet\n");
                restoreDisplay();
            }
        }
        btnStart = 0;
        pairingShown = false;
        resetShown = false;
    }
}
#endif

// ----------------------------------------------------------------
// Setup
// ----------------------------------------------------------------
void setup() {
// DEVELOPMENT-Flag schützt Serial-Ausgaben und Debug-Endpunkte (/debug/setseq,
// /debug/fillmessages, /debug/check). Produktions-Builds IMMER ohne dieses Flag
// erstellen (heltec_v3_prod / esp32_sx1276_prod Environment) — ohne Guard wäre
// /debug/setseq auf jedem Gerät aufrufbar und könnte die Sequenznummer-Logik
// zerstören.
#ifdef DEVELOPMENT
    Serial.begin(115200);
#endif
    logInit();
    logPrintf("\n[ProjektX] " FW_VERSION "\n");
    logPrintf("[ProjektX] " FW_CHANGES "\n");
    logPrintf("[ProjektX] Git: " GIT_HASH "\n");

    displayInit();
    displaySetState(DISP_STATE_BOOT);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        logPrintf("[Storage] NVS beschädigt — wird neu initialisiert\n");
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        logPrintf("[Storage] NVS Init fehlgeschlagen: %d\n", ret);
        displaySetState(DISP_STATE_ERROR_NVS);
        displayStatus("NVS FEHLER", "Neustart nötig", nullptr);
        while (true) { delay(100); }
    }
    storageInitMsgPartition();

    if (!SPIFFS.begin(true)) {
        logPrintf("[SPIFFS] Mount fehlgeschlagen\n");
    } else {
        logPrintf("[SPIFFS] OK\n");
    }

    if (!storageLoadSettingsPin(&settingsPin)) {
        settingsPin = pinFromMac();
        logPrintf("[Config] PIN (ab Werk, aus MAC): %06lu\n", settingsPin);
    } else {
        logPrintf("[Config] PIN aus NVS geladen\n");
    }
    logPrintf("[Config] Einstellungs-PIN: %06lu\n", settingsPin);

    if (!storageLoadWifiPass(wifiPass, sizeof(wifiPass))) {
        logPrintf("[Config] WLAN-Passwort: Standard (changeme)\n");
    } else {
        logPrintf("[Config] WLAN-Passwort aus NVS geladen\n");
    }

    if (!storageLoadDeviceName(deviceName, sizeof(deviceName))) {
        logPrintf("[Config] Kein Gerätename — 'Unbekannt'\n");
    }

    if (!storageLoadPreset(&loraPreset)) {
        loraPreset = PRESET_STANDARD;
        logPrintf("[Config] Preset: Standard (0)\n");
    }
    if (loraPreset >= PRESET_COUNT) {
        logPrintf("[Config] Ungültiges Preset %d — auf Standard zurückgesetzt\n", loraPreset);
        loraPreset = PRESET_STANDARD;
    }

    reloadChannelPsks();

    if (!pskLoaded) {
        logPrintf("====================================================\n");
        logPrintf("KEIN KANAL KONFIGURIERT.\n");
        logPrintf("LoRa-Kommunikation deaktiviert.\n");
        logPrintf("Konfigoberfläche: http://projektx.local/config.html\n");
        logPrintf("Einstellungs-PIN: %06lu\n", settingsPin);
        logPrintf("====================================================\n");
    }

    computeNodeId(nodeId);
    logPrintf("[Config] NodeID:    %02X%02X\n", nodeId[0], nodeId[1]);
    snprintf(wifiSSID, sizeof(wifiSSID), "ProjektX-%02X%02X", nodeId[0], nodeId[1]);

    msgMutex = xSemaphoreCreateMutex();
    outMutex = xSemaphoreCreateMutex();
    memset(messages, 0, sizeof(messages));
    memset(sendQueue, 0, sizeof(sendQueue));

    // NVS-Chatverlauf wiederherstellen (Heap statt Stack — 50×230B = ~11.5KB)
    {
        PersistedMessage *restored = (PersistedMessage *)malloc(MSG_SLOTS * sizeof(PersistedMessage));
        if (restored) {
            int restoredCount = storageLoadAllMessages(restored, MSG_SLOTS);
            if (restoredCount > 0) {
                int startIdx = 0;
                if (restoredCount > MAX_MESSAGES) {
                    startIdx = restoredCount - MAX_MESSAGES;
                }
                int count = restoredCount - startIdx;
                for (int i = 0; i < count; i++) {
                    PersistedMessage &src = restored[startIdx + i];
                    messages[i].valid = true;
                    strncpy(messages[i].sender, src.sender, 20);
                    messages[i].sender[20] = '\0';
                    strncpy(messages[i].text, src.text, 182);
                    messages[i].text[182] = '\0';
                    strncpy(messages[i].channel, src.channel, CHANNEL_NAME_MAX);
                    messages[i].channel[CHANNEL_NAME_MAX] = '\0';
                    messages[i].isOwn = src.isOwn != 0;
                    messages[i].rssi = 0.0f;
                    messages[i].airtimeMs = 0;
                    messages[i].timestamp = src.timestamp;
                }
                msgCount = count;
                msgHead = count % MAX_MESSAGES;
                msgTotalCount = count;
                logPrintf("[Storage] %d Nachrichten aus NVS wiederhergestellt\n", restoredCount);
            }
            free(restored);
        }
        storageLogNvsStats();
    }

    {
        uint16_t restoredSeq = 0;
        pairingInit();

#ifdef BUTTON_PIN
    pinMode(BUTTON_PIN, INPUT_PULLUP);
#endif

    if (storageLoadSeqNum(&restoredSeq)) {
            seqNum = restoredSeq;
            logPrintf("[Config] Sequenznummer aus NVS: Start bei %u\n", seqNum);
        } else {
            logPrintf("[Config] Keine Sequenznummer in NVS — starte bei 0\n");
        }
    }

    meshInit();

    xTaskCreatePinnedToCore(webTask, "Web", 8192, NULL, 1, NULL, 0);

    logPrintf("[LoRa] Initialisierung...");
    if (!loraInit(loraPreset)) {
        logPrintf(" FEHLER\n");
        displaySetState(DISP_STATE_ERROR_LORA);
        displayStatus("LoRa FEHLER", "Neustart nötig", nullptr);
        while (true) { delay(100); }
    }
    logPrintf(" OK\n");
    // Kein displayStatus hier — Tick übernimmt nach Boot-Sequenz
}

// ----------------------------------------------------------------
// Eigene Nachricht direkt in Send-Queue einreihen (ohne outgoing-Slot)
// ----------------------------------------------------------------
bool enqueueOwnMessage(const char *text, const char *channel, uint32_t timestamp) {
    int chIdx = -1;
    for (int i = 0; i < channelPskCount; i++) {
        if (strcmp(channelPsks[i].name, channel) == 0) { chIdx = i; break; }
    }
    if (chIdx < 0) {
        logPrintf("[enqueueOwn] Kanal '%s' nicht gefunden\n", channel);
        return false;
    }
    const char *sender = (channelPsks[chIdx].senderName[0] != '\0')
        ? channelPsks[chIdx].senderName : deviceName;
    uint8_t packet[PACKET_MAX];
    int len = buildPacket(sender, text, timestamp,
                          channelPsks[chIdx].psk, channelPsks[chIdx].networkId, packet);
    if (len <= 0 || !enqueueSend(packet, len, QUEUE_OWN)) return false;
    strncpy(sendQueue[0].ownSender, sender, 20);  sendQueue[0].ownSender[20] = '\0';
    strncpy(sendQueue[0].ownText,   text,   182); sendQueue[0].ownText[182]   = '\0';
    strncpy(sendQueue[0].ownChannel, channel, CHANNEL_NAME_MAX);
    sendQueue[0].ownChannel[CHANNEL_NAME_MAX] = '\0';
    sendQueue[0].ownTimestamp = timestamp;
    return true;
}

// ----------------------------------------------------------------
// Loop
// ----------------------------------------------------------------
void loop() {
    // ── Outgoing aus WebUI übernehmen → Paket bauen → Queue ─────
    if (xSemaphoreTake(outMutex, 0) == pdTRUE) {
        if (outgoing.pending) {
            uint32_t outTs = outgoing.timestamp;
            if (outTs != 0) {
                timeRefUnixSec   = outTs;
                timeRefMillis    = millis();
                hasTimeReference = true;
            }

            int chIdx = -1;
            for (int i = 0; i < channelPskCount; i++) {
                if (strcmp(channelPsks[i].name, outgoing.channel) == 0) {
                    chIdx = i;
                    break;
                }
            }

            if (chIdx < 0) {
                logPrintf("[LoRa] Kanal '%s' nicht gefunden\n", outgoing.channel);
                outgoing.lastResult = SEND_STATUS_NO_CHANNEL;
            } else {
                const char *senderForPacket = deviceName;
                if (channelPsks[chIdx].senderName[0] != '\0')
                    senderForPacket = channelPsks[chIdx].senderName;
                uint8_t packet[PACKET_MAX];
                int packetLen = buildPacket(senderForPacket, outgoing.text, outTs,
                                            channelPsks[chIdx].psk, channelPsks[chIdx].networkId,
                                            packet);
                if (packetLen > 0 && enqueueSend(packet, packetLen, QUEUE_OWN)) {
                    strncpy(sendQueue[0].ownSender, senderForPacket, 20);
                    sendQueue[0].ownSender[20] = '\0';
                    strncpy(sendQueue[0].ownText, outgoing.text, 182);
                    sendQueue[0].ownText[182] = '\0';
                    strncpy(sendQueue[0].ownChannel, channelPsks[chIdx].name, CHANNEL_NAME_MAX);
                    sendQueue[0].ownChannel[CHANNEL_NAME_MAX] = '\0';
                    sendQueue[0].ownTimestamp = outTs;
                } else {
                    outgoing.lastResult = SEND_STATUS_ERROR;
                }
            }
            outgoing.pending = false;
        }
        xSemaphoreGive(outMutex);
    }

    if (!pskLoaded) {
        // Queue-Abarbeitung auch ohne PSK — Fix 2026-06-25 (Pairing-Bug):
        // Ohne diesen Block wurde ECDH_RESP nie gesendet, weil der frühere
        // Early-Return die Queue nie erreichte. QUEUE_PAIRING-Pakete müssen
        // auch im unkonfigurierten Zustand gesendet werden können.
        pairingTick();
#ifdef BUTTON_PIN
        handleButtonTick();
#endif
        if (loraDutyCycleRemainingMs() == 0 && sendQueueCount > 0) {
            SendQueueEntry &front = sendQueue[0];
            LoraSendResult qr = loraSend(front.data, front.len);
            if (qr == LORA_SEND_OK) {
                logPrintf("[MeshQ] PAIRING gesendet — OK (Wartezeit: %lus)\n",
                    (millis() - front.enqueuedAt) / 1000);
                removeQueueFront();
            } else {
                logPrintf("[MeshQ] PAIRING senden fehlgeschlagen: %d\n", qr);
            }
        }
        uint8_t bufNoPsk[PACKET_MAX];
        int resNoPsk = loraReceive(bufNoPsk, PACKET_MAX);
        if (resNoPsk > 0) {
            logPrintf("[LoRa] Paket empfangen (kein PSK), Länge: %d, RSSI: %.0f\n",
                resNoPsk, loraRSSI());
            if (resNoPsk >= PAIRING_HEADER_LEN && bufNoPsk[0] == PAIRING_MAGIC) {
                pairingHandlePacket(bufNoPsk, resNoPsk);
            }
        }
        delay(10);
        return;
    }

    // ── Queue abarbeiten ────────────────────────────────────────
    // dutyCycleReason vor der Queue-Abarbeitung zurücksetzen, sobald die
    // DC-Pause abgelaufen ist. Sonst bleibt im Frontend "eigene Nachricht /
    // Weiterleitung" stehen, obwohl das Gerät längst wieder sendebereit ist.
    if (loraDutyCycleRemainingMs() == 0) {
        dutyCycleReason = DC_REASON_NONE;
    }
    if (loraDutyCycleRemainingMs() == 0 && sendQueueCount > 0) {
        while (sendQueueCount > 0) {
            SendQueueEntry &front = sendQueue[0];

            if (front.type == QUEUE_RELAY) {
                unsigned long age = millis() - front.enqueuedAt;
                if (age > RELAY_TTL_MS) {
                    logPrintf("[MeshQ] RELAY verfallen nach %lus — verworfen\n", age / 1000);
                    removeQueueFront();
                    continue;
                }
            }

            LoraSendResult result = loraSend(front.data, front.len);
            const char *typeStr = (front.type == QUEUE_OWN) ? "OWN" :
                                  (front.type == QUEUE_PAIRING) ? "PAIRING" : "RELAY";

            if (result == LORA_SEND_OK) {
                unsigned long waitTime = (millis() - front.enqueuedAt) / 1000;
                dutyCycleReason = (front.type == QUEUE_RELAY) ? DC_REASON_RELAY : DC_REASON_OWN;
                logPrintf("[MeshQ] %s gesendet — OK (Wartezeit: %lus)\n", typeStr, waitTime);

                if (front.type == QUEUE_OWN) {
                    addMessage(front.ownSender, front.ownText, true, 0.0f, 0,
                              loraLastAirtimeMs(), front.ownTimestamp, front.ownChannel);
                    if (xSemaphoreTake(outMutex, 0) == pdTRUE) {
                        outgoing.lastResult = SEND_STATUS_OK;
                        xSemaphoreGive(outMutex);
                    }
                }
                removeQueueFront();
                break;
            } else if (result == LORA_SEND_DUTYCYCLE) {
                logPrintf("[MeshQ] DC neu aufgebaut — warte\n");
                break;
            } else if (result == LORA_SEND_CHANNEL_BUSY) {
                logPrintf("[MeshQ] BUSY — retry nächste Iteration\n");
                break;
            } else {
                logPrintf("[MeshQ] ERROR — Eintrag verworfen (%s)\n", typeStr);
                if (front.type == QUEUE_OWN) {
                    if (xSemaphoreTake(outMutex, 0) == pdTRUE) {
                        outgoing.lastResult = SEND_STATUS_ERROR;
                        xSemaphoreGive(outMutex);
                    }
                }
                removeQueueFront();
                break;
            }
        }
    }

    // ── Heartbeat ───────────────────────────────────────────────
    static unsigned long lastHeartbeat = 0;
    {
        unsigned long now = millis();
        if (now - lastHeartbeat >= 30000) {
            lastHeartbeat = now;
            logPrintf("[Status] Uptime: %lus | Heap: %lu B | Kanäle: %d | DC-Pause: %lums | Queue: %d/%d\n",
                now / 1000, esp_get_free_heap_size(),
                channelPskCount,
                loraDutyCycleRemainingMs(),
                sendQueueCount, SEND_QUEUE_SIZE);
        }
    }

    // ── Pairing-Tick ──────────────────────────────────────────────
    pairingTick();

    // ── Autoreply-Tick — verzögerter Send nach meshHandleIncoming ─
    meshAutoReplyTick();

#ifdef BUTTON_PIN
    handleButtonTick();
#endif

    // ── LoRa-Empfang ────────────────────────────────────────────
    uint8_t buf[PACKET_MAX];
    int result = loraReceive(buf, PACKET_MAX);
    if (result > 0) {
        logPrintf("[LoRa] Paket empfangen, Länge: %d, RSSI: %.0f\n",
            result, loraRSSI());
        if (result >= PAIRING_HEADER_LEN && buf[0] == PAIRING_MAGIC) {
            pairingHandlePacket(buf, result);
        } else {
            meshHandleIncoming(buf, result);
        }
    } else if (result == -2) {
        logPrintf("[LoRa] Empfangsfehler\n");
    }

    // ── Display Tick ─────────────────────────────────────────────
    {
        DisplayContext dctx;
        dctx.ssid           = wifiSSID;
        dctx.wifiPass       = wifiPass;
        dctx.activeChannel  = activeChannel;
        dctx.pskLoaded      = pskLoaded;
        dctx.unreadMessages = unreadMessages;
        dctx.dutyCycleMs    = loraDutyCycleRemainingMs();
        displayTick(dctx);
    }
}

// ----------------------------------------------------------------
// Paket bauen (ohne Senden — Queue übernimmt das)
// ----------------------------------------------------------------
int buildPacket(const char *sender, const char *text, uint32_t timestamp,
                const uint8_t *usePsk, const uint8_t *useNetId,
                uint8_t *outBuf) {
    memset(outBuf, 0, PACKET_MAX);

    outBuf[0] = PROTOCOL_VERSION;
    outBuf[1] = useNetId[0];
    outBuf[2] = useNetId[1];
    outBuf[3] = nodeId[0];
    outBuf[4] = nodeId[1];
    outBuf[5] = (seqNum >> 8) & 0xFF;
    outBuf[6] =  seqNum       & 0xFF;
    outBuf[7] = FLAG_CHAT;
    uint32_t thisMsgId = ((uint32_t)nodeId[0] << 24) | ((uint32_t)nodeId[1] << 16) |
                         ((uint32_t)((seqNum >> 8) & 0xFF) << 8) | (seqNum & 0xFF);
    storageWriteSeqNum(seqNum);
    seqNum++;

    uint8_t iv[12];
    generateIV(iv);
    memcpy(outBuf + 8, iv, 12);

    uint8_t  textLen    = strnlen(text, 182);
    uint16_t payloadLen = 20 + 1 + 4 + textLen;
    uint8_t  plaintext[payloadLen];
    memset(plaintext, 0, payloadLen);
    memcpy(plaintext, sender, strnlen(sender, 20));
    plaintext[20] = 0x00;
    plaintext[21] =  timestamp        & 0xFF;
    plaintext[22] = (timestamp >>  8) & 0xFF;
    plaintext[23] = (timestamp >> 16) & 0xFF;
    plaintext[24] = (timestamp >> 24) & 0xFF;
    memcpy(plaintext + 25, text, textLen);

    bool ok = encryptPayload(usePsk, iv, outBuf, 8, plaintext, payloadLen,
                             outBuf+20, outBuf+20+payloadLen);
    if (!ok) return 0;

    meshAddToDedup(thisMsgId);

    uint16_t packetLen = 8 + 12 + payloadLen + 16;
    logPrintf("[LoRa] Paket gebaut, %d Byte\n", packetLen);
    return packetLen;
}

// ----------------------------------------------------------------
// Web-Task
// ----------------------------------------------------------------
void webTask(void *pvParameters) {
    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
        if (event == ARDUINO_EVENT_WIFI_AP_STACONNECTED) {
            logPrintf("[WiFi] Client verbunden:  %02X:%02X:%02X:%02X:%02X:%02X\n",
                info.wifi_ap_staconnected.mac[0], info.wifi_ap_staconnected.mac[1],
                info.wifi_ap_staconnected.mac[2], info.wifi_ap_staconnected.mac[3],
                info.wifi_ap_staconnected.mac[4], info.wifi_ap_staconnected.mac[5]);
        } else if (event == ARDUINO_EVENT_WIFI_AP_STADISCONNECTED) {
            logPrintf("[WiFi] Client getrennt:   %02X:%02X:%02X:%02X:%02X:%02X\n",
                info.wifi_ap_stadisconnected.mac[0], info.wifi_ap_stadisconnected.mac[1],
                info.wifi_ap_stadisconnected.mac[2], info.wifi_ap_stadisconnected.mac[3],
                info.wifi_ap_stadisconnected.mac[4], info.wifi_ap_stadisconnected.mac[5]);
        }
    });

    WiFi.softAP(wifiSSID, wifiPass);
    logPrintf("[WiFi] AP: %s  PW: %s  IP: %s\n", wifiSSID, wifiPass,
        WiFi.softAPIP().toString().c_str());

    DNSServer dnsServer;
    dnsServer.start(53, "*", WiFi.softAPIP());
    logPrintf("[DNS] Captive Portal DNS gestartet\n");

    setupWebUI(server, messages, &msgCount, &msgHead, &msgTotalCount, msgMutex, &outgoing, outMutex, deviceName, &dutyCycleReason, activeChannel);

    server.on("/channels", HTTP_GET, [](AsyncWebServerRequest *req) {
        String json = "{\"channels\":[";
        for (int i = 0; i < channelPskCount; i++) {
            if (i > 0) json += ",";
            String name = channelPsks[i].name;
            name.replace("\\", "\\\\");
            name.replace("\"", "\\\"");
            json += "\"" + name + "\"";
        }
        json += "]}";
        logPrintf("[Portal] %s %s → 200\n", req->methodToString(), req->url().c_str());
        req->send(200, "application/json", json);
    });

    setupConfigRoutes(server);
    setupPairingRoutes(server);

#ifdef DEVELOPMENT
    server.on("/debug/check", HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send(200, "text/plain", "dev");
    });

    server.on("/debug/setseq", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (!request->hasParam("val", true)) {
            request->send(400, "text/plain", "Missing val");
            return;
        }
        uint16_t val = (uint16_t)request->getParam("val", true)->value().toInt();
        seqNum = val;
        storageClearSeqSlots();
        for (int i = 0; i < SEQ_SLOTS; i++) {
            storageWriteSeqNum((uint16_t)(val - SEQ_SLOTS + i));
        }
        char resp[64];
        snprintf(resp, sizeof(resp), "SeqNum auf %u gesetzt", val);
        request->send(200, "text/plain", resp);
        logPrintf("[Debug] SeqNum manuell auf %u gesetzt\n", val);
    });

    server.on("/debug/fillmessages", HTTP_POST, [](AsyncWebServerRequest *request) {
        int count = 55;
        if (request->hasParam("count", true)) {
            count = request->getParam("count", true)->value().toInt();
            if (count < 1) count = 1;
            if (count > 200) count = 200;
        }
        uint32_t baseTs = 1000000;
        if (hasTimeReference) {
            baseTs = timeRefUnixSec + (millis() - timeRefMillis) / 1000;
        }
        for (int i = 0; i < count; i++) {
            char text[32];
            snprintf(text, sizeof(text), "Testnachricht %d", i + 1);
            addMessage("Test", text, false, 0.0f, 0, 0, baseTs + i, "Familie");
        }
        char resp[64];
        snprintf(resp, sizeof(resp), "OK — %d Nachrichten erzeugt", count);
        request->send(200, "text/plain", resp);
        storageLogNvsStats();
    });
#endif

    server.begin();
    logPrintf("[Web] Server gestartet\n");
    logPrintf("[Web] Konfigoberfläche: http://%s/config.html\n",
        WiFi.softAPIP().toString().c_str());

    while (true) {
        dnsServer.processNextRequest();
        logDrain();
        vTaskDelay(50 / portTICK_PERIOD_MS);
    }
}

// ----------------------------------------------------------------
// Nachricht in Ringpuffer schreiben
// ----------------------------------------------------------------
void addMessage(const char *sender, const char *text, bool isOwn, float rssi, int snr, unsigned long airtimeMs, uint32_t timestamp, const char *channel) {
    if (xSemaphoreTake(msgMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        messages[msgHead].valid = true;
        strncpy(messages[msgHead].sender, sender, 20);
        messages[msgHead].sender[20] = '\0';
        strncpy(messages[msgHead].text, text, 182);
        messages[msgHead].text[182] = '\0';
        strncpy(messages[msgHead].channel, channel, CHANNEL_NAME_MAX);
        messages[msgHead].channel[CHANNEL_NAME_MAX] = '\0';
        messages[msgHead].isOwn = isOwn;
        messages[msgHead].rssi = rssi;
        messages[msgHead].snr = snr;
        messages[msgHead].airtimeMs = airtimeMs;
        messages[msgHead].timestamp = timestamp;
        msgHead = (msgHead + 1) % MAX_MESSAGES;
        if (msgCount < MAX_MESSAGES) msgCount++;
        msgTotalCount++;
        storageWriteMessage(sender, text, channel, timestamp, isOwn);
        xSemaphoreGive(msgMutex);
    }
}
