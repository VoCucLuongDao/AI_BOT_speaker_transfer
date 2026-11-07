#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "bt_a2dp.h"
#include "bt_link.h"
#include "mp3_player.h"

#define TAG                 "WROOM_LINK"
#define CMD_QUEUE_LEN       6
#define TX_QUEUE_LEN        12
#define WIFI_CONNECTED_BIT  BIT0

typedef struct {
    uint8_t src[6];
    uint8_t seq;
    char text[BT_LINK_TEXT_LEN];
} link_cmd_t;

typedef struct {
    uint8_t seq;
    char text[BT_LINK_TEXT_LEN];
} link_tx_t;

static QueueHandle_t s_cmd_queue;
static QueueHandle_t s_tx_queue;
static TaskHandle_t s_link_task;
static TaskHandle_t s_network_task;
static SemaphoreHandle_t s_tx_mutex;
static EventGroupHandle_t s_wifi_events;
static bt_link_command_cb_t s_command_cb;
static volatile bool s_ready;
static volatile bool s_connected;
static bool s_tx_paused;

static void copy_text(char *dst, size_t len, const char *src)
{
    snprintf(dst, len, "%s", src != NULL ? src : "");
    size_t n = strlen(dst);
    while (n > 0 && (dst[n - 1] == '\r' || dst[n - 1] == '\n')) {
        dst[--n] = '\0';
    }
}

static bool send_all(int socket, const void *data, size_t len)
{
    const uint8_t *bytes = (const uint8_t *)data;
    size_t sent = 0;
    while (sent < len) {
        int ret = send(socket, bytes + sent, len - sent, 0);
        if (ret <= 0) {
            return false;
        }
        sent += (size_t)ret;
    }
    return true;
}

static bool send_packet(int socket, uint8_t type, uint8_t seq, const char *text)
{
    bt_link_packet_t pkt = {0};
    pkt.magic = BT_LINK_MAGIC;
    pkt.version = BT_LINK_VERSION;
    pkt.type = type;
    pkt.seq = seq;
    copy_text(pkt.data.text, sizeof(pkt.data.text), text);
    pkt.length = (uint16_t)(strlen(pkt.data.text) + 1);
    return send_all(socket, &pkt, sizeof(pkt));
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *disc =
            (const wifi_event_sta_disconnected_t *)data;
        ESP_LOGW(TAG, "Mat ket noi AP %s; reason=%u rssi=%d; dang ket noi lai",
                 BT_LINK_AP_SSID,
                 disc != NULL ? (unsigned)disc->reason : 0,
                 disc != NULL ? (int)disc->rssi : 0);
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        s_connected = false;
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            ESP_LOGI(TAG, "Da nhan IP tu AP %s (rssi %d, chan %u)",
                     BT_LINK_AP_SSID, (int)ap_info.rssi,
                     (unsigned)ap_info.primary);
        } else {
            ESP_LOGI(TAG, "Da nhan IP tu AP %s", BT_LINK_AP_SSID);
        }
    }
}

static bool is_duplicate(uint8_t seq)
{
    static bool seen;
    static uint8_t previous_seq;
    static int64_t previous_us;
    const int64_t now = esp_timer_get_time();
    if (seen && seq == previous_seq && now - previous_us < 8000 * 1000) {
        return true;
    }
    seen = true;
    previous_seq = seq;
    previous_us = now;
    return false;
}

static void handle_command(const bt_link_packet_t *pkt)
{
    if (pkt->type != BT_LINK_MSG_COMMAND || pkt->length == 0 ||
        pkt->length > sizeof(pkt->data.text) ||
        memchr(pkt->data.text, '\0', pkt->length) == NULL ||
        is_duplicate(pkt->seq)) {
        return;
    }
    link_cmd_t cmd = {0};
    cmd.seq = pkt->seq;
    copy_text(cmd.text, sizeof(cmd.text), pkt->data.text);
    if (xQueueSend(s_cmd_queue, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Command queue full, drop: %s", cmd.text);
    }
}

static void handle_pcm_packet(const bt_link_packet_t *pkt)
{
    switch (pkt->type) {
    case BT_LINK_MSG_PCM_START:
        if (pkt->length != 0) {
            ESP_LOGW(TAG, "Invalid PCM start packet length: %u", (unsigned)pkt->length);
            return;
        }
        if (mp3_player_stream_start() != ESP_OK) {
            ESP_LOGE(TAG, "Could not start incoming PCM playback");
            return;
        }
        bt_a2dp_resume();
        ESP_LOGI(TAG, "Bat dau nhan PCM tu S3");
        break;
    case BT_LINK_MSG_PCM_DATA:
        if (pkt->length == 0 || pkt->length > sizeof(pkt->data.pcm)) {
            ESP_LOGW(TAG, "Invalid PCM data packet length: %u", (unsigned)pkt->length);
            return;
        }
        if (!mp3_player_stream_write(pkt->data.pcm, pkt->length)) {
            ESP_LOGW(TAG, "PCM ring buffer stopped while receiving data");
        }
        break;
    case BT_LINK_MSG_PCM_END:
        if (pkt->length != 0) {
            ESP_LOGW(TAG, "Invalid PCM end packet length: %u", (unsigned)pkt->length);
            return;
        }
        mp3_player_stream_end();
        bt_link_broadcast_status("S3 stream da phat xong");
        ESP_LOGI(TAG, "Da nhan het PCM tu S3");
        break;
    case BT_LINK_MSG_PCM_STOP:
        if (pkt->length != 0) {
            ESP_LOGW(TAG, "Invalid PCM stop packet length: %u", (unsigned)pkt->length);
            return;
        }
        mp3_player_stream_stop();
        bt_a2dp_suspend();
        ESP_LOGI(TAG, "Da dung luong PCM tu S3");
        break;
    default:
        break;
    }
}

static void bt_link_network_task(void *arg)
{
    (void)arg;
    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT,
                                               pdFALSE, pdTRUE, pdMS_TO_TICKS(1000));
        if ((bits & WIFI_CONNECTED_BIT) == 0) {
            continue;
        }

        int socket_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (socket_fd < 0) {
            ESP_LOGE(TAG, "TCP socket create failed: errno=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        struct timeval send_timeout = {1, 0};
        if (setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO,
                       &send_timeout, sizeof(send_timeout)) != 0) {
            ESP_LOGW(TAG, "TCP send timeout setup failed: errno=%d", errno);
        }
        struct sockaddr_in server = {0};
        server.sin_family = AF_INET;
        server.sin_port = htons(BT_LINK_TCP_PORT);
        server.sin_addr.s_addr = inet_addr(BT_LINK_AP_IP);
        ESP_LOGI(TAG, "Ket noi TCP toi %s:%u", BT_LINK_AP_IP,
                 (unsigned)BT_LINK_TCP_PORT);
        if (connect(socket_fd, (struct sockaddr *)&server, sizeof(server)) != 0) {
            ESP_LOGW(TAG, "TCP connect failed: errno=%d", errno);
            close(socket_fd);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        s_connected = true;
        ESP_LOGI(TAG, "TCP link toi S3 da ket noi (heap %u)",
                 (unsigned)esp_get_free_heap_size());
        send_packet(socket_fd, BT_LINK_MSG_STATUS, 0, "link:connected");

        bt_link_packet_t incoming = {0};
        size_t received = 0;
        int64_t last_heartbeat_us = esp_timer_get_time();
        const char *drop_cause = "wifi down";
        bool connection_ok = true;
        while (connection_ok &&
               (xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT) != 0) {
            xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
            link_tx_t outgoing;
            if (!s_tx_paused && xQueueReceive(s_tx_queue, &outgoing, 0) == pdTRUE) {
                const bool sent = send_packet(socket_fd, BT_LINK_MSG_STATUS,
                                              outgoing.seq, outgoing.text);
                xSemaphoreGive(s_tx_mutex);
                if (!sent) {
                    drop_cause = "status tx failed";
                    connection_ok = false;
                    break;
                }
            } else {
                xSemaphoreGive(s_tx_mutex);
            }

            fd_set read_set;
            FD_ZERO(&read_set);
            FD_SET(socket_fd, &read_set);
            struct timeval timeout = {0, 20000};
            int ready = select(socket_fd + 1, &read_set, NULL, NULL, &timeout);
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                drop_cause = "select error";
                connection_ok = false;
            } else if (ready > 0) {
                int ret = recv(socket_fd, (uint8_t *)&incoming + received,
                               sizeof(incoming) - received, 0);
                if (ret <= 0) {
                    drop_cause = "peer closed";
                    connection_ok = false;
                } else {
                    received += (size_t)ret;
                    if (received == sizeof(incoming)) {
                        if (incoming.magic == BT_LINK_MAGIC &&
                            incoming.version == BT_LINK_VERSION) {
                            if (incoming.type == BT_LINK_MSG_COMMAND) {
                                handle_command(&incoming);
                            } else if (incoming.type >= BT_LINK_MSG_PCM_START &&
                                       incoming.type <= BT_LINK_MSG_PCM_STOP) {
                                handle_pcm_packet(&incoming);
                            }
                        }
                        received = 0;
                    }
                }
            }

            const int64_t now = esp_timer_get_time();
            if (now - last_heartbeat_us >= 5 * 1000 * 1000) {
                if (!send_packet(socket_fd, BT_LINK_MSG_STATUS, 0, "link:connected")) {
                    drop_cause = "heartbeat tx failed";
                    connection_ok = false;
                }
                last_heartbeat_us = now;
            }
        }

        s_connected = false;
        if (mp3_player_is_streaming()) {
            mp3_player_stream_stop();
            bt_a2dp_suspend();
        }
        shutdown(socket_fd, SHUT_RDWR);
        close(socket_fd);
        ESP_LOGW(TAG, "TCP link da ngat (%s); se ket noi lai", drop_cause);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

static void bt_link_task(void *arg)
{
    (void)arg;
    link_cmd_t cmd;
    static const uint8_t no_mac[6] = {0};
    for (;;) {
        if (xQueueReceive(s_cmd_queue, &cmd, pdMS_TO_TICKS(200)) != pdTRUE) {
            continue;
        }
        ESP_LOGI(TAG, "Lenh tu S3: %s", cmd.text);
        if (s_command_cb != NULL) {
            bt_link_send_status(no_mac, cmd.seq, "link:busy");
            s_command_cb(cmd.text, no_mac, cmd.seq);
            bt_link_send_status(no_mac, cmd.seq, "link:idle");
        }
    }
}

esp_err_t bt_link_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    s_cmd_queue = xQueueCreate(CMD_QUEUE_LEN, sizeof(link_cmd_t));
    s_tx_queue = xQueueCreate(TX_QUEUE_LEN, sizeof(link_tx_t));
    s_tx_mutex = xSemaphoreCreateMutex();
    s_wifi_events = xEventGroupCreate();
    if (s_cmd_queue == NULL || s_tx_queue == NULL ||
        s_tx_mutex == NULL || s_wifi_events == NULL) {
        ESP_LOGE(TAG, "Failed to create link queues/synchronization");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default: %s", esp_err_to_name(err));
        return err;
    }
    if (esp_netif_create_default_wifi_sta() == NULL) {
        ESP_LOGE(TAG, "Failed to create Wi-Fi STA interface");
        return ESP_FAIL;
    }

    wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wifi_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_storage: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                     wifi_event_handler, NULL);
    if (err == ESP_OK) {
        err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                         wifi_event_handler, NULL);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Register Wi-Fi handlers: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode(STA): %s", esp_err_to_name(err));
        return err;
    }

    wifi_config_t sta_config = {0};
    snprintf((char *)sta_config.sta.ssid, sizeof(sta_config.sta.ssid),
             "%s", BT_LINK_AP_SSID);
    snprintf((char *)sta_config.sta.password, sizeof(sta_config.sta.password),
             "%s", BT_LINK_AP_PASSWORD);
    sta_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    err = esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config(STA): %s", esp_err_to_name(err));
        return err;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_ps: %s", esp_err_to_name(err));
    }
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
        return err;
    }

    if (xTaskCreate(bt_link_task, "bt_link", 6144, NULL, 4, &s_link_task) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create TCP link tasks");
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(bt_link_network_task, "bt_tcp", 4096, NULL, 4,
                    &s_network_task) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create TCP network task");
        vTaskDelete(s_link_task);
        s_link_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_ready = true;
    ESP_LOGI(TAG, "Joining S3 SoftAP '%s'; TCP port %u", BT_LINK_AP_SSID,
             (unsigned)BT_LINK_TCP_PORT);
    return ESP_OK;
}

bool bt_link_is_ready(void)
{
    return s_ready;
}

bool bt_link_is_connected(void)
{
    return s_connected;
}

void bt_link_set_command_handler(bt_link_command_cb_t cb)
{
    s_command_cb = cb;
}

esp_err_t bt_link_send_status(const uint8_t dest[6], uint8_t seq, const char *text)
{
    (void)dest;
    if (!s_ready || s_tx_queue == NULL || text == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    link_tx_t packet = {0};
    packet.seq = seq;
    copy_text(packet.text, sizeof(packet.text), text);
    if (xQueueSend(s_tx_queue, &packet, 0) != pdTRUE) {
        ESP_LOGW(TAG, "TCP status queue full, drop: %s", packet.text);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t bt_link_broadcast_status(const char *text)
{
    return bt_link_send_status(NULL, 0, text);
}

void bt_link_pause_tx(void)
{
    if (s_tx_mutex == NULL) {
        return;
    }
    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    s_tx_paused = true;
    xSemaphoreGive(s_tx_mutex);
}

void bt_link_resume_tx(void)
{
    if (s_tx_mutex == NULL) {
        return;
    }
    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    s_tx_paused = false;
    xSemaphoreGive(s_tx_mutex);
}
