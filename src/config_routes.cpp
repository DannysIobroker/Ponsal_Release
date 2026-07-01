#include "config_routes.h"
#include <Arduino.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include "storage.h"
#include "mesh.h"
#include "lora_hal.h"
#include "serial_log.h"

extern uint32_t settingsPin;
extern char deviceName[21];
extern char wifiSSID[32];
extern char wifiPass[64];
extern uint8_t loraPreset;
extern bool autoReplyEnabled;
extern uint8_t showWifiPw;
extern uint8_t showSettingsPin;
extern ChannelPsk channelPsks[];
extern uint8_t channelPskCount;
extern void reloadChannelPsks();

static void portalLog(AsyncWebServerRequest *req, int code) {
    logPrintf("[Portal] %s %s → %d\n", req->methodToString(), req->url().c_str(), code);
}

void setupConfigRoutes(AsyncWebServer &server) {
    // REIHENFOLGE KRITISCH: /config/get/psk MUSS vor /config/get registriert werden.
    // AsyncWebServer matcht Pfad-Präfixe — /config/get würde sonst alle
    // /config/get/*-Anfragen abfangen, bevor die spezifischere Route greift.

    server.on("/config.html", HTTP_GET, [](AsyncWebServerRequest *req) {
        if (SPIFFS.exists("/config.html")) {
            portalLog(req, 200);
            req->send(SPIFFS, "/config.html", "text/html");
        } else {
            portalLog(req, 404);
            req->send(404, "text/plain", "config.html nicht gefunden");
        }
    });

    server.on("/config/auth", HTTP_POST, [](AsyncWebServerRequest *req) {},
    nullptr, [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t, size_t) {
        JsonDocument doc;
        if (deserializeJson(doc, data, len) != DeserializationError::Ok) {
            portalLog(req, 400); req->send(400); return;
        }
        uint32_t sentPin = doc["pin"].as<String>().toInt();
        if (sentPin == settingsPin) {
            portalLog(req, 200); req->send(200, "application/json", "{\"ok\":true}");
        } else {
            portalLog(req, 403); req->send(403, "text/plain", "Falsche PIN");
        }
    });

    server.on("/config/get/psk", HTTP_GET, [](AsyncWebServerRequest *req) {
        if (!req->hasHeader("X-Pin") ||
            (uint32_t)req->header("X-Pin").toInt() != settingsPin) {
            portalLog(req, 403); req->send(403); return;
        }
        if (!req->hasParam("channel")) {
            portalLog(req, 400); req->send(400, "text/plain", "channel fehlt"); return;
        }
        String channelName = req->getParam("channel")->value();
        uint8_t pskBuf[32];
        if (!storageLoadPsk(channelName.c_str(), pskBuf)) {
            portalLog(req, 404); req->send(404, "text/plain", "Kanal nicht gefunden"); return;
        }
        char hexStr[65] = {0};
        for (int i = 0; i < 32; i++) sprintf(hexStr + i*2, "%02X", pskBuf[i]);
        char pskJson[80];
        snprintf(pskJson, sizeof(pskJson), "{\"psk\":\"%s\"}", hexStr);
        portalLog(req, 200); req->send(200, "application/json", String(pskJson));
    });

    server.on("/config/get", HTTP_GET, [](AsyncWebServerRequest *req) {
        if (!req->hasHeader("X-Pin") ||
            (uint32_t)req->header("X-Pin").toInt() != settingsPin) {
            portalLog(req, 403); req->send(403); return;
        }
        JsonDocument doc;
        doc["name"]            = deviceName;
        doc["preset"]          = loraPreset;
        doc["autoReply"]       = autoReplyEnabled;
        doc["showWifiPw"]      = (bool)(showWifiPw != 0);
        doc["showSettingsPin"] = (bool)(showSettingsPin != 0);

        char channelNames[MAX_CHANNELS][CHANNEL_NAME_MAX + 1];
        uint8_t channelCount = 0;
        storageLoadChannelNames(channelNames, &channelCount);

        JsonArray channels = doc["channels"].to<JsonArray>();
        for (int i = 0; i < channelCount; i++) {
            JsonObject ch = channels.add<JsonObject>();
            ch["name"] = channelNames[i];
            for (int j = 0; j < channelPskCount; j++) {
                if (strcmp(channelPsks[j].name, channelNames[i]) == 0) {
                    ch["senderName"] = channelPsks[j].senderName;
                    ch["nameLocked"] = channelPsks[j].nameLocked;
                    break;
                }
            }
        }

        String out;
        serializeJson(doc, out);
        portalLog(req, 200); req->send(200, "application/json", out);
    });

    server.on("/config/set/name", HTTP_POST, [](AsyncWebServerRequest *req) {},
    nullptr, [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t, size_t) {
        if (!req->hasHeader("X-Pin") ||
            (uint32_t)req->header("X-Pin").toInt() != settingsPin) {
            portalLog(req, 403); req->send(403); return;
        }
        JsonDocument doc;
        if (deserializeJson(doc, data, len) != DeserializationError::Ok) {
            portalLog(req, 400); req->send(400); return;
        }
        const char *name = doc["name"];
        if (!name || strlen(name) == 0 || strlen(name) > 20) {
            portalLog(req, 400); req->send(400, "text/plain", "Ungültiger Name"); return;
        }
        if (storageStoreDeviceName(name)) {
            strncpy(deviceName, name, 20);
            deviceName[20] = '\0';
            portalLog(req, 200); req->send(200, "application/json", "{\"ok\":true}");
        } else {
            portalLog(req, 500); req->send(500, "text/plain", "Speichern fehlgeschlagen");
        }
    });

    server.on("/config/set/channelname", HTTP_POST, [](AsyncWebServerRequest *req) {},
    nullptr, [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t, size_t) {
        if (!req->hasHeader("X-Pin") ||
            (uint32_t)req->header("X-Pin").toInt() != settingsPin) {
            portalLog(req, 403); req->send(403); return;
        }
        JsonDocument doc;
        if (deserializeJson(doc, data, len) != DeserializationError::Ok) {
            portalLog(req, 400); req->send(400); return;
        }
        const char *channel = doc["channel"];
        const char *senderName = doc["senderName"];
        if (!channel || strlen(channel) == 0) {
            portalLog(req, 400); req->send(400, "text/plain", "Kein Kanal angegeben"); return;
        }
        for (int i = 0; i < channelPskCount; i++) {
            if (strcmp(channelPsks[i].name, channel) == 0 && channelPsks[i].nameLocked) {
                portalLog(req, 403); req->send(403, "text/plain", "Name ist gesperrt"); return;
            }
        }
        if (!senderName || strlen(senderName) == 0) {
            storageDeleteChannelSenderName(channel);
        } else {
            if (strlen(senderName) > 20) {
                portalLog(req, 400); req->send(400, "text/plain", "Name zu lang"); return;
            }
            storageStoreChannelSenderName(channel, senderName);
        }
        reloadChannelPsks();
        portalLog(req, 200); req->send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/config/set/channel", HTTP_POST, [](AsyncWebServerRequest *req) {},
    nullptr, [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t, size_t) {
        if (!req->hasHeader("X-Pin") ||
            (uint32_t)req->header("X-Pin").toInt() != settingsPin) {
            portalLog(req, 403); req->send(403); return;
        }
        JsonDocument doc;
        if (deserializeJson(doc, data, len) != DeserializationError::Ok) {
            portalLog(req, 400); req->send(400); return;
        }
        const char *name = doc["name"];
        if (!storageValidateChannelName(name)) {
            portalLog(req, 400); req->send(400, "text/plain", "Ungültiger Kanalname (max. 10 Zeichen, keine Sonderzeichen)"); return;
        }

        uint8_t newPsk[32];
        esp_fill_random(newPsk, 32);

        if (storageStorePsk(name, newPsk)) {
            reloadChannelPsks();
            portalLog(req, 200); req->send(200, "application/json", "{\"ok\":true}");
        } else {
            portalLog(req, 500); req->send(500, "text/plain", "Speichern fehlgeschlagen (Kanal-Limit erreicht?)");
        }
    });

    server.on("/config/delete/channel", HTTP_POST, [](AsyncWebServerRequest *req) {},
    nullptr, [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t, size_t) {
        if (!req->hasHeader("X-Pin") ||
            (uint32_t)req->header("X-Pin").toInt() != settingsPin) {
            portalLog(req, 403); req->send(403); return;
        }
        JsonDocument doc;
        if (deserializeJson(doc, data, len) != DeserializationError::Ok) {
            portalLog(req, 400); req->send(400); return;
        }
        const char *name = doc["name"];
        if (!name) { portalLog(req, 400); req->send(400); return; }

        storageDeletePsk(name);
        storageDeleteChannelSenderName(name);
        storageDeleteChannelNameLock(name);
        reloadChannelPsks();
        portalLog(req, 200); req->send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/config/set/preset", HTTP_POST, [](AsyncWebServerRequest *req) {},
    nullptr, [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t, size_t) {
        if (!req->hasHeader("X-Pin") ||
            (uint32_t)req->header("X-Pin").toInt() != settingsPin) {
            portalLog(req, 403); req->send(403); return;
        }
        JsonDocument doc;
        if (deserializeJson(doc, data, len) != DeserializationError::Ok) {
            portalLog(req, 400); req->send(400); return;
        }
        uint8_t preset = doc["preset"] | 0;
        if (preset >= PRESET_COUNT) { portalLog(req, 400); req->send(400, "text/plain", "Ungültiger Preset"); return; }
        if (storageStorePreset(preset)) {
            loraPreset = preset;
            if (!loraSetPreset(preset)) {
                logPrintf("[Config] Preset-Wechsel auf Radio fehlgeschlagen\n");
            }
            portalLog(req, 200); req->send(200, "application/json", "{\"ok\":true}");
        } else {
            portalLog(req, 500); req->send(500, "text/plain", "Speichern fehlgeschlagen");
        }
    });

    server.on("/config/set/psk", HTTP_POST, [](AsyncWebServerRequest *req) {},
    nullptr, [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t, size_t) {
        if (!req->hasHeader("X-Pin") ||
            (uint32_t)req->header("X-Pin").toInt() != settingsPin) {
            portalLog(req, 403); req->send(403); return;
        }
        JsonDocument doc;
        if (deserializeJson(doc, data, len) != DeserializationError::Ok) {
            portalLog(req, 400); req->send(400); return;
        }
        const char *name   = doc["name"];
        const char *hexStr = doc["psk"];
        if (!storageValidateChannelName(name) || !hexStr || strlen(hexStr) != 64) {
            portalLog(req, 400); req->send(400, "text/plain", "Ungültige Daten (Kanalname max. 10 Zeichen)"); return;
        }
        uint8_t newPsk[32];
        for (int i = 0; i < 32; i++) {
            char byte[3] = {hexStr[i*2], hexStr[i*2+1], 0};
            char *end;
            long val = strtol(byte, &end, 16);
            if (*end != 0) {
                portalLog(req, 400); req->send(400, "text/plain", "Ungültiger Hex-String"); return;
            }
            newPsk[i] = (uint8_t)val;
        }
        if (storageStorePsk(name, newPsk)) {
            reloadChannelPsks();
            portalLog(req, 200); req->send(200, "application/json", "{\"ok\":true}");
        } else {
            portalLog(req, 500); req->send(500, "text/plain", "Speichern fehlgeschlagen");
        }
    });

    server.on("/config/set/autoreply", HTTP_POST, [](AsyncWebServerRequest *req) {},
    nullptr, [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t, size_t) {
        if (!req->hasHeader("X-Pin") ||
            (uint32_t)req->header("X-Pin").toInt() != settingsPin) {
            portalLog(req, 403); req->send(403); return;
        }
        JsonDocument doc;
        if (deserializeJson(doc, data, len) != DeserializationError::Ok) {
            portalLog(req, 400); req->send(400); return;
        }
        autoReplyEnabled = doc["enabled"] | false;
        logPrintf("[Config] Auto-Reply (RSSI-Echo): %s\n", autoReplyEnabled ? "an" : "aus");
        portalLog(req, 200); req->send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/config/set/displaytoggle", HTTP_POST, [](AsyncWebServerRequest *req) {},
    nullptr, [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t, size_t) {
        if (!req->hasHeader("X-Pin") ||
            (uint32_t)req->header("X-Pin").toInt() != settingsPin) {
            portalLog(req, 403); req->send(403); return;
        }
        JsonDocument doc;
        if (deserializeJson(doc, data, len) != DeserializationError::Ok) {
            portalLog(req, 400); req->send(400); return;
        }
        const char *field = doc["field"];
        if (!field) { portalLog(req, 400); req->send(400, "text/plain", "field fehlt"); return; }
        bool show = doc["show"] | true;
        uint8_t val = show ? 1 : 0;
        if (strcmp(field, "wifi_pw") == 0) {
            storageStoreShowWifiPw(val);
            showWifiPw = val;
        } else if (strcmp(field, "settings_pin") == 0) {
            storageStoreShowPin(val);
            showSettingsPin = val;
        } else {
            portalLog(req, 400); req->send(400, "text/plain", "Unbekanntes field"); return;
        }
        portalLog(req, 200); req->send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/config/set/pin", HTTP_POST, [](AsyncWebServerRequest *req) {},
    nullptr, [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t, size_t) {
        if (!req->hasHeader("X-Pin") ||
            (uint32_t)req->header("X-Pin").toInt() != settingsPin) {
            portalLog(req, 403); req->send(403); return;
        }
        JsonDocument doc;
        if (deserializeJson(doc, data, len) != DeserializationError::Ok) {
            portalLog(req, 400); req->send(400); return;
        }
        uint32_t newPin = doc["pin"].as<String>().toInt();
        if (newPin > 999999) { portalLog(req, 400); req->send(400, "text/plain", "Ungültige PIN"); return; }
        if (storageStoreSettingsPin(newPin)) {
            settingsPin = newPin;
            portalLog(req, 200); req->send(200, "application/json", "{\"ok\":true}");
        } else {
            portalLog(req, 500); req->send(500, "text/plain", "Speichern fehlgeschlagen");
        }
    });

    server.on("/config/set/wifipass", HTTP_POST, [](AsyncWebServerRequest *req) {},
    nullptr, [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t, size_t) {
        if (!req->hasHeader("X-Pin") ||
            (uint32_t)req->header("X-Pin").toInt() != settingsPin) {
            portalLog(req, 403); req->send(403); return;
        }
        JsonDocument doc;
        if (deserializeJson(doc, data, len) != DeserializationError::Ok) {
            portalLog(req, 400); req->send(400); return;
        }
        String pw = doc["password"].as<String>();
        if (pw.length() < 8) {
            portalLog(req, 400);
            req->send(400, "application/json", "{\"error\":\"Passwort muss mindestens 8 Zeichen haben\"}");
            return;
        }
        if (pw.length() > 63) {
            portalLog(req, 400);
            req->send(400, "application/json", "{\"error\":\"Passwort darf höchstens 63 Zeichen haben\"}");
            return;
        }
        for (size_t i = 0; i < pw.length(); i++) {
            char c = pw[i];
            if (c < 32 || c > 126) {
                portalLog(req, 400);
                req->send(400, "application/json",
                    "{\"error\":\"Passwort enth\\u00e4lt ung\\u00fcltige Zeichen (nur Buchstaben, Zahlen und g\\u00e4ngige Sonderzeichen erlaubt)\"}");
                return;
            }
        }
        if (!storageStoreWifiPass(pw.c_str())) {
            portalLog(req, 500); req->send(500, "text/plain", "Speichern fehlgeschlagen");
            return;
        }
        strncpy(wifiPass, pw.c_str(), 63);
        wifiPass[63] = '\0';
        portalLog(req, 200);
        req->send(200, "application/json", "{\"ok\":true}");
        WiFi.softAP(wifiSSID, wifiPass);
        logPrintf("[WiFi] WLAN-Passwort geändert, AP neu gestartet\n");
    });
}
