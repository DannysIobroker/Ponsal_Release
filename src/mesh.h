#pragma once
#include <stdint.h>
#include "storage.h"

#define PROTOCOL_VERSION  0x01
#define FLAG_CHAT         0x01
#define PACKET_MAX        255
#define PACKET_MIN        61

struct ChannelPsk {
    char name[CHANNEL_NAME_MAX + 1];
    uint8_t psk[32];
    uint8_t networkId[2];
    char senderName[21];
    uint8_t nameLocked;
};

#define SEND_QUEUE_SIZE 12
#define RELAY_TTL_MS (5 * 60 * 1000UL)

enum QueueEntryType {
    QUEUE_OWN     = 0,
    QUEUE_RELAY   = 1,
    QUEUE_PAIRING = 2
};

void meshInit();
bool meshIsDuplicate(uint32_t msgId);
void meshAddToDedup(uint32_t msgId);
void meshHandleIncoming(uint8_t *buf, int len);
void meshAutoReplyTick();
bool enqueueSend(const uint8_t *data, size_t len, QueueEntryType type);
bool enqueueOwnMessage(const char *text, const char *channel, uint32_t timestamp);
