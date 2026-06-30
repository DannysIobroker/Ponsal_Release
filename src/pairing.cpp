#include "pairing.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <ArduinoJson.h>
#include "mbedtls/ecp.h"
#include "mbedtls/ecdh.h"
#include "mbedtls/sha256.h"
#include "mbedtls/platform_util.h"
#include "crypto.h"
#include "storage.h"
#include "serial_log.h"
#include "display_hal.h"
#include "mesh.h"
#include "lora_hal.h"

static void portalLog(AsyncWebServerRequest *req, int code) {
    logPrintf("[Portal] %s %s → %d\n", req->methodToString(), req->url().c_str(), code);
}

extern ChannelPsk channelPsks[];
extern uint8_t channelPskCount;
extern uint32_t settingsPin;
extern char deviceName[21];
extern void reloadChannelPsks();
extern bool enqueueSend(const uint8_t *data, size_t len, QueueEntryType type);

// ── Interner Zustand ────────────────────────────────────────────
struct PairingCtx {
    PairingState state;
    PairingPendingAction pendingAction;
    uint32_t sessionId;
    unsigned long startedAt;
    unsigned long lastTxAt;
    unsigned long nameWaitStartedAt;

    mbedtls_ecp_group grp;
    mbedtls_mpi privKey;
    mbedtls_ecp_point pubKey;
    uint8_t pubKeyBytes[32];
    uint8_t sharedSecret[32];
    bool ecdhInitialized;

    char channel[CHANNEL_NAME_MAX + 1];
    uint8_t psk[32];
    char pin[7];

    uint8_t lastPacket[100];
    size_t lastPacketLen;

    char error[128];
    char pendingChannel[CHANNEL_NAME_MAX + 1];
    char pendingName[21];
    char peerName[21];
    uint8_t pendingLockName;
};

static PairingCtx ctx;
static SemaphoreHandle_t pairingMutex;

// ── ECDH-Hilfsfunktionen ───────────────────────────────────────

static void ecdhInit() {
    mbedtls_ecp_group_init(&ctx.grp);
    mbedtls_mpi_init(&ctx.privKey);
    mbedtls_ecp_point_init(&ctx.pubKey);
    ctx.ecdhInitialized = true;
}

static void ecdhCleanup() {
    if (ctx.ecdhInitialized) {
        mbedtls_ecp_group_free(&ctx.grp);
        mbedtls_mpi_free(&ctx.privKey);
        mbedtls_ecp_point_free(&ctx.pubKey);
        ctx.ecdhInitialized = false;
    }
    memset(ctx.sharedSecret, 0, 32);
    memset(ctx.psk, 0, 32);
}

static bool ecdhGenerateKeypair() {
    ecdhInit();
    unsigned long t0 = millis();

    int ret = mbedtls_ecp_group_load(&ctx.grp, MBEDTLS_ECP_DP_CURVE25519);
    if (ret != 0) {
        logPrintf("[Pairing] ecp_group_load fehlgeschlagen: -0x%04X\n", -ret);
        return false;
    }

    ret = mbedtls_ecdh_gen_public(&ctx.grp, &ctx.privKey, &ctx.pubKey,
                                   espRngCallback, NULL);
    if (ret != 0) {
        logPrintf("[Pairing] ecdh_gen_public fehlgeschlagen: -0x%04X\n", -ret);
        return false;
    }

    size_t olen = 0;
    ret = mbedtls_ecp_point_write_binary(&ctx.grp, &ctx.pubKey,
              MBEDTLS_ECP_PF_COMPRESSED, &olen, ctx.pubKeyBytes, 32);
    if (ret != 0) {
        logPrintf("[Pairing] point_write_binary fehlgeschlagen: -0x%04X (olen=%d)\n", -ret, (int)olen);
        return false;
    }

    logPrintf("[Pairing] ECDH Keygen: %lums, pubkey %d Byte\n",
        millis() - t0, (int)olen);
    return true;
}

static bool ecdhComputeShared(const uint8_t *peerPubKeyBytes, size_t peerLen) {
    unsigned long t0 = millis();

    mbedtls_ecp_point peerQ;
    mbedtls_ecp_point_init(&peerQ);
    mbedtls_mpi z;
    mbedtls_mpi_init(&z);

    int ret = mbedtls_ecp_point_read_binary(&ctx.grp, &peerQ, peerPubKeyBytes, peerLen);
    if (ret != 0) {
        logPrintf("[Pairing] point_read_binary fehlgeschlagen: -0x%04X\n", -ret);
        mbedtls_ecp_point_free(&peerQ);
        mbedtls_mpi_free(&z);
        return false;
    }

    ret = mbedtls_ecdh_compute_shared(&ctx.grp, &z, &peerQ, &ctx.privKey,
                                       espRngCallback, NULL);
    if (ret != 0) {
        logPrintf("[Pairing] compute_shared fehlgeschlagen: -0x%04X\n", -ret);
        mbedtls_ecp_point_free(&peerQ);
        mbedtls_mpi_free(&z);
        return false;
    }

    uint8_t rawSecret[32];
    ret = mbedtls_mpi_write_binary(&z, rawSecret, 32);
    mbedtls_ecp_point_free(&peerQ);
    mbedtls_mpi_free(&z);
    if (ret != 0) {
        logPrintf("[Pairing] mpi_write_binary fehlgeschlagen: -0x%04X\n", -ret);
        return false;
    }

    mbedtls_sha256(rawSecret, 32, ctx.sharedSecret, 0);
    mbedtls_platform_zeroize(rawSecret, 32);

    uint32_t pinVal = ((uint32_t)ctx.sharedSecret[0] << 24) |
                      ((uint32_t)ctx.sharedSecret[1] << 16) |
                      ((uint32_t)ctx.sharedSecret[2] << 8)  |
                       (uint32_t)ctx.sharedSecret[3];
    pinVal = pinVal % 1000000;
    snprintf(ctx.pin, sizeof(ctx.pin), "%06lu", (unsigned long)pinVal);

    logPrintf("[Pairing] Shared Secret berechnet: %lums, PIN=%s\n",
        millis() - t0, ctx.pin);
    return true;
}

// ── Paketbau ────────────────────────────────────────────────────

static size_t buildHeader(uint8_t type, uint8_t *out) {
    out[0] = PAIRING_MAGIC;
    out[1] = type;
    out[2] = (ctx.sessionId >> 24) & 0xFF;
    out[3] = (ctx.sessionId >> 16) & 0xFF;
    out[4] = (ctx.sessionId >>  8) & 0xFF;
    out[5] =  ctx.sessionId        & 0xFF;
    return PAIRING_HEADER_LEN;
}

static bool parseHeader(const uint8_t *buf, int len, uint8_t *type, uint32_t *sessionId) {
    if (len < PAIRING_HEADER_LEN || buf[0] != PAIRING_MAGIC) return false;
    *type = buf[1];
    *sessionId = ((uint32_t)buf[2] << 24) | ((uint32_t)buf[3] << 16) |
                 ((uint32_t)buf[4] <<  8) |  (uint32_t)buf[5];
    return true;
}

static void sendAndStore(const uint8_t *pkt, size_t len) {
    memcpy(ctx.lastPacket, pkt, len);
    ctx.lastPacketLen = len;
    enqueueSend(pkt, len, QUEUE_PAIRING);
    ctx.lastTxAt = millis();
}

static void retransmit() {
    if (ctx.lastPacketLen > 0) {
        enqueueSend(ctx.lastPacket, ctx.lastPacketLen, QUEUE_PAIRING);
        ctx.lastTxAt = millis();
        logPrintf("[Pairing] Retransmit %d Byte\n", (int)ctx.lastPacketLen);
    }
}

// ── State Machine: Aktionen ─────────────────────────────────────

static void resetState() {
    ecdhCleanup();
    PairingPendingAction savedAction = ctx.pendingAction;
    char savedChannel[CHANNEL_NAME_MAX + 1];
    char savedName[21];
    uint8_t savedLockName = ctx.pendingLockName;
    strncpy(savedChannel, ctx.pendingChannel, CHANNEL_NAME_MAX);
    savedChannel[CHANNEL_NAME_MAX] = '\0';
    strncpy(savedName, ctx.pendingName, 20);
    savedName[20] = '\0';
    memset(&ctx, 0, sizeof(ctx));
    ctx.state = PAIR_IDLE;
    ctx.pendingAction = savedAction;
    ctx.pendingLockName = savedLockName;
    strncpy(ctx.pendingChannel, savedChannel, CHANNEL_NAME_MAX);
    strncpy(ctx.pendingName, savedName, 20);
}

static void doStartGive() {
    if (ctx.state != PAIR_IDLE && ctx.state != PAIR_GIVER_DONE &&
        ctx.state != PAIR_GIVER_WAIT_NAME_ACK && ctx.state != PAIR_RECEIVER_DONE &&
        ctx.state != PAIR_ERROR) {
        logPrintf("[Pairing] StartGive ignoriert — Pairing läuft\n");
        return;
    }
    if (ctx.state != PAIR_IDLE) resetState();

    int chIdx = -1;
    for (int i = 0; i < channelPskCount; i++) {
        if (strcmp(channelPsks[i].name, ctx.pendingChannel) == 0) {
            chIdx = i;
            break;
        }
    }
    if (chIdx < 0) {
        snprintf(ctx.error, sizeof(ctx.error), "Kanal '%s' nicht gefunden", ctx.pendingChannel);
        ctx.state = PAIR_ERROR;
        logPrintf("[Pairing] %s\n", ctx.error);
        return;
    }

    strncpy(ctx.channel, channelPsks[chIdx].name, CHANNEL_NAME_MAX);
    ctx.channel[CHANNEL_NAME_MAX] = '\0';
    memcpy(ctx.psk, channelPsks[chIdx].psk, 32);

    ctx.sessionId = esp_random();
    if (!ecdhGenerateKeypair()) {
        snprintf(ctx.error, sizeof(ctx.error), "ECDH Keygen fehlgeschlagen");
        ctx.state = PAIR_ERROR;
        return;
    }

    uint8_t pkt[PAIRING_HEADER_LEN + PAIRING_PUBKEY_LEN];
    size_t hlen = buildHeader(PAIRING_TYPE_ECDH_INIT, pkt);
    memcpy(pkt + hlen, ctx.pubKeyBytes, PAIRING_PUBKEY_LEN);

    sendAndStore(pkt, hlen + PAIRING_PUBKEY_LEN);

    ctx.state = PAIR_GIVER_WAIT_RESP;
    ctx.startedAt = millis();
    displayStatus("Pairing", "Sende...", "Warte auf Antwort");
    logPrintf("[Pairing] Geber gestartet, Kanal '%s', Session %08lX\n",
        ctx.channel, ctx.sessionId);
}

static void doStartReceive() {
    if (ctx.state != PAIR_IDLE && ctx.state != PAIR_GIVER_DONE &&
        ctx.state != PAIR_GIVER_WAIT_NAME_ACK && ctx.state != PAIR_RECEIVER_DONE &&
        ctx.state != PAIR_ERROR) {
        logPrintf("[Pairing] StartReceive ignoriert — Pairing läuft\n");
        return;
    }
    if (ctx.state != PAIR_IDLE) resetState();
    ctx.state = PAIR_RECEIVER_WAIT_INIT;
    ctx.startedAt = millis();
    displayStatus("Pairing", "Warte auf", "Sender...");
    logPrintf("[Pairing] Empfänger gestartet, warte auf ECDH_INIT\n");
}

static void doConfirm() {
    if (ctx.state != PAIR_GIVER_PIN_CONFIRM) {
        logPrintf("[Pairing] Confirm ignoriert — nicht PIN_CONFIRM\n");
        return;
    }

    uint8_t plaintext[CHANNEL_NAME_MAX + 1 + 32];
    memset(plaintext, 0, sizeof(plaintext));
    strncpy((char*)plaintext, ctx.channel, CHANNEL_NAME_MAX);
    memcpy(plaintext + CHANNEL_NAME_MAX + 1, ctx.psk, 32);
    size_t ptLen = CHANNEL_NAME_MAX + 1 + 32;

    uint8_t iv[12];
    generateIV(iv);
    uint8_t ciphertext[ptLen];
    uint8_t tag[16];

    uint8_t aad[4];
    aad[0] = (ctx.sessionId >> 24) & 0xFF;
    aad[1] = (ctx.sessionId >> 16) & 0xFF;
    aad[2] = (ctx.sessionId >>  8) & 0xFF;
    aad[3] =  ctx.sessionId        & 0xFF;

    if (!encryptPayload(ctx.sharedSecret, iv, aad, 4, plaintext, ptLen, ciphertext, tag)) {
        snprintf(ctx.error, sizeof(ctx.error), "PSK-Verschlüsselung fehlgeschlagen");
        ctx.state = PAIR_ERROR;
        return;
    }

    uint8_t pkt[PAIRING_HEADER_LEN + 12 + ptLen + 16];
    size_t hlen = buildHeader(PAIRING_TYPE_PSK_TRANSFER, pkt);
    memcpy(pkt + hlen, iv, 12);
    memcpy(pkt + hlen + 12, ciphertext, ptLen);
    memcpy(pkt + hlen + 12 + ptLen, tag, 16);

    sendAndStore(pkt, hlen + 12 + ptLen + 16);

    ctx.state = PAIR_GIVER_WAIT_ACK;
    displayStatus("Pairing", "PSK gesendet", "Warte auf Empf.");
    logPrintf("[Pairing] PSK_TRANSFER gesendet, %d Byte\n", (int)(hlen + 12 + ptLen + 16));
}

static void doSetName() {
    if (ctx.state != PAIR_GIVER_NAME_INPUT) {
        logPrintf("[Pairing] SetName ignoriert — nicht NAME_INPUT\n");
        return;
    }

    uint8_t pkt[PAIRING_HEADER_LEN + 21];
    size_t hlen = buildHeader(PAIRING_TYPE_NAME_TRANSFER, pkt);
    memset(pkt + hlen, 0, 21);
    strncpy((char*)(pkt + hlen), ctx.pendingName, 20);
    pkt[hlen + 20] = ctx.pendingLockName;

    // NAME_TRANSFER über enqueueSend(QUEUE_PAIRING) — wartet automatisch bis
    // die DC-Pause nach PSK_TRANSFER abgelaufen ist, ohne zu blockieren.
    sendAndStore(pkt, hlen + 21);

    ctx.state = PAIR_GIVER_WAIT_NAME_ACK;
    ctx.nameWaitStartedAt = millis();
    displayStatus("Pairing", "Name gesendet", "Warte auf Best.");
    logPrintf("[Pairing] NAME_TRANSFER gesendet: '%s', warte auf NAME_ACK\n", ctx.pendingName);
}

static void doRenameChannel() {
    if (ctx.state != PAIR_RECEIVER_CHANNEL_RENAME) {
        logPrintf("[Pairing] RenameChannel ignoriert — nicht CHANNEL_RENAME\n");
        return;
    }

    char newName[CHANNEL_NAME_MAX + 1];
    strncpy(newName, ctx.pendingChannel, CHANNEL_NAME_MAX);
    newName[CHANNEL_NAME_MAX] = '\0';

    // Nutzereingabe nur übernehmen wenn gültig; sonst sicheren Namen erzeugen
    if (!storageValidateChannelName(newName)) {
        // Basis aus Originalnamen (≤7, damit "_NN" passt), sonst "Kanal"
        char base[CHANNEL_NAME_MAX + 1];
        strncpy(base, ctx.channel, 7);
        base[7] = '\0';
        if (!storageValidateChannelName(base)) strcpy(base, "Kanal");
        for (int suffix = 2; suffix <= 99; suffix++) {
            snprintf(newName, sizeof(newName), "%s_%d", base, suffix);
            bool exists = false;
            for (int i = 0; i < channelPskCount; i++) {
                if (strcmp(channelPsks[i].name, newName) == 0) { exists = true; break; }
            }
            if (!exists) break;
        }
    }

    if (!storageStorePsk(newName, ctx.psk)) {
        snprintf(ctx.error, sizeof(ctx.error), "PSK speichern fehlgeschlagen");
        ctx.state = PAIR_ERROR;
        return;
    }

    strncpy(ctx.channel, newName, CHANNEL_NAME_MAX);
    ctx.channel[CHANNEL_NAME_MAX] = '\0';
    reloadChannelPsks();

    uint8_t pkt[PAIRING_HEADER_LEN + 1];
    size_t hlen = buildHeader(PAIRING_TYPE_PSK_ACK, pkt);
    pkt[hlen] = 0x00;
    sendAndStore(pkt, hlen + 1);

    ctx.state = PAIR_RECEIVER_WAIT_NAME;
    ctx.nameWaitStartedAt = millis();
    displayStatus("Pairing OK", newName, "Warte auf Name");
    logPrintf("[Pairing] Kanal umbenannt: '%s', PSK gespeichert, PSK_ACK gesendet\n", newName);
}

static void doAbort() {
    logPrintf("[Pairing] Abbruch durch Nutzer\n");
    ecdhCleanup();
    memset(&ctx, 0, sizeof(ctx));
    ctx.state = PAIR_IDLE;
    displayStatus("ProjektX", deviceName, "");
}

// ── Paketempfang ────────────────────────────────────────────────

static void handleEcdhResp(const uint8_t *payload, int payloadLen) {
    if (ctx.state != PAIR_GIVER_WAIT_RESP) return;
    if (payloadLen < PAIRING_PUBKEY_LEN) return;

    if (!ecdhComputeShared(payload, PAIRING_PUBKEY_LEN)) {
        snprintf(ctx.error, sizeof(ctx.error), "ECDH Berechnung fehlgeschlagen");
        ctx.state = PAIR_ERROR;
        return;
    }

    ctx.state = PAIR_GIVER_PIN_CONFIRM;
    displayStatus("Pairing PIN", ctx.pin, "Im Browser best.");
    logPrintf("[Pairing] ECDH_RESP empfangen, PIN=%s\n", ctx.pin);
}

static void handleEcdhInit(const uint8_t *buf, int len, uint32_t sessionId) {
    if (ctx.state != PAIR_RECEIVER_WAIT_INIT) return;
    const uint8_t *payload = buf + PAIRING_HEADER_LEN;
    int payloadLen = len - PAIRING_HEADER_LEN;
    if (payloadLen < PAIRING_PUBKEY_LEN) return;

    ctx.sessionId = sessionId;

    if (!ecdhGenerateKeypair()) {
        snprintf(ctx.error, sizeof(ctx.error), "ECDH Keygen fehlgeschlagen");
        ctx.state = PAIR_ERROR;
        return;
    }

    if (!ecdhComputeShared(payload, PAIRING_PUBKEY_LEN)) {
        snprintf(ctx.error, sizeof(ctx.error), "ECDH Berechnung fehlgeschlagen");
        ctx.state = PAIR_ERROR;
        return;
    }

    uint8_t pkt[PAIRING_HEADER_LEN + PAIRING_PUBKEY_LEN];
    size_t hlen = buildHeader(PAIRING_TYPE_ECDH_RESP, pkt);
    memcpy(pkt + hlen, ctx.pubKeyBytes, PAIRING_PUBKEY_LEN);
    sendAndStore(pkt, hlen + PAIRING_PUBKEY_LEN);

    ctx.state = PAIR_RECEIVER_PIN_DISPLAY;
    displayStatus("Pairing PIN", ctx.pin, "Geber bestaetigt");
    logPrintf("[Pairing] ECDH_INIT empfangen, Session %08lX, PIN=%s\n",
        ctx.sessionId, ctx.pin);
}

static void handlePskTransfer(const uint8_t *buf, int len) {
    if (ctx.state == PAIR_RECEIVER_WAIT_NAME || ctx.state == PAIR_RECEIVER_DONE) {
        uint8_t pkt[PAIRING_HEADER_LEN + 1];
        size_t hlen = buildHeader(PAIRING_TYPE_PSK_ACK, pkt);
        pkt[hlen] = 0x00;
        enqueueSend(pkt, hlen + 1, QUEUE_PAIRING);
        logPrintf("[Pairing] PSK_TRANSFER wiederholt — PSK_ACK erneut gesendet\n");
        return;
    }
    if (ctx.state != PAIR_RECEIVER_PIN_DISPLAY) return;

    const uint8_t *payload = buf + PAIRING_HEADER_LEN;
    int payloadLen = len - PAIRING_HEADER_LEN;
    size_t ptLen = CHANNEL_NAME_MAX + 1 + 32;
    size_t expectedLen = 12 + ptLen + 16;
    if ((size_t)payloadLen < expectedLen) {
        logPrintf("[Pairing] PSK_TRANSFER zu kurz: %d < %d\n", payloadLen, (int)expectedLen);
        return;
    }

    const uint8_t *iv = payload;
    const uint8_t *ciphertext = payload + 12;
    const uint8_t *tag = payload + 12 + ptLen;

    uint8_t aad[4];
    aad[0] = (ctx.sessionId >> 24) & 0xFF;
    aad[1] = (ctx.sessionId >> 16) & 0xFF;
    aad[2] = (ctx.sessionId >>  8) & 0xFF;
    aad[3] =  ctx.sessionId        & 0xFF;

    uint8_t plaintext[ptLen];
    if (!decryptPayload(ctx.sharedSecret, iv, aad, 4, ciphertext, ptLen, tag, plaintext)) {
        logPrintf("[Pairing] PSK-Entschlüsselung fehlgeschlagen (falsche PIN/Session?)\n");
        snprintf(ctx.error, sizeof(ctx.error), "PSK-Entschlüsselung fehlgeschlagen");
        ctx.state = PAIR_ERROR;
        return;
    }

    char channelName[CHANNEL_NAME_MAX + 1] = {0};
    memcpy(channelName, plaintext, CHANNEL_NAME_MAX);
    channelName[CHANNEL_NAME_MAX] = '\0';
    memcpy(ctx.psk, plaintext + CHANNEL_NAME_MAX + 1, 32);
    strncpy(ctx.channel, channelName, CHANNEL_NAME_MAX);
    ctx.channel[CHANNEL_NAME_MAX] = '\0';

    bool nameExists = false;
    for (int i = 0; i < channelPskCount; i++) {
        if (strcmp(channelPsks[i].name, channelName) == 0) {
            nameExists = true;
            break;
        }
    }

    // Duplikat ODER ungültiger Name (zu lang / Sonderzeichen) → Umbenennung im Browser.
    // PSK_ACK wird hier bewusst NICHT gesendet: der Geber wartet weiter und
    // retransmittiert PSK_TRANSFER. Erst doRenameChannel() sendet PSK_ACK,
    // nachdem der Nutzer einen gültigen Namen bestätigt hat.
    if (nameExists || !storageValidateChannelName(channelName)) {
        ctx.state = PAIR_RECEIVER_CHANNEL_RENAME;
        displayStatus("Pairing", "Kanal benennen", "Browser oeffnen");
        logPrintf("[Pairing] Kanalname '%s' belegt/ungültig — Umbenennung nötig\n", channelName);
        return;
    }

    if (!storageStorePsk(channelName, ctx.psk)) {
        snprintf(ctx.error, sizeof(ctx.error), "PSK speichern fehlgeschlagen");
        ctx.state = PAIR_ERROR;
        return;
    }
    reloadChannelPsks();

    uint8_t pkt[PAIRING_HEADER_LEN + 1];
    size_t hlen = buildHeader(PAIRING_TYPE_PSK_ACK, pkt);
    pkt[hlen] = 0x00;
    sendAndStore(pkt, hlen + 1);

    ctx.state = PAIR_RECEIVER_WAIT_NAME;
    ctx.nameWaitStartedAt = millis();
    displayStatus("Pairing OK", channelName, "Warte auf Name");
    logPrintf("[Pairing] PSK empfangen, Kanal '%s' gespeichert\n", channelName);
}

static void handlePskAck(const uint8_t *payload, int payloadLen) {
    if (ctx.state != PAIR_GIVER_WAIT_ACK) return;
    if (payloadLen < 1 || payload[0] != 0x00) return;

    ctx.state = PAIR_GIVER_NAME_INPUT;
    displayStatus("Pairing", "Name eingeben", "im Browser");
    logPrintf("[Pairing] PSK_ACK empfangen — Name eingeben\n");
}

static void sendNameAck() {
    uint8_t pkt[PAIRING_HEADER_LEN + 1];
    size_t hlen = buildHeader(PAIRING_TYPE_NAME_ACK, pkt);
    pkt[hlen] = 0x01;
    enqueueSend(pkt, hlen + 1, QUEUE_PAIRING);
}

static void handleNameTransfer(const uint8_t *payload, int payloadLen) {
    if (ctx.state == PAIR_RECEIVER_DONE) {
        sendNameAck();
        logPrintf("[Pairing] NAME_TRANSFER wiederholt — NAME_ACK erneut gesendet\n");
        return;
    }
    if (ctx.state != PAIR_RECEIVER_WAIT_NAME) return;
    if (payloadLen < 1) return;

    int nameLen = payloadLen > 20 ? 20 : payloadLen;
    memset(ctx.peerName, 0, sizeof(ctx.peerName));
    memcpy(ctx.peerName, payload, nameLen);

    // Lock-Flag: Standard 0x00 (frei). Rückwärtskompatibel: ältere Firmware
    // sendet NAME_TRANSFER ohne Byte 20 (payloadLen == 20) → lockFlag = 0x00.
    uint8_t lockFlag = (payloadLen >= 21) ? payload[20] : 0;

    storageStoreChannelSenderName(ctx.channel, ctx.peerName);
    storageStoreChannelNameLock(ctx.channel, lockFlag);
    reloadChannelPsks();

    if (strcmp(deviceName, "Unbekannt") == 0) {
        storageStoreDeviceName(ctx.peerName);
        strncpy(deviceName, ctx.peerName, 20);
        deviceName[20] = '\0';
    }

    sendNameAck();

    ctx.state = PAIR_RECEIVER_DONE;
    ecdhCleanup();
    displayStatus("Pairing OK", ctx.channel, ctx.peerName);
    logPrintf("[Pairing] Name empfangen: '%s' (lock=%d), NAME_ACK gesendet\n", ctx.peerName, lockFlag);
}

static void handleNameAck(const uint8_t *payload, int payloadLen) {
    if (ctx.state != PAIR_GIVER_WAIT_NAME_ACK) return;
    if (payloadLen < 1 || payload[0] != 0x01) return;

    ctx.state = PAIR_GIVER_DONE;
    ecdhCleanup();
    displayStatus("Pairing OK", ctx.channel, ctx.pendingName);
    logPrintf("[Pairing] NAME_ACK empfangen — Pairing abgeschlossen\n");
}

// ── Public API ──────────────────────────────────────────────────

void pairingInit() {
    pairingMutex = xSemaphoreCreateMutex();
    memset(&ctx, 0, sizeof(ctx));
    ctx.state = PAIR_IDLE;
}

void pairingHandlePacket(uint8_t *buf, int len) {
    uint8_t type;
    uint32_t sessionId;
    if (!parseHeader(buf, len, &type, &sessionId)) return;

    if (xSemaphoreTake(pairingMutex, pdMS_TO_TICKS(50)) != pdTRUE) return;

    if (type == PAIRING_TYPE_ECDH_INIT) {
        handleEcdhInit(buf, len, sessionId);
    } else {
        if (ctx.state != PAIR_IDLE && sessionId != ctx.sessionId) {
            logPrintf("[Pairing] Session-Mismatch: erwartet %08lX, empfangen %08lX\n",
                ctx.sessionId, sessionId);
            xSemaphoreGive(pairingMutex);
            return;
        }
        const uint8_t *payload = buf + PAIRING_HEADER_LEN;
        int payloadLen = len - PAIRING_HEADER_LEN;

        switch (type) {
            case PAIRING_TYPE_ECDH_RESP:    handleEcdhResp(payload, payloadLen); break;
            case PAIRING_TYPE_PSK_TRANSFER: handlePskTransfer(buf, len); break;
            case PAIRING_TYPE_PSK_ACK:      handlePskAck(payload, payloadLen); break;
            case PAIRING_TYPE_NAME_TRANSFER: handleNameTransfer(payload, payloadLen); break;
            case PAIRING_TYPE_NAME_ACK:      handleNameAck(payload, payloadLen); break;
            default:
                logPrintf("[Pairing] Unbekannter Typ: 0x%02X\n", type);
        }
    }

    xSemaphoreGive(pairingMutex);
}

void pairingTick() {
    if (xSemaphoreTake(pairingMutex, 0) != pdTRUE) return;

    if (ctx.pendingAction != PAIR_ACTION_NONE) {
        PairingPendingAction action = ctx.pendingAction;
        ctx.pendingAction = PAIR_ACTION_NONE;

        switch (action) {
            case PAIR_ACTION_START_GIVE:    doStartGive(); break;
            case PAIR_ACTION_START_RECEIVE: doStartReceive(); break;
            case PAIR_ACTION_CONFIRM:       doConfirm(); break;
            case PAIR_ACTION_SETNAME:       doSetName(); break;
            case PAIR_ACTION_ABORT:         doAbort(); break;
            case PAIR_ACTION_RENAME_CHANNEL: doRenameChannel(); break;
            default: break;
        }
    }

    if (ctx.state == PAIR_IDLE || ctx.state == PAIR_GIVER_DONE ||
        ctx.state == PAIR_RECEIVER_DONE || ctx.state == PAIR_ERROR) {
        xSemaphoreGive(pairingMutex);
        return;
    }

    unsigned long now = millis();

    if (ctx.state == PAIR_GIVER_WAIT_NAME_ACK &&
        now - ctx.nameWaitStartedAt > 60000UL) {
        snprintf(ctx.error, sizeof(ctx.error),
            "Pairing nicht erfolgreich, bitte wiederholen.");
        ctx.state = PAIR_ERROR;
        ecdhCleanup();
        displayStatus("Pairing", "Keine Best.", "Name unklar");
        logPrintf("[Pairing] NAME_ACK Timeout — keine Bestätigung\n");
        xSemaphoreGive(pairingMutex);
        return;
    }

    if (ctx.state == PAIR_RECEIVER_WAIT_NAME &&
        now - ctx.nameWaitStartedAt > PAIRING_NAME_TIMEOUT_MS) {
        ctx.state = PAIR_RECEIVER_DONE;
        ecdhCleanup();
        displayStatus("Pairing OK", ctx.channel, "");
        logPrintf("[Pairing] Name-Timeout — Pairing trotzdem abgeschlossen\n");
        xSemaphoreGive(pairingMutex);
        return;
    }

    if (ctx.state == PAIR_RECEIVER_CHANNEL_RENAME &&
        now - ctx.startedAt > PAIRING_TIMEOUT_MS) {
        logPrintf("[Pairing] Rename-Timeout — verwende Fallback-Namen\n");
        ctx.pendingChannel[0] = '\0';
        doRenameChannel();
        xSemaphoreGive(pairingMutex);
        return;
    }

    if (now - ctx.startedAt > PAIRING_TIMEOUT_MS) {
        snprintf(ctx.error, sizeof(ctx.error),
            "Pairing abgebrochen — kein Gerät gefunden.\n"
            "Mögliche Ursachen:\n"
            "• Geräte zu weit voneinander entfernt\n"
            "• Geräte auf unterschiedlichen LoRa-Presets\n"
            "• Gegenstelle nicht im Pairing-Modus");
        ctx.state = PAIR_ERROR;
        ecdhCleanup();
        displayStatus("Pairing", "Timeout", "");
        logPrintf("[Pairing] Timeout nach %lus\n", (now - ctx.startedAt) / 1000);
        xSemaphoreGive(pairingMutex);
        return;
    }

    bool shouldRetransmit = (ctx.state == PAIR_GIVER_WAIT_RESP ||
                             ctx.state == PAIR_RECEIVER_PIN_DISPLAY ||
                             ctx.state == PAIR_GIVER_WAIT_ACK ||
                             ctx.state == PAIR_GIVER_WAIT_NAME_ACK);
    if (shouldRetransmit && now - ctx.lastTxAt > PAIRING_RETRANSMIT_MS) {
        retransmit();
    }

    xSemaphoreGive(pairingMutex);
}

bool pairingGetStatus(PairingStatus *out) {
    if (xSemaphoreTake(pairingMutex, pdMS_TO_TICKS(50)) != pdTRUE) return false;

    out->state = ctx.state;
    memcpy(out->pin, ctx.pin, sizeof(ctx.pin));
    memcpy(out->channel, ctx.channel, sizeof(ctx.channel));
    memcpy(out->error, ctx.error, sizeof(ctx.error));
    memcpy(out->peerName, ctx.peerName, sizeof(ctx.peerName));

    if (ctx.state != PAIR_IDLE && ctx.state != PAIR_GIVER_DONE &&
        ctx.state != PAIR_RECEIVER_DONE && ctx.state != PAIR_ERROR) {
        unsigned long elapsed = millis() - ctx.startedAt;
        out->remainingMs = (elapsed < PAIRING_TIMEOUT_MS) ? (PAIRING_TIMEOUT_MS - elapsed) : 0;
    } else {
        out->remainingMs = 0;
    }

    xSemaphoreGive(pairingMutex);
    return true;
}

void pairingRequestStartGive(const char *channel, bool lockName) {
    if (xSemaphoreTake(pairingMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    strncpy(ctx.pendingChannel, channel, CHANNEL_NAME_MAX);
    ctx.pendingChannel[CHANNEL_NAME_MAX] = '\0';
    ctx.pendingLockName = lockName ? 0x01 : 0x00;
    ctx.pendingAction = PAIR_ACTION_START_GIVE;
    xSemaphoreGive(pairingMutex);
}

void pairingRequestStartReceive() {
    if (xSemaphoreTake(pairingMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    ctx.pendingAction = PAIR_ACTION_START_RECEIVE;
    xSemaphoreGive(pairingMutex);
}

void pairingRequestConfirm() {
    if (xSemaphoreTake(pairingMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    ctx.pendingAction = PAIR_ACTION_CONFIRM;
    xSemaphoreGive(pairingMutex);
}

void pairingRequestSetName(const char *name) {
    if (xSemaphoreTake(pairingMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    strncpy(ctx.pendingName, name, 20);
    ctx.pendingName[20] = '\0';
    ctx.pendingAction = PAIR_ACTION_SETNAME;
    xSemaphoreGive(pairingMutex);
}

void pairingRequestAbort() {
    if (xSemaphoreTake(pairingMutex, pdMS_TO_TICKS(200)) != pdTRUE) return;
    logPrintf("[Pairing] Abbruch durch Nutzer\n");
    ecdhCleanup();
    PairingState oldState = ctx.state;
    memset(&ctx, 0, sizeof(ctx));
    ctx.state = PAIR_IDLE;
    if (oldState != PAIR_IDLE) displayStatus("ProjektX", deviceName, "");
    xSemaphoreGive(pairingMutex);
}

void pairingRequestRenameChannel(const char *newName) {
    if (xSemaphoreTake(pairingMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    strncpy(ctx.pendingChannel, newName, CHANNEL_NAME_MAX);
    ctx.pendingChannel[CHANNEL_NAME_MAX] = '\0';
    ctx.pendingAction = PAIR_ACTION_RENAME_CHANNEL;
    xSemaphoreGive(pairingMutex);
}

// ── HTTP-Routen ─────────────────────────────────────────────────

static const char *stateToString(PairingState s) {
    switch (s) {
        case PAIR_IDLE:                return "IDLE";
        case PAIR_GIVER_WAIT_RESP:     return "GIVER_WAIT_RESP";
        case PAIR_GIVER_PIN_CONFIRM:   return "GIVER_PIN_CONFIRM";
        case PAIR_GIVER_WAIT_ACK:      return "GIVER_WAIT_ACK";
        case PAIR_GIVER_NAME_INPUT:    return "GIVER_NAME_INPUT";
        case PAIR_GIVER_WAIT_NAME_ACK:     return "GIVER_WAIT_NAME_ACK";
        case PAIR_GIVER_DONE:          return "GIVER_DONE";
        case PAIR_RECEIVER_WAIT_INIT:  return "RECEIVER_WAIT_INIT";
        case PAIR_RECEIVER_PIN_DISPLAY:return "RECEIVER_PIN_DISPLAY";
        case PAIR_RECEIVER_WAIT_NAME:  return "RECEIVER_WAIT_NAME";
        case PAIR_RECEIVER_CHANNEL_RENAME: return "RECEIVER_CHANNEL_RENAME";
        case PAIR_RECEIVER_DONE:       return "RECEIVER_DONE";
        case PAIR_ERROR:               return "ERROR";
        default:                       return "UNKNOWN";
    }
}

void setupPairingRoutes(AsyncWebServer &server) {
    server.on("/pairing/start/give", HTTP_POST,
        [](AsyncWebServerRequest *req) {},
        nullptr,
        [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t, size_t) {
            if (!req->hasHeader("X-Pin") ||
                (uint32_t)req->header("X-Pin").toInt() != settingsPin) {
                portalLog(req, 403); req->send(403); return;
            }
            JsonDocument doc;
            if (deserializeJson(doc, data, len) != DeserializationError::Ok) {
                portalLog(req, 400); req->send(400, "text/plain", "JSON ungültig"); return;
            }
            const char *channel = doc["channel"];
            if (!channel || strlen(channel) == 0) {
                portalLog(req, 400); req->send(400, "text/plain", "Kein Kanal angegeben"); return;
            }
            bool lockName = doc["lockName"] | false;
            pairingRequestStartGive(channel, lockName);
            portalLog(req, 200); req->send(200, "application/json", "{\"ok\":true}");
        });

    server.on("/pairing/start/receive", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!req->hasHeader("X-Pin") ||
            (uint32_t)req->header("X-Pin").toInt() != settingsPin) {
            portalLog(req, 403); req->send(403); return;
        }
        pairingRequestStartReceive();
        portalLog(req, 200); req->send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/pairing/status", HTTP_GET, [](AsyncWebServerRequest *req) {
        if (!req->hasHeader("X-Pin") ||
            (uint32_t)req->header("X-Pin").toInt() != settingsPin) {
            portalLog(req, 403); req->send(403); return;
        }
        PairingStatus st;
        if (!pairingGetStatus(&st)) {
            portalLog(req, 500); req->send(500, "text/plain", "Status nicht verfügbar"); return;
        }
        JsonDocument doc;
        doc["state"] = stateToString(st.state);
        if (st.pin[0] != '\0') doc["pin"] = st.pin;
        doc["remainingMs"] = st.remainingMs;
        if (st.channel[0] != '\0') doc["channel"] = st.channel;
        if (st.error[0] != '\0') doc["error"] = st.error;
        if (st.peerName[0] != '\0') doc["peerName"] = st.peerName;
        String json;
        serializeJson(doc, json);
        portalLog(req, 200); req->send(200, "application/json", json);
    });

    server.on("/pairing/confirm", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!req->hasHeader("X-Pin") ||
            (uint32_t)req->header("X-Pin").toInt() != settingsPin) {
            portalLog(req, 403); req->send(403); return;
        }
        pairingRequestConfirm();
        portalLog(req, 200); req->send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/pairing/abort", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!req->hasHeader("X-Pin") ||
            (uint32_t)req->header("X-Pin").toInt() != settingsPin) {
            portalLog(req, 403); req->send(403); return;
        }
        pairingRequestAbort();
        portalLog(req, 200); req->send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/pairing/setname", HTTP_POST,
        [](AsyncWebServerRequest *req) {},
        nullptr,
        [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t, size_t) {
            if (!req->hasHeader("X-Pin") ||
                (uint32_t)req->header("X-Pin").toInt() != settingsPin) {
                portalLog(req, 403); req->send(403); return;
            }
            JsonDocument doc;
            if (deserializeJson(doc, data, len) != DeserializationError::Ok) {
                portalLog(req, 400); req->send(400, "text/plain", "JSON ungültig"); return;
            }
            const char *name = doc["name"];
            if (!name || strlen(name) == 0) {
                portalLog(req, 400); req->send(400, "text/plain", "Kein Name angegeben"); return;
            }
            pairingRequestSetName(name);
            portalLog(req, 200); req->send(200, "application/json", "{\"ok\":true}");
        });

    server.on("/pairing/renamechannel", HTTP_POST,
        [](AsyncWebServerRequest *req) {},
        nullptr,
        [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t, size_t) {
            if (!req->hasHeader("X-Pin") ||
                (uint32_t)req->header("X-Pin").toInt() != settingsPin) {
                portalLog(req, 403); req->send(403); return;
            }
            JsonDocument doc;
            if (deserializeJson(doc, data, len) != DeserializationError::Ok) {
                portalLog(req, 400); req->send(400, "text/plain", "JSON ungültig"); return;
            }
            const char *name = doc["name"];
            pairingRequestRenameChannel(name ? name : "");
            portalLog(req, 200); req->send(200, "application/json", "{\"ok\":true}");
        });
}
