/*
 * Bluetooth Classic A2DP Source: quet thiet bi, ket noi, va cap PCM 44.1 kHz
 * stereo cho stack ma hoa SBC gui sang loa/tai nghe Bluetooth.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "nvs.h"

#include "bt_a2dp.h"
#include "mp3_player.h"

#define TAG                 "BT"
#define LOCAL_DEVICE_NAME   "ESP32_MP3_Speaker"
#define INQUIRY_SECONDS     10          /* ~12.8 giay thuc te */
#define DEVICE_CACHE_NAMESPACE "bt_devices"
#define DEVICE_CACHE_KEY       "cache"
#define DEVICE_CACHE_MAGIC     0x42544456
#define DEVICE_CACHE_VERSION   1

/* Page timeout 0x4000 * 0.625 ms = 10.24 s (mac dinh 0x2000 = 5.12 s).
 * Nhieu loa Bluetooth tra loi page cham, tang len de giam loi 0x04. */
#define PAGE_TIMEOUT        0x4000

static bt_dev_t s_devs[BT_MAX_DEVICES];
static volatile int s_dev_count;
static volatile bool s_scanning;
static volatile bool s_scan_done;

typedef struct {
    uint32_t magic;
    uint8_t version;
    uint8_t count;
    bt_dev_t devices[BT_MAX_DEVICES];
} bt_device_cache_t;

static esp_bd_addr_t s_peer_bda;
static volatile bt_a2dp_state_t s_state = BT_A2DP_STATE_IDLE;
static volatile bool s_start_pending;
static volatile bool s_suspended;      /* pause: khong tu bat lai stream */
static TimerHandle_t s_retry_timer;

static char *bda2str(const esp_bd_addr_t bda, char *str, size_t size)
{
    if (str == NULL || size < 18) {
        return NULL;
    }
    snprintf(str, size, "%02x:%02x:%02x:%02x:%02x:%02x",
             bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
    return str;
}

static const char *eir_name(uint8_t *eir, char *out, size_t out_len)
{
    uint8_t *name = NULL;
    uint8_t name_len = 0;

    if (eir != NULL) {
        name = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &name_len);
        if (name == NULL) {
            name = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &name_len);
        }
    }
    if (name == NULL || name_len == 0) {
        return NULL;
    }
    if (name_len >= out_len) {
        name_len = out_len - 1;
    }
    memcpy(out, name, name_len);
    out[name_len] = '\0';
    return out;
}

/* Tra ve 1 neu them thiet bi moi, 0 neu da co. */
static int add_device(const esp_bd_addr_t bda, const char *name, int8_t rssi, uint32_t cod)
{
    for (int i = 0; i < s_dev_count; i++) {
        if (memcmp(s_devs[i].bda, bda, ESP_BD_ADDR_LEN) == 0) {
            if (name != NULL && name[0] != '\0' &&
                strcmp(s_devs[i].name, "(khong ten)") == 0) {
                snprintf(s_devs[i].name, sizeof(s_devs[i].name), "%s", name);
            }
            if (rssi > s_devs[i].rssi) {
                s_devs[i].rssi = rssi;
            }
            return 0;
        }
    }

    if (s_dev_count >= BT_MAX_DEVICES) {
        return 0;
    }

    bt_dev_t *d = &s_devs[s_dev_count++];
    memcpy(d->bda, bda, ESP_BD_ADDR_LEN);
    snprintf(d->name, sizeof(d->name), "%s", (name != NULL && name[0] != '\0') ? name : "(khong ten)");
    d->rssi = rssi;
    d->cod = cod;
    return 1;
}

static void gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT: {
        if (!s_scanning) {
            break;
        }
        char name[64] = {0};
        char bda_str[18];
        int8_t rssi = -127;
        uint32_t cod = 0;
        uint8_t *eir = NULL;
        bool have_name = false;

        for (int i = 0; i < param->disc_res.num_prop; i++) {
            esp_bt_gap_dev_prop_t *p = &param->disc_res.prop[i];
            switch (p->type) {
            case ESP_BT_GAP_DEV_PROP_BDNAME:
                if (p->len > 0 && p->len < (int)sizeof(name)) {
                    memcpy(name, p->val, p->len);
                    name[p->len] = '\0';
                    have_name = true;
                }
                break;
            case ESP_BT_GAP_DEV_PROP_COD:
                cod = *(uint32_t *)p->val;
                break;
            case ESP_BT_GAP_DEV_PROP_RSSI:
                rssi = *(int8_t *)p->val;
                break;
            case ESP_BT_GAP_DEV_PROP_EIR:
                eir = (uint8_t *)p->val;
                break;
            default:
                break;
            }
        }

        if (!have_name && eir_name(eir, name, sizeof(name)) != NULL) {
            have_name = true;
        }

        /* Chi in khi phat hien thiet bi moi de khong lam ngap log. */
        if (add_device(param->disc_res.bda, have_name ? name : NULL, rssi, cod)) {
            ESP_LOGI(TAG, "Thay thiet bi: %s %s (rssi %d, cod 0x%06x)",
                     bda2str(param->disc_res.bda, bda_str, sizeof(bda_str)),
                     have_name ? name : "(khong ten)", rssi, (unsigned)cod);
        }
        break;
    }
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
            s_scanning = false;
            s_scan_done = true;
            ESP_LOGI(TAG, "Quet xong: %d thiet bi", s_dev_count);
        } else if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED) {
            ESP_LOGI(TAG, "Bat dau quet...");
        }
        break;
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        ESP_LOGI(TAG, "Xac thuc %s: %s", param->auth_cmpl.device_name,
                 param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS ? "OK" : "that bai");
        break;
    case ESP_BT_GAP_PIN_REQ_EVT: {
        esp_bt_pin_code_t pin = {'0', '0', '0', '0'};
        esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin);
        break;
    }
    case ESP_BT_GAP_CFM_REQ_EVT:
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;
    case ESP_BT_GAP_MODE_CHG_EVT:
        ESP_LOGI(TAG, "Che do cong suat BT: mode %d, interval %u",
                 (int)param->mode_chg.mode, (unsigned)param->mode_chg.interval);
        break;
    case ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT:        /* st 0x04 = page timeout, 0x05/0x06 = loi xac thuc, 0x00 = OK */
        if (param->acl_conn_cmpl_stat.stat != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGW(TAG, "ACL connect that bai: stat 0x%02x",
                     (unsigned)param->acl_conn_cmpl_stat.stat);
        } else {
            ESP_LOGI(TAG, "ACL da ket noi (handle 0x%04x)",
                     (unsigned)param->acl_conn_cmpl_stat.handle);
        }
        break;
    case ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT:
        ESP_LOGW(TAG, "ACL ngat (reason 0x%02x)",
                 (unsigned)param->acl_disconn_cmpl_stat.reason);
        break;
    default:
        break;
    }
}

static void avrc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param)
{
    if (event == ESP_AVRC_CT_CONNECTION_STATE_EVT) {
        ESP_LOGI(TAG, "AVRCP %s", param->conn_stat.connected ? "da ket noi" : "da ngat");
    }
}

static uint32_t s_feed_calls;
static uint32_t s_feed_empty;
static uint32_t s_feed_bytes;

static int32_t a2d_data_cb(uint8_t *buf, int32_t len)
{
    if (buf == NULL || len <= 0) {
        return 0;
    }
    int32_t n = mp3_player_read(buf, len);
    if (n < len) {
        memset(buf + n, 0, (size_t)(len - n));   /* underrun -> im lang */
    }
    s_feed_calls++;
    s_feed_bytes += (uint32_t)(n > 0 ? n : 0);
    if (n == 0) {
        s_feed_empty++;
    }
    if ((s_feed_calls % 500) == 0) {
        ESP_LOGI(TAG, "feed: %lu lan goi (len %ld), %lu lan ring rong, %lu byte doc duoc (heap %u)",
                 (unsigned long)s_feed_calls, (long)len, (unsigned long)s_feed_empty,
                 (unsigned long)s_feed_bytes,
                 (unsigned)esp_get_free_heap_size());
    }
    return len;
}

static void a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT:
        switch (param->conn_stat.state) {
        case ESP_A2D_CONNECTION_STATE_CONNECTING:
            s_state = BT_A2DP_STATE_CONNECTING;
            ESP_LOGI(TAG, "A2DP dang ket noi...");
            break;
        case ESP_A2D_CONNECTION_STATE_CONNECTED:
            s_state = BT_A2DP_STATE_CONNECTED;
            ESP_LOGI(TAG, "Da ket noi A2DP (MTU %u)%s",
                     (unsigned)param->conn_stat.audio_mtu,
                     s_suspended ? " - dang pause" : ", cho chon bai...");
            if (!s_suspended) {
                s_start_pending = true;
                esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
            }
            break;
        case ESP_A2D_CONNECTION_STATE_DISCONNECTING:
            ESP_LOGI(TAG, "A2DP dang ngat ket noi...");
            break;
        case ESP_A2D_CONNECTION_STATE_DISCONNECTED:
            s_start_pending = false;
            if (s_state == BT_A2DP_STATE_CONNECTING) {
                s_state = BT_A2DP_STATE_FAILED;
                ESP_LOGW(TAG, "Ket noi A2DP that bai");
            } else if (s_state == BT_A2DP_STATE_CONNECTED || s_state == BT_A2DP_STATE_STREAMING) {
                s_state = BT_A2DP_STATE_IDLE;
                ESP_LOGW(TAG, "Da ngat ket noi A2DP (%s)",
                         param->conn_stat.disc_rsn == ESP_A2D_DISC_RSN_ABNORMAL
                             ? "mat tin hieu" : "binh thuong");
            }
            break;
        }
        break;

    case ESP_A2D_AUDIO_CFG_EVT: {
        const esp_a2d_cie_sbc_t *s = &param->audio_cfg.mcc.cie.sbc_info;
        if (param->audio_cfg.mcc.type == ESP_A2D_MCT_SBC) {
            unsigned hz = 0;
            if (s->samp_freq & ESP_A2D_SBC_CIE_SF_44K) {
                hz = 44100;
            } else if (s->samp_freq & ESP_A2D_SBC_CIE_SF_48K) {
                hz = 48000;
            } else if (s->samp_freq & ESP_A2D_SBC_CIE_SF_32K) {
                hz = 32000;
            } else if (s->samp_freq & ESP_A2D_SBC_CIE_SF_16K) {
                hz = 16000;
            }
            ESP_LOGI(TAG, "Codec SBC: %u Hz, ch_mode 0x%x, bitpool %u..%u, subband 0x%x, block 0x%x",
                     hz, s->ch_mode, s->min_bitpool, s->max_bitpool,
                     s->num_subbands, s->block_len);
        } else {
            ESP_LOGW(TAG, "Codec khong phai SBC (type %d)", (int)param->audio_cfg.mcc.type);
        }
        break;
    }

    case ESP_A2D_MEDIA_CTRL_ACK_EVT:
        if (param->media_ctrl_stat.status != ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
            ESP_LOGW(TAG, "media ctrl %d that bai (status %d)",
                     (int)param->media_ctrl_stat.cmd, (int)param->media_ctrl_stat.status);
            if (param->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY) {
                s_start_pending = false;
            }
            break;
        }
        if (param->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY) {
            if (s_suspended) {
                /* Pause arrived while the source was getting ready. */
                s_start_pending = false;
                esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_SUSPEND);
            } else {
                esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
                s_start_pending = true;
            }
        } else if (param->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_START) {
            s_state = BT_A2DP_STATE_STREAMING;
            s_start_pending = false;
            ESP_LOGI(TAG, "Bat dau stream A2DP");
        } else if (param->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_SUSPEND) {
            if (s_state == BT_A2DP_STATE_STREAMING) {
                s_state = BT_A2DP_STATE_CONNECTED;
            }
        }
        break;

    case ESP_A2D_AUDIO_STATE_EVT:
        if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED) {
            if (!s_suspended) {
                s_state = BT_A2DP_STATE_STREAMING;
            }
        } else if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_SUSPEND) {
            if (s_state == BT_A2DP_STATE_STREAMING) {
                s_state = BT_A2DP_STATE_CONNECTED;
            }
        }
        break;

    default:
        break;
    }
}

/* Neu da ket noi nhung media chua chay, thu bat lai moi 3 giay. */
static void retry_timer_cb(TimerHandle_t t)
{
    (void)t;
    if (s_state == BT_A2DP_STATE_CONNECTED && !s_start_pending && !s_suspended) {
        s_start_pending = true;
        esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
    }
}

esp_err_t bt_a2dp_init(void)
{
    esp_err_t ret;

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_bt_controller_init: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_bt_controller_enable: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_bluedroid_config_t bd_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    ret = esp_bluedroid_init_with_cfg(&bd_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_bluedroid_init_with_cfg: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_bluedroid_enable();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_bluedroid_enable: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(iocap));

    esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_VARIABLE;
    esp_bt_pin_code_t pin_code = {0};
    esp_bt_gap_set_pin(pin_type, 0, pin_code);

    esp_bt_gap_set_device_name(LOCAL_DEVICE_NAME);
    esp_bt_gap_register_callback(gap_cb);

    /* Tang page timeout truoc khi ket noi (co hieu luc cho lan page ke tiep). */
    ret = esp_bt_gap_set_page_timeout(PAGE_TIMEOUT);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_bt_gap_set_page_timeout: %s", esp_err_to_name(ret));
    }

    /* Khoi tao AVRCP truoc A2DP de BTA dang ky ca AVRCP (nhieu loa can). */
    ret = esp_avrc_ct_init();
    if (ret == ESP_OK) {
        esp_avrc_ct_register_callback(avrc_ct_cb);
    } else {
        ESP_LOGW(TAG, "esp_avrc_ct_init: %s", esp_err_to_name(ret));
    }

    ret = esp_a2d_source_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_a2d_source_init: %s", esp_err_to_name(ret));
        return ret;
    }
    esp_a2d_register_callback(a2d_cb);
    esp_a2d_source_register_data_callback(a2d_data_cb);

    /* Khong cho thiet bi khac tu ket noi vao */
    esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);

    s_retry_timer = xTimerCreate("a2dp_retry", pdMS_TO_TICKS(3000), pdTRUE, NULL, retry_timer_cb);
    if (s_retry_timer != NULL) {
        xTimerStart(s_retry_timer, 0);
    }

    ESP_LOGI(TAG, "Bluetooth san sang, ten thiet bi: %s", LOCAL_DEVICE_NAME);
    return ESP_OK;
}

esp_err_t bt_a2dp_start_scan(void)
{
    if (s_scanning) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_state == BT_A2DP_STATE_CONNECTED || s_state == BT_A2DP_STATE_STREAMING ||
        s_state == BT_A2DP_STATE_CONNECTING) {
        return ESP_ERR_INVALID_STATE;
    }
    s_dev_count = 0;
    s_scan_done = false;
    s_scanning = true;

    esp_err_t ret = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, INQUIRY_SECONDS, 0);
    if (ret != ESP_OK) {
        s_scanning = false;
        ESP_LOGE(TAG, "Khong bat dau duoc qua trinh quet: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t bt_a2dp_save_device_cache(void)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(DEVICE_CACHE_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    bt_device_cache_t cache = {0};
    cache.magic = DEVICE_CACHE_MAGIC;
    cache.version = DEVICE_CACHE_VERSION;
    cache.count = (uint8_t)s_dev_count;
    memcpy(cache.devices, s_devs, (size_t)s_dev_count * sizeof(s_devs[0]));

    ret = nvs_set_blob(handle, DEVICE_CACHE_KEY, &cache, sizeof(cache));
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Da luu danh sach Bluetooth: %u thiet bi", (unsigned)cache.count);
    }
    return ret;
}

esp_err_t bt_a2dp_load_device_cache(void)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(DEVICE_CACHE_NAMESPACE, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    bt_device_cache_t cache = {0};
    size_t size = sizeof(cache);
    ret = nvs_get_blob(handle, DEVICE_CACHE_KEY, &cache, &size);
    nvs_close(handle);
    if (ret != ESP_OK) {
        return ret;
    }
    if (size != sizeof(cache) || cache.magic != DEVICE_CACHE_MAGIC ||
        cache.version != DEVICE_CACHE_VERSION || cache.count > BT_MAX_DEVICES) {
        ESP_LOGW(TAG, "Danh sach Bluetooth luu tren NVS khong hop le");
        return ESP_ERR_INVALID_SIZE;
    }
    for (int i = 0; i < cache.count; i++) {
        if (memchr(cache.devices[i].name, '\0', sizeof(cache.devices[i].name)) == NULL) {
            ESP_LOGW(TAG, "Ten thiet bi trong cache NVS khong hop le");
            return ESP_ERR_INVALID_SIZE;
        }
    }

    memcpy(s_devs, cache.devices, (size_t)cache.count * sizeof(s_devs[0]));
    s_dev_count = cache.count;
    ESP_LOGI(TAG, "Da nap danh sach Bluetooth tu NVS: %u thiet bi", (unsigned)cache.count);
    return ESP_OK;
}

void bt_a2dp_cancel_scan(void)
{
    if (s_scanning) {
        esp_bt_gap_cancel_discovery();
    }
}

bool bt_a2dp_scan_done(void)
{
    return s_scan_done;
}

bool bt_a2dp_is_scanning(void)
{
    return s_scanning;
}

int bt_a2dp_device_count(void)
{
    return s_dev_count;
}

const bt_dev_t *bt_a2dp_device(int index)
{
    if (index < 0 || index >= s_dev_count) {
        return NULL;
    }
    return &s_devs[index];
}

bool bt_a2dp_device_is_audio(int index)
{
    const bt_dev_t *d = bt_a2dp_device(index);
    if (d == NULL) {
        return false;
    }
    uint8_t major = (uint8_t)((d->cod & ESP_BT_COD_MAJOR_DEV_BIT_MASK) >> ESP_BT_COD_MAJOR_DEV_BIT_OFFSET);
    return major == ESP_BT_COD_MAJOR_DEV_AV;
}

esp_err_t bt_a2dp_connect(int index)
{
    if (index < 0 || index >= s_dev_count) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_state == BT_A2DP_STATE_CONNECTING || s_state == BT_A2DP_STATE_CONNECTED ||
        s_state == BT_A2DP_STATE_STREAMING) {
        return ESP_ERR_INVALID_STATE;
    }

    memcpy(s_peer_bda, s_devs[index].bda, ESP_BD_ADDR_LEN);
    s_state = BT_A2DP_STATE_CONNECTING;
    /* Keep A2DP idle until a song decoder is ready; starting an empty source
     * stream wastes scarce internal heap and can exhaust the BT stack. */
    s_suspended = true;
    s_start_pending = false;
    s_feed_calls = 0;
    s_feed_empty = 0;

    esp_err_t ret = esp_a2d_source_connect(s_peer_bda);
    if (ret != ESP_OK) {
        s_state = BT_A2DP_STATE_FAILED;
        ESP_LOGE(TAG, "esp_a2d_source_connect: %s", esp_err_to_name(ret));
    }
    return ret;
}

void bt_a2dp_disconnect(void)
{
    if (s_state == BT_A2DP_STATE_CONNECTED || s_state == BT_A2DP_STATE_STREAMING) {
        esp_a2d_source_disconnect(s_peer_bda);
    }
    s_start_pending = false;
    s_suspended = false;
    if (s_state != BT_A2DP_STATE_CONNECTING) {
        s_state = BT_A2DP_STATE_IDLE;
    }
}

void bt_a2dp_suspend(void)
{
    s_suspended = true;
    s_start_pending = false;
    if (s_state == BT_A2DP_STATE_CONNECTED || s_state == BT_A2DP_STATE_STREAMING) {
        esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_SUSPEND);
    }
}

void bt_a2dp_resume(void)
{
    s_suspended = false;
    if (bt_a2dp_is_connected() && !s_start_pending) {
        s_start_pending = true;
        esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
    }
}

bool bt_a2dp_is_suspended(void)
{
    return s_suspended;
}

bt_a2dp_state_t bt_a2dp_state(void)
{
    return s_state;
}

const char *bt_a2dp_state_str(void)
{
    switch (s_state) {
    case BT_A2DP_STATE_IDLE:       return "chua ket noi";
    case BT_A2DP_STATE_CONNECTING: return "dang ket noi";
    case BT_A2DP_STATE_CONNECTED:  return "da ket noi, chua stream";
    case BT_A2DP_STATE_STREAMING:  return "dang phat nhac";
    case BT_A2DP_STATE_FAILED:     return "ket noi that bai";
    default:                       return "khong ro";
    }
}

bool bt_a2dp_is_connected(void)
{
    return s_state == BT_A2DP_STATE_CONNECTED || s_state == BT_A2DP_STATE_STREAMING;
}

bool bt_a2dp_is_streaming(void)
{
    return s_state == BT_A2DP_STATE_STREAMING;
}
