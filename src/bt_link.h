#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define BT_LINK_AP_SSID       "AI_BOT"
#define BT_LINK_AP_PASSWORD   "123456789"
#define BT_LINK_TCP_PORT      3333
#define BT_LINK_AP_IP         "192.168.4.1"
#define BT_LINK_MAGIC         0xB7
#define BT_LINK_VERSION       2
#define BT_LINK_TEXT_LEN      192
#define BT_LINK_PCM_CHUNK_LEN 512

typedef enum {
    BT_LINK_MSG_COMMAND = 1,
    BT_LINK_MSG_STATUS = 2,
    BT_LINK_MSG_PCM_START = 3,
    BT_LINK_MSG_PCM_DATA = 4,
    BT_LINK_MSG_PCM_END = 5,
    BT_LINK_MSG_PCM_STOP = 6,
} bt_link_msg_type_t;

typedef struct __attribute__((packed)) {
    uint8_t magic;
    uint8_t version;
    uint8_t type;
    uint8_t seq;
    uint16_t length;
    union {
        char text[BT_LINK_TEXT_LEN];
        uint8_t pcm[BT_LINK_PCM_CHUNK_LEN];
    } data;
} bt_link_packet_t;

_Static_assert(sizeof(bt_link_packet_t) == 6 + BT_LINK_PCM_CHUNK_LEN,
               "S3/WROOM TCP packet layout must remain synchronized");

typedef void (*bt_link_command_cb_t)(const char *command, const uint8_t src[6], uint8_t seq);

esp_err_t bt_link_init(void);
bool bt_link_is_ready(void);
bool bt_link_is_connected(void);
void bt_link_set_command_handler(bt_link_command_cb_t cb);
esp_err_t bt_link_send_status(const uint8_t dest[6], uint8_t seq, const char *text);
esp_err_t bt_link_broadcast_status(const char *text);
void bt_link_pause_tx(void);
void bt_link_resume_tx(void);
