#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// ----------------------------------------------------------------
// PSK — kanalbasiert
// Kanalname max. 20 Zeichen. Max. 8 Kanäle.
// NVS-Schlüssel: "psk_<kanalname>"
// ----------------------------------------------------------------
bool storageStorePsk(const char *channelName, const uint8_t *psk);
bool storageLoadPsk(const char *channelName, uint8_t *psk);
bool storageDeletePsk(const char *channelName);

// Alle gespeicherten Kanalnamen laden — names: Array von MAX_CHANNELS Strings
#define MAX_CHANNELS 8
#define CHANNEL_NAME_MAX 20
// Maximale Kanalnamen-Länge für NVS-Schlüssel: "psk_"/"name_"/"lock_" (5) + Name ≤ 15
#define CHANNEL_NAME_KEYMAX 10
bool storageLoadChannelNames(char names[MAX_CHANNELS][CHANNEL_NAME_MAX + 1], uint8_t *count);

// Kanalname validieren: 1–10 Byte, keine HTML/JS-Metazeichen, keine Steuerzeichen.
// Verhindert NVS-Key-Kollision (>10) und XSS über onclick-Attribute.
bool storageValidateChannelName(const char *name);

// Anzahl aktuell gespeicherter Kanäle (für 8-Kanal-Limit)
uint8_t storageChannelCount();

// ----------------------------------------------------------------
// Kanal-Absendername — max. 20 Zeichen, NVS-Schlüssel: "name_<kanalname>"
// ----------------------------------------------------------------
bool storageStoreChannelSenderName(const char *channelName, const char *senderName);
bool storageLoadChannelSenderName(const char *channelName, char *senderName, size_t maxLen);
bool storageDeleteChannelSenderName(const char *channelName);

// ----------------------------------------------------------------
// Kanal-Namenssperre — 0=editierbar, 1=gesperrt, NVS-Schlüssel: "lock_<kanalname>"
// ----------------------------------------------------------------
bool storageStoreChannelNameLock(const char *channelName, uint8_t locked);
bool storageLoadChannelNameLock(const char *channelName, uint8_t *locked);
bool storageDeleteChannelNameLock(const char *channelName);

// ----------------------------------------------------------------
// Gerätename — max. 20 Zeichen
// ----------------------------------------------------------------
bool storageStoreDeviceName(const char *name);
bool storageLoadDeviceName(char *name, size_t maxLen);

// ----------------------------------------------------------------
// Einstellungs-PIN — 6-stellig, uint32_t (000000–999999)
// ----------------------------------------------------------------
bool storageStoreSettingsPin(uint32_t pin);
bool storageLoadSettingsPin(uint32_t *pin);

// ----------------------------------------------------------------
// WLAN-Passwort — max. 63 Zeichen (WPA2-Limit), Buffer in main.cpp = 64 Bytes
// ----------------------------------------------------------------
bool storageStoreWifiPass(const char *pass);
bool storageLoadWifiPass(char *pass, size_t maxLen);

// ----------------------------------------------------------------
// LoRa-Preset — uint8_t (0=Standard, 1=Reichweite, 2=Organisation, 3=Stadt)
// ----------------------------------------------------------------
bool storageStorePreset(uint8_t preset);
bool storageLoadPreset(uint8_t *preset);

// ----------------------------------------------------------------
// Chatverlauf-Ringspeicher — 50 Slots in NVS
// ----------------------------------------------------------------
#define MSG_SLOTS 50

struct __attribute__((packed)) PersistedMessage {
    char     sender[21];
    char     text[183];
    char     channel[CHANNEL_NAME_MAX + 1];
    uint32_t timestamp;
    uint8_t  isOwn;
};

void storageInitMsgPartition();
void storageWriteMessage(const char *sender, const char *text,
                         const char *channel, uint32_t timestamp, bool isOwn);
int  storageLoadAllMessages(PersistedMessage *msgs, int maxCount);
void storageLogNvsStats();

// ----------------------------------------------------------------
// Sequenznummer-Persistenz — 16 Slots in msgstore-Partition
// ----------------------------------------------------------------
#define SEQ_SLOTS 16

void storageWriteSeqNum(uint16_t seqNum);
bool storageLoadSeqNum(uint16_t *seqNum);
void storageClearSeqSlots();

// ----------------------------------------------------------------
// Werksreset-Hilfsfunktionen
// ----------------------------------------------------------------
void storageClearAllChannels();
void storageClearMessages();
bool storageDeleteDeviceName();
