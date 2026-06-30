#include "storage.h"
#include <Arduino.h>
#include "serial_log.h"
#include <nvs_flash.h>
#include <nvs.h>
#include <string.h>

#define NVS_NAMESPACE    "projektx"
#define NVS_KEY_NAME     "device_name"
#define NVS_KEY_PIN      "settings_pin"
#define NVS_KEY_WIFIPASS "wifi_pass"
#define NVS_KEY_PRESET   "lora_preset"
#define NVS_KEY_CHANNELS "channel_list"

#define NVS_MSG_PARTITION "msgstore"
#define NVS_MSG_NAMESPACE "msgs"
#define NVS_KEY_MSG_IDX   "msg_slot_idx"

// ----------------------------------------------------------------
// Internes Hilfsmittel
// ----------------------------------------------------------------

static bool nvsOpen(nvs_handle_t *handle, nvs_open_mode_t mode) {
    esp_err_t err = nvs_open(NVS_NAMESPACE, mode, handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        // Namespace existiert noch nicht — kein Fehler, nur leer
        return false;
    }
    if (err != ESP_OK) {
        logPrintf("[Storage] nvs_open fehlgeschlagen: %s\n", esp_err_to_name(err));
        return false;
    }
    return true;
}

// PSK-NVS-Schlüssel aus Kanalname bauen: "psk_<name>"
static void buildPskKey(const char *channelName, char *keyBuf, size_t keyBufLen) {
    snprintf(keyBuf, keyBufLen, "psk_%s", channelName);
    keyBuf[15] = '\0';
}

static void buildChannelNameKey(const char *channelName, char *keyBuf, size_t keyBufLen) {
    snprintf(keyBuf, keyBufLen, "name_%s", channelName);
    keyBuf[15] = '\0';
    if (strlen(channelName) > 10)
        logPrintf("[Storage] Warnung: Kanalname '%s' >10 Zeichen — NVS-Schlüssel abgeschnitten\n", channelName);
}

static void buildChannelLockKey(const char *channelName, char *keyBuf, size_t keyBufLen) {
    snprintf(keyBuf, keyBufLen, "lock_%s", channelName);
    keyBuf[15] = '\0';
}

// ----------------------------------------------------------------
// Kanal-Liste — kommagetrennt in einem NVS-String
// "Familie,Dorf,THW"
// ----------------------------------------------------------------

static bool loadChannelListRaw(char *buf, size_t bufLen) {
    nvs_handle_t handle;
    if (!nvsOpen(&handle, NVS_READONLY)) return false;
    size_t len = bufLen;
    esp_err_t err = nvs_get_str(handle, NVS_KEY_CHANNELS, buf, &len);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) { buf[0] = '\0'; return true; }
    return (err == ESP_OK);
}

static bool saveChannelListRaw(const char *buf) {
    nvs_handle_t handle;
    if (!nvsOpen(&handle, NVS_READWRITE)) return false;
    esp_err_t err = nvs_set_str(handle, NVS_KEY_CHANNELS, buf);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return (err == ESP_OK);
}

static bool channelExists(const char *listBuf, const char *name) {
    // Sucht exakten Eintrag in kommagetrennter Liste
    const char *p = listBuf;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        if (len == strlen(name) && strncmp(p, name, len) == 0) return true;
        p = comma ? comma + 1 : p + len;
    }
    return false;
}

static bool addChannelToList(const char *name) {
    char buf[MAX_CHANNELS * (CHANNEL_NAME_MAX + 1) + MAX_CHANNELS] = {0};
    if (!loadChannelListRaw(buf, sizeof(buf))) return false;
    if (channelExists(buf, name)) return true; // bereits drin
    if (strlen(buf) > 0) strncat(buf, ",", sizeof(buf) - strlen(buf) - 1);
    strncat(buf, name, sizeof(buf) - strlen(buf) - 1);
    return saveChannelListRaw(buf);
}

static bool removeChannelFromList(const char *name) {
    char buf[MAX_CHANNELS * (CHANNEL_NAME_MAX + 1) + MAX_CHANNELS] = {0};
    if (!loadChannelListRaw(buf, sizeof(buf))) return false;

    char result[sizeof(buf)] = {0};
    char *p = buf;
    bool first = true;
    while (*p) {
        char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        if (!(len == strlen(name) && strncmp(p, name, len) == 0)) {
            if (!first) strncat(result, ",", sizeof(result) - strlen(result) - 1);
            strncat(result, p, min(len, sizeof(result) - strlen(result) - 1));
            first = false;
        }
        p = comma ? comma + 1 : p + len;
        if (!comma) break;
    }
    return saveChannelListRaw(result);
}

// ----------------------------------------------------------------
// Kanalname-Validierung + Zählung
// ----------------------------------------------------------------

bool storageValidateChannelName(const char *name) {
    if (!name) return false;
    size_t len = strlen(name);
    // CHANNEL_NAME_KEYMAX = 10 — NVS-Schlüssel darf max. 15 Zeichen haben.
    // Längster Präfix: "name_" / "lock_" = 5 Zeichen → max. 10 für den Namen.
    // Harte Grenze: "psk_Kanalnamelangetest" würde auf 15 abgeschnitten und
    // kollidiert mit einem anderen Kanal der mit denselben 11 Zeichen beginnt.
    if (len == 0 || len > CHANNEL_NAME_KEYMAX) return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20) return false;  // Steuerzeichen
        // HTML/JS-Metazeichen verbieten (XSS-Schutz in onclick-Attributen)
        if (c=='<'||c=='>'||c=='"'||c=='\''||c=='&'||c=='\\'||c=='/') return false;
        // ASCII: nur alnum, _, -, Leerzeichen; Bytes >=0x80 (UTF-8 Umlaute) durchlassen
        if (c < 0x80) {
            bool ok = (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||
                      c=='_'||c=='-'||c==' ';
            if (!ok) return false;
        }
    }
    return true;
}

uint8_t storageChannelCount() {
    char buf[MAX_CHANNELS * (CHANNEL_NAME_MAX + 1) + MAX_CHANNELS] = {0};
    if (!loadChannelListRaw(buf, sizeof(buf))) return 0;
    if (buf[0] == '\0') return 0;
    uint8_t count = 1;
    for (const char *p = buf; *p; p++) if (*p == ',') count++;
    return count;
}

// ----------------------------------------------------------------
// PSK
// ----------------------------------------------------------------

bool storageStorePsk(const char *channelName, const uint8_t *psk) {
    // 8-Kanal-Limit VOR dem Schreiben prüfen (kein verwaister Blob)
    {
        char listBuf[MAX_CHANNELS * (CHANNEL_NAME_MAX + 1) + MAX_CHANNELS] = {0};
        loadChannelListRaw(listBuf, sizeof(listBuf));
        if (!channelExists(listBuf, channelName) && storageChannelCount() >= MAX_CHANNELS) {
            logPrintf("[Storage] Kanal-Limit (%d) erreicht — '%s' abgelehnt\n",
                MAX_CHANNELS, channelName);
            return false;
        }
    }

    char key[16];
    buildPskKey(channelName, key, sizeof(key));

    nvs_handle_t handle;
    if (!nvsOpen(&handle, NVS_READWRITE)) return false;
    esp_err_t err = nvs_set_blob(handle, key, psk, 32);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);

    if (err != ESP_OK) {
        logPrintf("[Storage] PSK schreiben fehlgeschlagen (%s): %s\n",
            channelName, esp_err_to_name(err));
        return false;
    }

    if (!addChannelToList(channelName)) {
        logPrintf("[Storage] Warnung: Kanalliste konnte nicht aktualisiert werden\n");
    }

    logPrintf("[Storage] PSK gespeichert: Kanal '%s'\n", channelName);
    return true;
}

bool storageLoadPsk(const char *channelName, uint8_t *psk) {
    char key[16];
    buildPskKey(channelName, key, sizeof(key));

    nvs_handle_t handle;
    if (!nvsOpen(&handle, NVS_READONLY)) return false;
    size_t len = 32;
    esp_err_t err = nvs_get_blob(handle, key, psk, &len);
    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        logPrintf("[Storage] PSK nicht gefunden: Kanal '%s'\n", channelName);
        return false;
    }
    if (err != ESP_OK || len != 32) {
        logPrintf("[Storage] PSK lesen fehlgeschlagen (%s): %s\n",
            channelName, esp_err_to_name(err));
        return false;
    }

    logPrintf("[Storage] PSK geladen: Kanal '%s'\n", channelName);
    return true;
}

bool storageDeletePsk(const char *channelName) {
    char key[16];
    buildPskKey(channelName, key, sizeof(key));

    nvs_handle_t handle;
    if (!nvsOpen(&handle, NVS_READWRITE)) return false;
    esp_err_t err = nvs_erase_key(handle, key);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);

    removeChannelFromList(channelName);

    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        logPrintf("[Storage] PSK löschen fehlgeschlagen (%s): %s\n",
            channelName, esp_err_to_name(err));
        return false;
    }

    logPrintf("[Storage] PSK gelöscht: Kanal '%s'\n", channelName);
    return true;
}

// ----------------------------------------------------------------
// Kanal-Absendername
// ----------------------------------------------------------------

bool storageStoreChannelSenderName(const char *channelName, const char *senderName) {
    char key[16];
    buildChannelNameKey(channelName, key, sizeof(key));
    nvs_handle_t handle;
    if (!nvsOpen(&handle, NVS_READWRITE)) return false;
    esp_err_t err = nvs_set_str(handle, key, senderName);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) return false;
    logPrintf("[Storage] Kanal-Absendername gespeichert: '%s' → '%s'\n", channelName, senderName);
    return true;
}

bool storageLoadChannelSenderName(const char *channelName, char *senderName, size_t maxLen) {
    char key[16];
    buildChannelNameKey(channelName, key, sizeof(key));
    nvs_handle_t handle;
    if (!nvsOpen(&handle, NVS_READONLY)) return false;
    esp_err_t err = nvs_get_str(handle, key, senderName, &maxLen);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return false;
    return err == ESP_OK;
}

bool storageDeleteChannelSenderName(const char *channelName) {
    char key[16];
    buildChannelNameKey(channelName, key, sizeof(key));
    nvs_handle_t handle;
    if (!nvsOpen(&handle, NVS_READWRITE)) return false;
    esp_err_t err = nvs_erase_key(handle, key);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND;
}

// ----------------------------------------------------------------
// Kanal-Namenssperre
// ----------------------------------------------------------------

bool storageStoreChannelNameLock(const char *channelName, uint8_t locked) {
    char key[16];
    buildChannelLockKey(channelName, key, sizeof(key));
    nvs_handle_t handle;
    if (!nvsOpen(&handle, NVS_READWRITE)) return false;
    esp_err_t err = nvs_set_u8(handle, key, locked);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}

bool storageLoadChannelNameLock(const char *channelName, uint8_t *locked) {
    char key[16];
    buildChannelLockKey(channelName, key, sizeof(key));
    nvs_handle_t handle;
    if (!nvsOpen(&handle, NVS_READONLY)) return false;
    esp_err_t err = nvs_get_u8(handle, key, locked);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) { *locked = 0; return false; }
    return err == ESP_OK;
}

bool storageDeleteChannelNameLock(const char *channelName) {
    char key[16];
    buildChannelLockKey(channelName, key, sizeof(key));
    nvs_handle_t handle;
    if (!nvsOpen(&handle, NVS_READWRITE)) return false;
    esp_err_t err = nvs_erase_key(handle, key);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND;
}

bool storageLoadChannelNames(char names[MAX_CHANNELS][CHANNEL_NAME_MAX + 1], uint8_t *count) {
    *count = 0;
    char buf[MAX_CHANNELS * (CHANNEL_NAME_MAX + 1) + MAX_CHANNELS] = {0};
    if (!loadChannelListRaw(buf, sizeof(buf))) return false;
    if (buf[0] == '\0') return true; // keine Kanäle — kein Fehler

    char *p = buf;
    while (*p && *count < MAX_CHANNELS) {
        char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        len = min(len, (size_t)CHANNEL_NAME_MAX);
        strncpy(names[*count], p, len);
        names[*count][len] = '\0';
        (*count)++;
        p = comma ? comma + 1 : p + len;
        if (!comma) break;
    }
    return true;
}

// ----------------------------------------------------------------
// Gerätename
// ----------------------------------------------------------------

bool storageStoreDeviceName(const char *name) {
    nvs_handle_t handle;
    if (!nvsOpen(&handle, NVS_READWRITE)) return false;
    esp_err_t err = nvs_set_str(handle, NVS_KEY_NAME, name);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        logPrintf("[Storage] Gerätename schreiben fehlgeschlagen: %s\n", esp_err_to_name(err));
        return false;
    }
    logPrintf("[Storage] Gerätename gespeichert: '%s'\n", name);
    return true;
}

bool storageLoadDeviceName(char *name, size_t maxLen) {
    nvs_handle_t handle;
    if (!nvsOpen(&handle, NVS_READONLY)) return false;
    esp_err_t err = nvs_get_str(handle, NVS_KEY_NAME, name, &maxLen);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return false;
    if (err != ESP_OK) {
        logPrintf("[Storage] Gerätename lesen fehlgeschlagen: %s\n", esp_err_to_name(err));
        return false;
    }
    logPrintf("[Storage] Gerätename geladen: '%s'\n", name);
    return true;
}

// ----------------------------------------------------------------
// Einstellungs-PIN
// ----------------------------------------------------------------

bool storageStoreSettingsPin(uint32_t pin) {
    nvs_handle_t handle;
    if (!nvsOpen(&handle, NVS_READWRITE)) return false;
    esp_err_t err = nvs_set_u32(handle, NVS_KEY_PIN, pin);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        logPrintf("[Storage] PIN schreiben fehlgeschlagen: %s\n", esp_err_to_name(err));
        return false;
    }
    logPrintf("[Storage] Einstellungs-PIN gespeichert\n");
    return true;
}

bool storageLoadSettingsPin(uint32_t *pin) {
    nvs_handle_t handle;
    if (!nvsOpen(&handle, NVS_READONLY)) return false;
    esp_err_t err = nvs_get_u32(handle, NVS_KEY_PIN, pin);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return false;
    if (err != ESP_OK) {
        logPrintf("[Storage] PIN lesen fehlgeschlagen: %s\n", esp_err_to_name(err));
        return false;
    }
    return true;
}

// ----------------------------------------------------------------
// WLAN-Passwort
// ----------------------------------------------------------------

bool storageStoreWifiPass(const char *pass) {
    nvs_handle_t handle;
    if (!nvsOpen(&handle, NVS_READWRITE)) return false;
    esp_err_t err = nvs_set_str(handle, NVS_KEY_WIFIPASS, pass);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        logPrintf("[Storage] WLAN-Passwort schreiben fehlgeschlagen: %s\n", esp_err_to_name(err));
        return false;
    }
    logPrintf("[Storage] WLAN-Passwort gespeichert\n");
    return true;
}

bool storageLoadWifiPass(char *pass, size_t maxLen) {
    nvs_handle_t handle;
    if (!nvsOpen(&handle, NVS_READONLY)) return false;
    esp_err_t err = nvs_get_str(handle, NVS_KEY_WIFIPASS, pass, &maxLen);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return false;
    if (err != ESP_OK) {
        logPrintf("[Storage] WLAN-Passwort lesen fehlgeschlagen: %s\n", esp_err_to_name(err));
        return false;
    }
    return true;
}

// ----------------------------------------------------------------
// LoRa-Preset
// ----------------------------------------------------------------

bool storageStorePreset(uint8_t preset) {
    nvs_handle_t handle;
    if (!nvsOpen(&handle, NVS_READWRITE)) return false;
    esp_err_t err = nvs_set_u8(handle, NVS_KEY_PRESET, preset);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        logPrintf("[Storage] Preset schreiben fehlgeschlagen: %s\n", esp_err_to_name(err));
        return false;
    }
    logPrintf("[Storage] Preset gespeichert: %d\n", preset);
    return true;
}

bool storageLoadPreset(uint8_t *preset) {
    nvs_handle_t handle;
    if (!nvsOpen(&handle, NVS_READONLY)) return false;
    esp_err_t err = nvs_get_u8(handle, NVS_KEY_PRESET, preset);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return false;
    if (err != ESP_OK) {
        logPrintf("[Storage] Preset lesen fehlgeschlagen: %s\n", esp_err_to_name(err));
        return false;
    }
    return true;
}

// ----------------------------------------------------------------
// Chatverlauf-Ringspeicher — 50 Slots in separater NVS-Partition "msgstore"
// ----------------------------------------------------------------

static bool msgNvsOpen(nvs_handle_t *handle, nvs_open_mode_t mode) {
    esp_err_t err = nvs_open_from_partition(NVS_MSG_PARTITION, NVS_MSG_NAMESPACE, mode, handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return false;
    if (err != ESP_OK) {
        logPrintf("[Storage] msgstore nvs_open fehlgeschlagen: %s\n", esp_err_to_name(err));
        return false;
    }
    return true;
}

void storageInitMsgPartition() {
    esp_err_t ret = nvs_flash_init_partition(NVS_MSG_PARTITION);
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        logPrintf("[Storage] msgstore beschädigt — wird neu initialisiert\n");
        nvs_flash_erase_partition(NVS_MSG_PARTITION);
        nvs_flash_init_partition(NVS_MSG_PARTITION);
    } else if (ret != ESP_OK) {
        logPrintf("[Storage] msgstore init fehlgeschlagen: %s\n", esp_err_to_name(ret));
    }
}

void storageWriteMessage(const char *sender, const char *text,
                         const char *channel, uint32_t timestamp, bool isOwn) {
    nvs_handle_t handle;
    if (!msgNvsOpen(&handle, NVS_READWRITE)) return;

    uint8_t slotIdx = 0;
    nvs_get_u8(handle, NVS_KEY_MSG_IDX, &slotIdx);
    if (slotIdx >= MSG_SLOTS) slotIdx = 0;

    PersistedMessage msg = {};
    strncpy(msg.sender, sender, 20);
    msg.sender[20] = '\0';
    strncpy(msg.text, text, 182);
    msg.text[182] = '\0';
    strncpy(msg.channel, channel, CHANNEL_NAME_MAX);
    msg.channel[CHANNEL_NAME_MAX] = '\0';
    msg.timestamp = timestamp;
    msg.isOwn = isOwn ? 1 : 0;

    char key[16];
    snprintf(key, sizeof(key), "msg_slot_%d", slotIdx);

    esp_err_t err = nvs_set_blob(handle, key, &msg, sizeof(msg));
    if (err == ESP_OK) {
        uint8_t nextIdx = (slotIdx + 1) % MSG_SLOTS;
        nvs_set_u8(handle, NVS_KEY_MSG_IDX, nextIdx);
        nvs_commit(handle);
        logPrintf("[Storage] Nachricht in %s persistiert\n", key);
    } else {
        logPrintf("[Storage] Nachricht schreiben fehlgeschlagen: %s\n", esp_err_to_name(err));
    }

    nvs_close(handle);
}

int storageLoadAllMessages(PersistedMessage *msgs, int maxCount) {
    nvs_handle_t handle;
    if (!msgNvsOpen(&handle, NVS_READONLY)) return 0;

    int validCount = 0;
    for (int i = 0; i < MSG_SLOTS && validCount < maxCount; i++) {
        char key[16];
        snprintf(key, sizeof(key), "msg_slot_%d", i);

        PersistedMessage temp;
        size_t len = sizeof(temp);
        esp_err_t err = nvs_get_blob(handle, key, &temp, &len);
        if (err == ESP_OK && len == sizeof(temp)) {
            msgs[validCount] = temp;
            validCount++;
        }
    }
    nvs_close(handle);

    for (int i = 1; i < validCount; i++) {
        PersistedMessage tmp = msgs[i];
        int j = i - 1;
        while (j >= 0) {
            bool swap = false;
            if (tmp.timestamp == 0) {
                swap = false;
            } else if (msgs[j].timestamp == 0) {
                swap = true;
            } else if (msgs[j].timestamp > tmp.timestamp) {
                swap = true;
            }
            if (!swap) break;
            msgs[j + 1] = msgs[j];
            j--;
        }
        msgs[j + 1] = tmp;
    }

    return validCount;
}

void storageLogNvsStats() {
    nvs_stats_t stats;
    esp_err_t err = nvs_get_stats("nvs", &stats);
    if (err == ESP_OK) {
        logPrintf("[Storage] NVS (config): used=%d free=%d total=%d\n",
            stats.used_entries, stats.free_entries, stats.total_entries);
    }
    err = nvs_get_stats(NVS_MSG_PARTITION, &stats);
    if (err == ESP_OK) {
        logPrintf("[Storage] NVS (msgstore): used=%d free=%d total=%d\n",
            stats.used_entries, stats.free_entries, stats.total_entries);
    }
}

// ----------------------------------------------------------------
// Sequenznummer-Persistenz — 16 Slots in separatem Namespace "seqnums"
// innerhalb der msgstore-Partition
// ----------------------------------------------------------------

#define NVS_SEQ_NAMESPACE "seqnums"
#define NVS_KEY_SEQ_IDX   "seq_slot_idx"

static bool seqNvsOpen(nvs_handle_t *handle, nvs_open_mode_t mode) {
    esp_err_t err = nvs_open_from_partition(NVS_MSG_PARTITION, NVS_SEQ_NAMESPACE, mode, handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return false;
    if (err != ESP_OK) {
        logPrintf("[Storage] seqnums nvs_open fehlgeschlagen: %s\n", esp_err_to_name(err));
        return false;
    }
    return true;
}

void storageWriteSeqNum(uint16_t seqNum) {
    nvs_handle_t handle;
    if (!seqNvsOpen(&handle, NVS_READWRITE)) return;

    uint8_t slotIdx = 0;
    nvs_get_u8(handle, NVS_KEY_SEQ_IDX, &slotIdx);
    if (slotIdx >= SEQ_SLOTS) slotIdx = 0;

    char key[16];
    snprintf(key, sizeof(key), "seq_slot_%d", slotIdx);

    esp_err_t err = nvs_set_u16(handle, key, seqNum);
    if (err == ESP_OK) {
        uint8_t nextIdx = (slotIdx + 1) % SEQ_SLOTS;
        nvs_set_u8(handle, NVS_KEY_SEQ_IDX, nextIdx);
        nvs_commit(handle);
    }

    nvs_close(handle);
}

bool storageLoadSeqNum(uint16_t *seqNum) {
    nvs_handle_t handle;
    if (!seqNvsOpen(&handle, NVS_READONLY)) return false;

    uint16_t values[SEQ_SLOTS];
    bool valid[SEQ_SLOTS];
    int validCount = 0;

    for (int i = 0; i < SEQ_SLOTS; i++) {
        char key[16];
        snprintf(key, sizeof(key), "seq_slot_%d", i);
        esp_err_t err = nvs_get_u16(handle, key, &values[i]);
        valid[i] = (err == ESP_OK);
        if (valid[i]) validCount++;
    }

    nvs_close(handle);

    if (validCount == 0) return false;

    uint16_t best = 0;
    bool bestSet = false;
    for (int i = 0; i < SEQ_SLOTS; i++) {
        if (!valid[i]) continue;
        if (!bestSet) {
            best = values[i];
            bestSet = true;
        } else {
            uint16_t diff = (uint16_t)(values[i] - best);
            if (diff > 0 && diff < 32768) {
                best = values[i];
            }
        }
    }

    *seqNum = (uint16_t)(best + 1);
    logPrintf("[Storage] SeqNum wiederhergestellt: höchster Slot=%u, Start=%u\n", best, *seqNum);
    return true;
}

void storageClearSeqSlots() {
    nvs_handle_t handle;
    if (!seqNvsOpen(&handle, NVS_READWRITE)) return;
    for (int i = 0; i < SEQ_SLOTS; i++) {
        char key[16];
        snprintf(key, sizeof(key), "seq_slot_%d", i);
        nvs_erase_key(handle, key);
    }
    nvs_set_u8(handle, NVS_KEY_SEQ_IDX, 0);
    nvs_commit(handle);
    nvs_close(handle);
}

// ----------------------------------------------------------------
// Werksreset-Hilfsfunktionen
// ----------------------------------------------------------------

void storageClearAllChannels() {
    char names[MAX_CHANNELS][CHANNEL_NAME_MAX + 1];
    uint8_t count = 0;
    storageLoadChannelNames(names, &count);

    for (int i = 0; i < count; i++) {
        storageDeletePsk(names[i]);
        storageDeleteChannelSenderName(names[i]);
        storageDeleteChannelNameLock(names[i]);
    }
    saveChannelListRaw("");
    logPrintf("[Storage] Alle Kanäle gelöscht (%d)\n", count);
}

void storageClearMessages() {
    nvs_handle_t handle;
    if (!msgNvsOpen(&handle, NVS_READWRITE)) return;
    for (int i = 0; i < MSG_SLOTS; i++) {
        char key[16];
        snprintf(key, sizeof(key), "msg_slot_%d", i);
        nvs_erase_key(handle, key);
    }
    nvs_set_u8(handle, NVS_KEY_MSG_IDX, 0);
    nvs_commit(handle);
    nvs_close(handle);
    logPrintf("[Storage] Chatverlauf gelöscht (%d Slots)\n", MSG_SLOTS);
}

bool storageDeleteDeviceName() {
    nvs_handle_t handle;
    if (!nvsOpen(&handle, NVS_READWRITE)) return false;
    esp_err_t err = nvs_erase_key(handle, NVS_KEY_NAME);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    logPrintf("[Storage] Gerätename gelöscht\n");
    return err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND;
}