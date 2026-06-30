#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Berechnet NodeID = SHA-256(MAC)[0:2]
void computeNodeId(uint8_t out[2]);

// Berechnet NetzwerkID = SHA-256(PSK)[0:2]
void computeNetworkId(const uint8_t psk[32], uint8_t out[2]);

// Generiert 12 zufällige Bytes für IV
void generateIV(uint8_t iv[12]);

// AES-256-GCM verschlüsseln
// Gibt false zurück bei Fehler
bool encryptPayload(
    const uint8_t key[32],
    const uint8_t iv[12],
    const uint8_t *aad, size_t aadLen,
    const uint8_t *plaintext, size_t plaintextLen,
    uint8_t *ciphertext,      // gleiche Länge wie plaintext
    uint8_t tag[16]
);

// RNG-Callback für mbedTLS (wrapped esp_random())
int espRngCallback(void *ctx, unsigned char *buf, size_t len);

// AES-256-GCM entschlüsseln + verifizieren
// Gibt false zurück wenn Tag ungültig (falscher PSK, manipuliertes Paket)
bool decryptPayload(
    const uint8_t key[32],
    const uint8_t iv[12],
    const uint8_t *aad, size_t aadLen,
    const uint8_t *ciphertext, size_t ciphertextLen,
    const uint8_t tag[16],
    uint8_t *plaintext        // gleiche Länge wie ciphertext
);
