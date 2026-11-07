#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_bt_defs.h"
#include "esp_err.h"

#define BT_MAX_DEVICES 24

typedef struct {
    esp_bd_addr_t bda;
    char name[64];
    int8_t rssi;
    uint32_t cod;
} bt_dev_t;

typedef enum {
    BT_A2DP_STATE_IDLE = 0,     /* chua ket noi */
    BT_A2DP_STATE_CONNECTING,   /* dang page/ket noi */
    BT_A2DP_STATE_CONNECTED,    /* da ket noi AVDTP, chua stream */
    BT_A2DP_STATE_STREAMING,    /* dang truyen PCM */
    BT_A2DP_STATE_FAILED,       /* lan ket noi vua roi that bai */
} bt_a2dp_state_t;

/* Khoi tao Bluetooth Classic + A2DP source (goi sau nvs_flash_init). */
esp_err_t bt_a2dp_init(void);

/* Bat dau quet thiet bi (inquiry ~13 giay). */
esp_err_t bt_a2dp_start_scan(void);
/* Save/load the last completed Bluetooth inquiry results in NVS. */
esp_err_t bt_a2dp_save_device_cache(void);
esp_err_t bt_a2dp_load_device_cache(void);
/* Huy quet neu dang quet. */
void bt_a2dp_cancel_scan(void);
bool bt_a2dp_scan_done(void);
bool bt_a2dp_is_scanning(void);

int bt_a2dp_device_count(void);
const bt_dev_t *bt_a2dp_device(int index);
/* true neu Class of Device cho thay day la thiet bi audio (loa/tai nghe). */
bool bt_a2dp_device_is_audio(int index);

/* Ket noi A2DP toi thiet bi trong danh sach quet duoc. */
esp_err_t bt_a2dp_connect(int index);
void bt_a2dp_disconnect(void);

/* Tam dung / tiep tuc stream nhung GIU ket noi (AVDTP).  Khi pause, timer
 * retry khong tu bat lai stream nua. */
void bt_a2dp_suspend(void);
void bt_a2dp_resume(void);
bool bt_a2dp_is_suspended(void);

bt_a2dp_state_t bt_a2dp_state(void);
const char *bt_a2dp_state_str(void);

bool bt_a2dp_is_connected(void);
bool bt_a2dp_is_streaming(void);
