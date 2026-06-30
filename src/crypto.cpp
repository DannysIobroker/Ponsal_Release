// crypto.cpp
// NodeID, NetzwerkID, AES-256-GCM via mbedTLS (ESP-IDF, im Arduino Core enthalten)

#include "crypto.h"
#include <Arduino.h>
#include <esp_mac.h>
#include "mbedtls/sha256.h"
#include "mbedtls/gcm.h"

// ── RNG-Callback für mbedTLS ───────────────────────────────────
int espRngCallback(void *ctx, unsigned char *buf, size_t len) {
    (void)ctx;
    size_t i = 0;
    while (i + 4 <= len) {
        uint32_t r = esp_random();
        memcpy(buf + i, &r, 4);
        i += 4;
    }
    if (i < len) {
        uint32_t r = esp_random();
        memcpy(buf + i, &r, len - i);
    }
    return 0;
}

// ── NodeID ─────────────────────────────────────────────────────
void computeNodeId(uint8_t out[2]) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    uint8_t hash[32];
    mbedtls_sha256(mac, 6, hash, 0);  // 0 = SHA-256 (nicht SHA-224)

    out[0] = hash[0];
    out[1] = hash[1];
}

// ── NetzwerkID ─────────────────────────────────────────────────
void computeNetworkId(const uint8_t psk[32], uint8_t out[2]) {
    uint8_t hash[32];
    mbedtls_sha256(psk, 32, hash, 0);

    out[0] = hash[0];
    out[1] = hash[1];
}

// ── IV generieren ──────────────────────────────────────────────
void generateIV(uint8_t iv[12]) {
    // esp_random() liefert hardware-basierte Zufallszahlen
    for (int i = 0; i < 3; i++) {
        uint32_t r = esp_random();
        iv[i*4 + 0] = (r >> 24) & 0xFF;
        iv[i*4 + 1] = (r >> 16) & 0xFF;
        iv[i*4 + 2] = (r >>  8) & 0xFF;
        iv[i*4 + 3] =  r        & 0xFF;
    }
}

// ── AES-256-GCM verschlüsseln ──────────────────────────────────
bool encryptPayload(
    const uint8_t key[32],
    const uint8_t iv[12],
    const uint8_t *aad, size_t aadLen,
    const uint8_t *plaintext, size_t plaintextLen,
    uint8_t *ciphertext,
    uint8_t tag[16]
) {
    mbedtls_gcm_context ctx;
    mbedtls_gcm_init(&ctx);

    int ret = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (ret != 0) {
        mbedtls_gcm_free(&ctx);
        return false;
    }

    ret = mbedtls_gcm_crypt_and_tag(
        &ctx,
        MBEDTLS_GCM_ENCRYPT,
        plaintextLen,
        iv, 12,
        aad, aadLen,
        plaintext,
        ciphertext,
        16, tag
    );

    mbedtls_gcm_free(&ctx);
    return (ret == 0);
}

// ── AES-256-GCM entschlüsseln ──────────────────────────────────
bool decryptPayload(
    const uint8_t key[32],
    const uint8_t iv[12],
    const uint8_t *aad, size_t aadLen,
    const uint8_t *ciphertext, size_t ciphertextLen,
    const uint8_t tag[16],
    uint8_t *plaintext
) {
    mbedtls_gcm_context ctx;
    mbedtls_gcm_init(&ctx);

    int ret = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (ret != 0) {
        mbedtls_gcm_free(&ctx);
        return false;
    }

    ret = mbedtls_gcm_auth_decrypt(
        &ctx,
        ciphertextLen,
        iv, 12,
        aad, aadLen,
        tag, 16,
        ciphertext,
        plaintext
    );

    mbedtls_gcm_free(&ctx);

    // ret == 0: erfolgreich entschlüsselt und Tag verifiziert
    // ret == MBEDTLS_ERR_GCM_AUTH_FAILED: Tag ungültig
    //
    // Wichtig: GCM-Auth bestätigt nur, dass die gesendeten Bytes integer
    // sind (kein Bit-Flip, korrekter PSK). Sie prüft NICHT, ob der Inhalt
    // dem Paketformat entspricht (feste Offsets Name/Trenner/Timestamp).
    // Format-Konformität muss separat durch Längenprüfung sichergestellt
    // werden (PACKET_MIN in meshHandleIncoming).
    return (ret == 0);
}
