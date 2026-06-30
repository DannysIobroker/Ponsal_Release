// webui.cpp
// Webserver-Routen: Chat-Interface mit Mehrkanal-Navigation, Nachrichten-API, Sende-Endpunkt

#include "webui.h"
#include "lora_hal.h"
#include <Arduino.h>
#include "serial_log.h"

#include "webui_html.h"

volatile uint32_t unreadMessages = 0;

static void portalLog(AsyncWebServerRequest *req, int code) {
    logPrintf("[Portal] %s %s → %d\n", req->methodToString(), req->url().c_str(), code);
}

// ── Shared State (von main.cpp übergeben) ─────────────────────
static Message           *_messages;
static uint8_t           *_msgCount;
static uint8_t           *_msgHead;
static uint32_t          *_msgTotalCount;
static SemaphoreHandle_t  _msgMutex;
static OutgoingMsg       *_outgoing;
static SemaphoreHandle_t  _outMutex;
static const char        *_deviceName;
static volatile DutyCycleReason *_dcReason = nullptr;
static const char        *_activeChannel;

// ── Routen einrichten ──────────────────────────────────────────
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
) {
    _messages      = messages;
    _msgCount      = msgCount;
    _msgHead       = msgHead;
    _msgTotalCount = msgTotalCount;
    _msgMutex      = msgMutex;
    _outgoing      = outgoing;
    _outMutex      = outMutex;
    _deviceName    = deviceName;
    _dcReason      = dcReason;
    _activeChannel = activeChannel;

    // ── GET / — Chat-Interface ─────────────────────────────────
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
        portalLog(req, 200);
        req->send(200, "text/html", HTML);
    });

    // ── GET /messages?since=N&channel=X — Nachrichten als JSON ──
    server.on("/messages", HTTP_GET, [](AsyncWebServerRequest *req) {
        uint32_t since = 0;
        if (req->hasParam("since")) {
            since = (uint32_t)req->getParam("since")->value().toInt();
        }
        String channelFilter = "";
        if (req->hasParam("channel")) {
            channelFilter = req->getParam("channel")->value();
        }

        String json = "{\"device\":\"" + String(_deviceName) + "\"," +
                      "\"dutyCycleMs\":" + String(loraDutyCycleRemainingMs()) + "," +
                      "\"dutyCycleReason\":" + String(_dcReason ? (int)(*_dcReason) : 0) + ",";

        SendStatus result = SEND_STATUS_NONE;
        if (xSemaphoreTake(_outMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            result = _outgoing->lastResult;
            _outgoing->lastResult = SEND_STATUS_NONE;
            xSemaphoreGive(_outMutex);
        }
        json += "\"sendResult\":" + String((int)result) + ",";

        String msgs = "";
        int count = 0;

        if (xSemaphoreTake(_msgMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            uint32_t total = *_msgTotalCount;
            uint8_t  filled = *_msgCount;
            json += "\"total\":" + String(total) + ",\"messages\":[";

            int start = 0;
            int num = filled;
            if (filled == MAX_MESSAGES) {
                start = *_msgHead;
            }

            uint32_t oldestGlobalIdx = total - filled;

            for (int i = 0; i < num; i++) {
                int idx = (start + i) % MAX_MESSAGES;
                if (!_messages[idx].valid) continue;

                uint32_t globalIdx = oldestGlobalIdx + i;
                if (globalIdx < since) continue;

                if (channelFilter.length() > 0 &&
                    strcmp(_messages[idx].channel, channelFilter.c_str()) != 0) {
                    continue;
                }

                if (count > 0) msgs += ",";

                String senderStr = _messages[idx].sender;
                String textStr   = _messages[idx].text;
                senderStr.replace("\\", "\\\\");
                senderStr.replace("\"", "\\\"");
                textStr.replace("\\", "\\\\");
                textStr.replace("\"", "\\\"");

                msgs += "{\"sender\":\"" + senderStr + "\",\"text\":\"" + textStr + "\"";
                msgs += ",\"channel\":\"" + String(_messages[idx].channel) + "\"";
                msgs += ",\"rssi\":" + String((int)_messages[idx].rssi);
                msgs += ",\"snr\":" + String(_messages[idx].snr);
                msgs += ",\"airtimeMs\":" + String(_messages[idx].airtimeMs);
                msgs += ",\"timestamp\":" + String(_messages[idx].timestamp);
                // isOwn aus dem Message-Struct, nicht per Namensvergleich —
                // kanalspezifische Absendernamen ("Dorfinfoschef") stimmen
                // nicht mit deviceName überein und würden eigene Nachrichten
                // als fremd markieren.
                msgs += ",\"isOwn\":" + String(_messages[idx].isOwn ? "true" : "false");
                msgs += "}";
                count++;
            }

            xSemaphoreGive(_msgMutex);
        }

        json += msgs + "]}";
        // Browser hat Nachrichten abgeholt → Unread-Counter zurücksetzen
        unreadMessages = 0;
        portalLog(req, 200);
        req->send(200, "application/json", json);
    });

    // ── POST /send — Nachricht senden ──────────────────────────
    server.on("/send", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!req->hasParam("text", true)) {
            portalLog(req, 400);
            req->send(400, "text/plain", "text erforderlich");
            return;
        }

        String text = req->getParam("text", true)->value();
        text.trim();

        if (text.length() == 0) {
            portalLog(req, 400);
            req->send(400, "text/plain", "Leer");
            return;
        }

        if (text.length() > 182) text = text.substring(0, 182);

        String channel = "";
        if (req->hasParam("channel", true)) {
            channel = req->getParam("channel", true)->value();
        }
        if (channel.length() == 0) {
            channel = String(_activeChannel);
        }

        logPrintf("[Web] /send ch=%s: '%s'\n", channel.c_str(), text.c_str());

        uint32_t ts = 0;
        if (req->hasParam("ts", true)) {
            ts = (uint32_t)(req->getParam("ts", true)->value().toDouble() / 1000.0);
        }

        if (xSemaphoreTake(_outMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            if (_outgoing->pending) {
                xSemaphoreGive(_outMutex);
                portalLog(req, 503);
                req->send(503, "text/plain", "Vorherige Nachricht noch ausstehend");
                return;
            }
            strncpy(_outgoing->text, text.c_str(), 182);
            _outgoing->text[182] = '\0';
            strncpy(_outgoing->channel, channel.c_str(), CHANNEL_NAME_MAX);
            _outgoing->channel[CHANNEL_NAME_MAX] = '\0';
            _outgoing->pending = true;
            _outgoing->lastResult = SEND_STATUS_NONE;
            _outgoing->timestamp = ts;
            xSemaphoreGive(_outMutex);
        }

        portalLog(req, 200);
        req->send(200, "text/plain", "OK");
    });

    // ── Captive Portal — OS-spezifische Endpunkte ───────────
    // Diese Routen MÜSSEN vor onNotFound registriert werden: onNotFound ist
    // ein Catch-All, der spezifische Routen danach nicht mehr erreichbar macht.
    server.on("/generate_204", HTTP_GET, [](AsyncWebServerRequest *req) {
        portalLog(req, 204);
        req->send(204);
    });
    server.on("/hotspot-detect.html", HTTP_GET, [](AsyncWebServerRequest *req) {
        portalLog(req, 200);
        req->send(200, "text/html", "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
    });
    server.on("/ncsi.txt", HTTP_GET, [](AsyncWebServerRequest *req) {
        portalLog(req, 200);
        req->send(200, "text/plain", "Microsoft NCSI");
    });
    server.on("/canonical.html", HTTP_GET, [](AsyncWebServerRequest *req) {
        portalLog(req, 200);
        req->send(200, "text/html", "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
    });

    // ── Captive Portal Redirect (Fallback) ───────────────────
    server.onNotFound([](AsyncWebServerRequest *req) {
        portalLog(req, 302);
        req->redirect("http://192.168.4.1/");
    });
}
