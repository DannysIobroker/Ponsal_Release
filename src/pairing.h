#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <ESPAsyncWebServer.h>
#include "storage.h"

#define PAIRING_MAGIC              0x50
#define PAIRING_TYPE_ECDH_INIT     0x01
#define PAIRING_TYPE_ECDH_RESP     0x02
#define PAIRING_TYPE_PSK_TRANSFER  0x03
#define PAIRING_TYPE_PSK_ACK       0x04
#define PAIRING_TYPE_NAME_TRANSFER 0x05
#define PAIRING_TYPE_NAME_ACK      0x06

#define PAIRING_HEADER_LEN         6
#define PAIRING_PUBKEY_LEN         32
#define PAIRING_TIMEOUT_MS         180000UL
#define PAIRING_RETRANSMIT_MS      30000UL
#define PAIRING_NAME_TIMEOUT_MS    90000UL

enum PairingState {
    PAIR_IDLE = 0,
    PAIR_GIVER_WAIT_RESP,
    PAIR_GIVER_PIN_CONFIRM,
    PAIR_GIVER_WAIT_ACK,
    PAIR_GIVER_NAME_INPUT,
    PAIR_GIVER_WAIT_NAME_ACK,
    PAIR_GIVER_DONE,
    PAIR_RECEIVER_WAIT_INIT,
    PAIR_RECEIVER_PIN_DISPLAY,
    PAIR_RECEIVER_WAIT_NAME,
    PAIR_RECEIVER_CHANNEL_RENAME,
    PAIR_RECEIVER_DONE,
    PAIR_ERROR
};

enum PairingPendingAction {
    PAIR_ACTION_NONE = 0,
    PAIR_ACTION_START_GIVE,
    PAIR_ACTION_START_RECEIVE,
    PAIR_ACTION_CONFIRM,
    PAIR_ACTION_SETNAME,
    PAIR_ACTION_ABORT,
    PAIR_ACTION_RENAME_CHANNEL
};

struct PairingStatus {
    PairingState state;
    char pin[7];
    unsigned long remainingMs;
    char channel[CHANNEL_NAME_MAX + 1];
    char error[128];
    char peerName[21];
};

void pairingInit();
void pairingHandlePacket(uint8_t *buf, int len);
void pairingTick();
void setupPairingRoutes(AsyncWebServer &server);
bool pairingGetStatus(PairingStatus *out);

void pairingRequestStartGive(const char *channel, bool lockName = false);
void pairingRequestStartReceive();
void pairingRequestConfirm();
void pairingRequestSetName(const char *name);
void pairingRequestAbort();
void pairingRequestRenameChannel(const char *newName);
