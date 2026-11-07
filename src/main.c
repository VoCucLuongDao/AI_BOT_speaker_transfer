/*
 * ESP32 MP3 -> Bluetooth (A2DP Source)
 *
 * - Menu dieu khien qua UART1 (GPIO16 RX / GPIO17 TX, 115200) = COM6 tren PC.
 * - Quet thiet bi Bluetooth Classic, chon thiet bi + file mp3 trong SPIFFS,
 *   giai ma va phat nhac sang loa/tai nghe Bluetooth.
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "driver/uart.h"
#include "esp_bt.h"
#include "esp_err.h"
#include "esp_gap_bt_api.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "bt_a2dp.h"
#include "bt_link.h"
#include "mp3_player.h"

#define TAG             "app"
#define MOUNT_POINT     "/spiffs"
#define PARTITION_LABEL "storage"

/* Menu console (UART1) */
#define CON_PORT        UART_NUM_1
#define CON_TX_GPIO     17
#define CON_RX_GPIO     16
#define CON_BAUD        115200

#define MAX_FILES       16
#define FILE_NAME_LEN   64

static char s_files[MAX_FILES][FILE_NAME_LEN];
static int s_file_count;

static int s_current_file = -1;     /* bai dang phat / vua phat */
static int s_last_device = -1;      /* thiet bi A2DP dung gan nhat */
static bool s_was_playing;          /* da bat dau phat -> bao "phat xong" */

/* Chi 1 lenh (UART hoac TCP) duoc chay tai 1 thoi diem: A2DP, bo giai ma
 * va danh sach file deu la tai nguyen dung chung. */
static SemaphoreHandle_t s_cmd_mutex;

/* Sequence cua phan hoi TCP hien tai (do task nhan lenh dat). */
static uint8_t s_reply_mac[6];
static uint8_t s_reply_seq;
static volatile bool s_reply_active;

enum {
    CM02_REMOTE_IDLE,
    CM02_REMOTE_DEVICE,
    CM02_REMOTE_MODE,
    CM02_REMOTE_FILE,
};
static int s_remote_cm02_stage;
static int s_remote_cm02_device = -1;
static uint8_t s_remote_cm02_mac[6];

static void execute_command(const char *raw);

/* ------------------------------------------------------------------ */
/* Console UART1                                                       */
/* ------------------------------------------------------------------ */

static void console_init(void)
{
    const uart_config_t cfg = {
        .baud_rate = CON_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(CON_PORT, 1024, 1024, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(CON_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(CON_PORT, CON_TX_GPIO, CON_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

static void console_printf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) {
        return;
    }
    size_t len = (n < (int)sizeof(buf)) ? (size_t)n : sizeof(buf) - 1;
    uart_write_bytes(CON_PORT, buf, len);

    /* Neu dang phuc vu 1 lenh TCP thi moi dong in ra cung duoc gui ve
     * ESP32-S3 de hien thi tren console cua no. */
    if (s_reply_active) {
        char line[BT_LINK_TEXT_LEN];
        size_t copy = len < sizeof(line) - 1 ? len : sizeof(line) - 1;
        memcpy(line, buf, copy);
        line[copy] = '\0';
        while (copy > 0 && (line[copy - 1] == '\r' || line[copy - 1] == '\n')) {
            line[--copy] = '\0';
        }
        if (copy > 0) {
            bt_link_send_status(s_reply_mac, s_reply_seq, line);
            /* Pace status lines while a long reply is being streamed over TCP. */
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

static void console_flush_rx(void)
{
    uint8_t tmp[64];
    /* Doc bo byte dang nam trong buffer RX. Gioi han so vong lap: neu chan RX
     * co nhieu lien tuc thi van thoat ra, khong lam treo chuong trinh. */
    for (int i = 0; i < 32; i++) {
        if (uart_read_bytes(CON_PORT, tmp, sizeof(tmp), 0) <= 0) {
            break;
        }
    }
}

/* Ky tu duoc chap nhan trong dong lenh va cac tham so so. */
static bool is_command_char(uint8_t ch)
{
    if (ch >= '0' && ch <= '9') return true;
    if (ch >= 'a' && ch <= 'z') return true;
    if (ch >= 'A' && ch <= 'Z') return true;
    return ch == ' ' || ch == '-' || ch == '_' || ch == ':' || ch == '.';
}

/* Doc 1 dong tu COM6. Tra ve 1 khi co dong hop le, 0 khi het thoi gian cho.
 *
 * Nhieu dien tren chan RX sinh ra byte rac (ke ca backspace) lam sai lenh.
 * Moi byte khong nam trong tap ky tu lenh deu huy dong dang go va bat dau
 * lai tu dau -> nhieu khong the ghep vao lenh cua nguoi dung. */
static int console_read_line(char *out, size_t out_len, uint32_t timeout_ms)
{
    static char line[64];
    static size_t len;
    static bool escape_pending;
    static TickType_t last_digit_tick;

    TickType_t start = xTaskGetTickCount();
    uint8_t ch;

    /* Dong go do dang qua lau -> bo */
    if (len > 0 && (start - last_digit_tick) > pdMS_TO_TICKS(2000)) {
        len = 0;
        escape_pending = false;
    }

    while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(timeout_ms)) {
        if (uart_read_bytes(CON_PORT, &ch, 1, pdMS_TO_TICKS(20)) != 1) {
            continue;
        }
        if (ch == '\n') {
            goto finish_line;
        }
        if (ch == '\r') {
            continue;
        }
        if (ch == 0x7f || ch == '\b') {
            if (len > 0) {
                len--;
                uart_write_bytes(CON_PORT, "\b \b", 3);
            }
            continue;
        }

        /* Ho tro terminal gui chuoi "\\n" (2 ky tu) thay vi xuong dong. */
        if (escape_pending) {
            escape_pending = false;
            if (ch == 'n') {
                goto finish_line;
            }
        } else if (ch == '\\') {
            escape_pending = true;
            continue;
        }

        /* Ky tu la = nhieu/rac -> huy dong dang go */
        if (!is_command_char(ch)) {
            if (len > 0) {
                uart_write_bytes(CON_PORT, "\r\n", 2);
                len = 0;
            }
            continue;
        }

        uart_write_bytes(CON_PORT, (const char *)&ch, 1);   /* echo */
        if (len + 1 < sizeof(line)) {
            line[len++] = (char)ch;
        }
        last_digit_tick = xTaskGetTickCount();
    }
    return 0;

finish_line:
    uart_write_bytes(CON_PORT, "\r\n", 2);
    line[len] = '\0';
    len = 0;
    escape_pending = false;

    if (line[0] == '\0') {
        return 0;                           /* dong rong / chi co nhieu */
    }
    snprintf(out, out_len, "%s", line);
    return 1;
}

static bool line_is_digits(const char *s)
{
    if (*s == '\0') {
        return false;
    }
    for (; *s != '\0'; s++) {
        if (*s < '0' || *s > '9') {
            return false;
        }
    }
    return true;
}

/* Hoi 1 so nguyen trong khoang [min, max]. Dong rong = huy (tra ve -1). */
static int ask_int(const char *prompt, int min, int max)
{
    char line[32];

    console_printf("%s", prompt);

    for (;;) {
        if (console_read_line(line, sizeof(line), 1000) == 0) {
            continue;
        }

        char *p = line;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        size_t n = strlen(p);
        while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == '\t')) {
            p[--n] = '\0';
        }

        if (*p == '\0') {
            return -1;                          /* Enter = huy */
        }
        if (!line_is_digits(p)) {
            console_printf("Chi nhap so, nhap lai: ");
            continue;
        }

        int v = atoi(p);
        if (v < min || v > max) {
            console_printf("Gia tri '%s' khong hop le (chon %d..%d), nhap lai: ", p, min, max);
            continue;
        }
        return v;
    }
}

/* ------------------------------------------------------------------ */
/* SPIFFS                                                              */
/* ------------------------------------------------------------------ */

static void scan_mp3_files(void)
{
    s_file_count = 0;
    DIR *dir = opendir(MOUNT_POINT);
    if (dir == NULL) {
        return;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && s_file_count < MAX_FILES) {
        size_t n = strlen(ent->d_name);
        if (n > 4 && strcasecmp(ent->d_name + n - 4, ".mp3") == 0) {
            size_t copy = (n < FILE_NAME_LEN) ? n : (FILE_NAME_LEN - 1);
            memcpy(s_files[s_file_count], ent->d_name, copy);
            s_files[s_file_count][copy] = '\0';
            s_file_count++;
        }
    }
    closedir(dir);
}

static void print_files(void)
{
    if (s_file_count == 0) {
        console_printf("Khong co file .mp3 trong SPIFFS.\r\n");
        console_printf("Copy file vao data\\ roi chay: tools\\upload_fs.ps1\r\n");
        return;
    }
    console_printf("Danh sach file MP3:\r\n");
    for (int i = 0; i < s_file_count; i++) {
        char full[300];
        struct stat st;
        snprintf(full, sizeof(full), MOUNT_POINT "/%s", s_files[i]);
        long size = (stat(full, &st) == 0) ? (long)st.st_size : -1;
        console_printf("  %d) %-28s %ld byte\r\n", i, s_files[i], size);
    }
}

/* ------------------------------------------------------------------ */
/* Menu                                                                */
/* ------------------------------------------------------------------ */

static void print_menu(void)
{
    console_printf("\r\n===== ESP32 MP3 -> Bluetooth (A2DP Source) =====\r\n");
    console_printf("1) [cm01] Quet thiet bi Bluetooth\r\n");
    console_printf("2) [cm02] Chon loa + file MP3 local tren WROOM\r\n");
    console_printf("3) [cm03] Dung phat / ngat ket noi\r\n");
    console_printf("4) [cm04] Liet ke file MP3\r\n");
    console_printf("5) [cm05] Xem trang thai (status)\r\n");
    console_printf("6) [cm06] Lap lai bai vua phat\r\n");
    console_printf("7) [cm07] Chon bai khac (giu ket noi)\r\n");
    console_printf("8) [cm08] Tam dung / tiep tuc (pause/resume)\r\n");
    console_printf("9) [cm09] Danh sach lenh dieu khien\r\n");
    console_printf("0) [cm00] In lai menu\r\n");
    console_printf("Nhap cm00..cm09 hoac lenh chu: play 1 / pause / resume / next / prev / stop / list / scan / status\r\n");
    console_printf("Lenh: ");
}

static void print_control_help(void)
{
    console_printf("---- Lenh dieu khien (go tren UART hoac gui tu ESP32-S3 qua TCP) ----\r\n");
    console_printf("  cm01..cm09       ma menu dung chung voi ESP32-S3 (xem menu)\r\n");
    console_printf("  cm00             in menu (lenh 'menu' va 'music' khong duoc ho tro)\r\n");
    console_printf("  Khong nhap so menu don; dung cmNN tren UART va TCP\r\n");
    console_printf("  cm02             UART WROOM: phat MP3 local; UART S3: chon local/stream\r\n");
    console_printf("                   lenh noi bo: cm02 <ten loa> <ten file MP3>\r\n");
    console_printf("                       cm03 1 = dung phat, cm03 2 = ngat ket noi\r\n");
    console_printf("                       cm08 1 = pause, cm08 2 = resume\r\n");
    console_printf("  status           trang thai A2DP, bai dang phat, Wi-Fi/TCP\r\n");
    console_printf("  list             liet ke file .mp3 trong SPIFFS\r\n");
    console_printf("  devices          liet ke thiet bi Bluetooth da quet\r\n");
    console_printf("  scan             quet thiet bi Bluetooth (~13 giay)\r\n");
    console_printf("  connect <n|ten>  ket noi theo so thu tu hoac ten thiet bi\r\n");
    console_printf("  play [n]         phat file so n (khong co n = phat lai bai cu)\r\n");
    console_printf("                   tu ket noi A2DP neu chua ket noi\r\n");
    console_printf("  pause            tam dung, giu ket noi + vi tri bai\r\n");
    console_printf("  resume           tiep tuc phat\r\n");
    console_printf("  next / prev      bai tiep theo / bai truoc (giu ket noi)\r\n");
    console_printf("  replay           phat lai bai vua phat\r\n");
    console_printf("  stop             dung phat, GIU ket noi A2DP\r\n");
    console_printf("  disconnect       dung phat + ngat ket noi A2DP\r\n");
    console_printf("  selftest         test giai ma MP3 (khong phat ra loa)\r\n");
    console_printf("  ping             kiem tra duong Wi-Fi/TCP toi S3\r\n");
    console_printf("  help             in danh sach nay\r\n");
}

static void print_devices(void)
{
    int count = bt_a2dp_device_count();
    if (count == 0) {
        console_printf("Chua quet thay thiet bi nao. Chon 1 de quet.\r\n");
        return;
    }
    console_printf("Thiet bi tim thay:\r\n");
    for (int i = 0; i < count; i++) {
        const bt_dev_t *d = bt_a2dp_device(i);
        uint8_t major = (uint8_t)((d->cod & ESP_BT_COD_MAJOR_DEV_BIT_MASK) >> ESP_BT_COD_MAJOR_DEV_BIT_OFFSET);
        const char *tag = "khac";
        if (major == ESP_BT_COD_MAJOR_DEV_AV) {
            tag = "audio";
        } else if (major == ESP_BT_COD_MAJOR_DEV_PHONE) {
            tag = "dien thoai";
        }
        console_printf("  %d) [%-9s] %-24s %02x:%02x:%02x:%02x:%02x:%02x rssi %d\r\n",
                       i, tag, d->name,
                       d->bda[0], d->bda[1], d->bda[2], d->bda[3], d->bda[4], d->bda[5],
                       d->rssi);
    }
    console_printf("(chon thiet bi [audio] la loa/tai nghe Bluetooth)\r\n");
}

static void do_scan(void)
{
    if (bt_a2dp_is_connected() || bt_a2dp_is_streaming()) {
        console_printf("Dang ket noi/ phat nhac. Chon 3 de ngat truoc khi quet.\r\n");
        return;
    }

    console_printf("Dang quet Bluetooth (~13 giay), vui long doi...\r\n");
    esp_err_t ret = bt_a2dp_start_scan();
    if (ret != ESP_OK) {
        console_printf("Khong bat dau duoc qua trinh quet (%s).\r\n", esp_err_to_name(ret));
        return;
    }

    uint32_t waited = 0;
    while (bt_a2dp_is_scanning() && waited < 40000) {
        vTaskDelay(pdMS_TO_TICKS(200));
        waited += 200;
        if (waited % 2000 < 200) {
            console_printf(".");
        }
    }
    console_printf("\r\n");
    print_devices();
    if (bt_a2dp_scan_done()) {
        /* NVS commit can temporarily disable the flash cache. Pause TCP output
         * until the flash operation completes. */
        bt_link_pause_tx();
        esp_err_t save_ret = bt_a2dp_save_device_cache();
        bt_link_resume_tx();
        if (save_ret == ESP_OK) {
            console_printf("Da luu danh sach thiet bi tren WROOM.\r\n");
        } else {
            console_printf("Luu danh sach Bluetooth that bai: %s\r\n",
                           esp_err_to_name(save_ret));
        }
    } else {
        console_printf("Quet chua hoan tat; danh sach tam thoi chua duoc luu.\r\n");
    }
}

/* Ket noi A2DP toi thiet bi so `dev` (thu toi 3 lan). */
static bool connect_device(int dev)
{
    if (dev < 0 || dev >= bt_a2dp_device_count()) {
        console_printf("Thiet bi khong hop le. Hay gui 'devices' de xem so thu tu.\r\n");
        return false;
    }
    const bt_dev_t *d = bt_a2dp_device(dev);
    if (d == NULL) {
        console_printf("Thiet bi khong hop le.\r\n");
        return false;
    }

    mp3_player_stop();
    bt_a2dp_disconnect();
    bt_a2dp_cancel_scan();
    vTaskDelay(pdMS_TO_TICKS(500));

    bool ok = false;
    for (int attempt = 1; attempt <= 3 && !ok; attempt++) {
        console_printf("Ket noi A2DP toi '%s' [%02x:%02x:%02x:%02x:%02x:%02x] (lan %d/3)",
                       d->name,
                       d->bda[0], d->bda[1], d->bda[2], d->bda[3], d->bda[4], d->bda[5],
                       attempt);

        esp_err_t ret = bt_a2dp_connect(dev);
        if (ret != ESP_OK) {
            console_printf(" -> loi gui yeu cau: %s\r\n", esp_err_to_name(ret));
            break;
        }

        uint32_t waited = 0;
        while (waited < 15000) {
            bt_a2dp_state_t st = bt_a2dp_state();
            if (st == BT_A2DP_STATE_CONNECTED || st == BT_A2DP_STATE_STREAMING) {
                ok = true;
                break;
            }
            if (st == BT_A2DP_STATE_FAILED) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(250));
            waited += 250;
            if (waited % 1000 == 0) {
                console_printf(".");
            }
        }

        if (ok) {
            console_printf(" OK\r\n");
            s_last_device = dev;
            break;
        }

        console_printf("\r\n  -> that bai (%s)\r\n", bt_a2dp_state_str());
        bt_a2dp_disconnect();
        if (attempt < 3) {
            console_printf("  Thu lai sau 2 giay...\r\n");
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }

    if (!ok) {
        console_printf("KHONG KET NOI DUOC A2DP.\r\n");
        console_printf("Kiem tra:\r\n");
        console_printf(" - Loa da bat va dang o che do Bluetooth (khong phai AUX/USB)\r\n");
        console_printf(" - Loa CHUA ket noi voi dien thoai (tat Bluetooth dien thoai roi thu lai)\r\n");
        console_printf(" - Loa o gan board, roi gui 'scan' lai roi 'connect <n>'\r\n");
    }
    return ok;
}

static void do_play(void)
{
    if (bt_a2dp_device_count() <= 0) {
        console_printf("Chua co thiet bi. Hay chon 1 de quet truoc.\r\n");
        return;
    }

    print_devices();
    int dev = ask_int("Chon so thu tu thiet bi: ", 0, bt_a2dp_device_count() - 1);
    if (dev < 0) {
        console_printf("Da huy.\r\n");
        return;
    }

    const bt_dev_t *selected_device = bt_a2dp_device(dev);
    if (selected_device == NULL) {
        console_printf("Thiet bi khong hop le.\r\n");
        return;
    }

    char command[BT_LINK_TEXT_LEN];
    snprintf(command, sizeof(command), "cm02 %s", selected_device->name);
    execute_command(command);
    if (!bt_a2dp_is_connected() || s_last_device != dev) {
        return;
    }

    console_printf("UART WROOM chi phat MP3 local (nguon 1). De chon nguon 2/stream, hay gui cm02 tren UART S3.\r\n");
    if (s_file_count == 0) {
        return;
    }
    int f = ask_int("Chon so thu tu file MP3: ", 0, s_file_count - 1);
    if (f < 0) {
        console_printf("Da huy.\r\n");
        return;
    }

    snprintf(command, sizeof(command), "cm02 %s %s",
             selected_device->name, s_files[f]);
    execute_command(command);
}

static void do_stop(void)
{
    s_was_playing = false;
    mp3_player_stop();
    bt_a2dp_disconnect();
    console_printf("Da dung phat / ngat ket noi.\r\n");
}

/* Phat 1 file khi A2DP da ket noi san (khong ket noi lai). */
static void start_file(int f)
{
    if (f < 0 || f >= s_file_count) {
        console_printf("File khong hop le.\r\n");
        return;
    }

    char path[300];
    snprintf(path, sizeof(path), MOUNT_POINT "/%s", s_files[f]);

    bt_a2dp_suspend();                  /* dung stream cu trong khi doi bai */
    mp3_player_stop();
    if (mp3_player_start(path) != ESP_OK) {
        console_printf("Khong khoi dong duoc bo giai ma.\r\n");
        return;
    }
    bt_a2dp_resume();                   /* chi phat sau khi decoder da san sang */

    s_current_file = f;
    s_was_playing = true;
    console_printf("Dang phat: %s\r\n", s_files[f]);
}

/* Menu 3 tren UART WROOM: dung phat hay ngat ket noi. */
static void do_stop_menu(void)
{
    char line[8];
    console_printf("1) Dung phat (giu ket noi A2DP)\r\n");
    console_printf("2) Ngat ket noi A2DP\r\n");
    console_printf("Chon (Enter = huy): ");
    if (console_read_line(line, sizeof(line), 15000) != 1) {
        console_printf("\r\n");
        return;
    }
    int v = atoi(line);
    if (v == 2) {
        do_stop();
    } else if (v == 1) {
        s_was_playing = false;
        mp3_player_stop();
        bt_a2dp_suspend();
        console_printf("Da dung phat (van giu ket noi A2DP).\r\n");
    } else {
        console_printf("Da huy.\r\n");
    }
}

/* Menu 8 tren UART WROOM: pause hay resume. */
static void do_pause_menu(void)
{
    char line[8];
    console_printf("1) Pause   2) Resume\r\n");
    console_printf("Chon (Enter = huy): ");
    if (console_read_line(line, sizeof(line), 15000) != 1) {
        console_printf("\r\n");
        return;
    }
    int v = atoi(line);
    if (v == 1) {
        if (!bt_a2dp_is_connected()) {
            console_printf("Chua ket noi A2DP.\r\n");
            return;
        }
        mp3_player_pause();
        bt_a2dp_suspend();
        console_printf("Da pause (giu ket noi). Goi 'resume' de tiep tuc.\r\n");
    } else if (v == 2) {
        if (!bt_a2dp_is_connected()) {
            console_printf("Chua ket noi A2DP.\r\n");
            return;
        }
        if (!mp3_player_is_running() && s_current_file >= 0 &&
            s_current_file < s_file_count) {
            start_file(s_current_file);
        } else {
            mp3_player_resume();
            bt_a2dp_resume();
            console_printf("Da resume.\r\n");
        }
    } else {
        console_printf("Da huy.\r\n");
    }
}

/* Lap lai bai vua phat. */
static void do_replay(void)
{
    if (s_current_file < 0 || s_current_file >= s_file_count) {
        console_printf("Chua co bai nao duoc phat. Chon 2 truoc.\r\n");
        return;
    }
    if (!bt_a2dp_is_connected()) {
        console_printf("Chua ket noi A2DP. Chon 2 de ket noi va phat.\r\n");
        return;
    }

    start_file(s_current_file);
}

/* Chon bai khac trong khi van giu ket noi A2DP. */
static void do_choose_play(void)
{
    if (!bt_a2dp_is_connected()) {
        console_printf("Chua ket noi A2DP. Chon 2 de ket noi va phat.\r\n");
        return;
    }

    print_files();
    if (s_file_count == 0) {
        return;
    }
    int f = ask_int("Chon so thu tu file MP3: ", 0, s_file_count - 1);
    if (f < 0) {
        console_printf("Da huy.\r\n");
        return;
    }

    start_file(f);
}

static void do_selftest(void)
{
    print_files();
    if (s_file_count == 0) {
        return;
    }
    int f = ask_int("Chon so thu tu file MP3: ", 0, s_file_count - 1);
    if (f < 0) {
        console_printf("Da huy.\r\n");
        return;
    }

    char path[300];
    snprintf(path, sizeof(path), MOUNT_POINT "/%s", s_files[f]);

    console_printf("Dang giai ma thu '%s' (khong phat)...\r\n", s_files[f]);
    mp3_selftest_result_t r;
    esp_err_t ret = mp3_player_selftest(path, &r);
    if (ret != ESP_OK && !r.ok) {
        console_printf("Test that bai: %s\r\n", esp_err_to_name(ret));
        return;
    }

    console_printf("Ket qua: %lu frame, %lu loi\r\n",
                   (unsigned long)r.frames, (unsigned long)r.errors);
    console_printf("Dinh dang: %d Hz, %d kenh, %d kbps\r\n",
                   r.sample_rate, r.channels, r.bitrate_kbps);
    console_printf("PCM ra: %lu sample stereo (~%lu ms)\r\n",
                   (unsigned long)r.pcm_samples, (unsigned long)r.duration_ms);
    console_printf("RAM con lai: %u byte\r\n", (unsigned)esp_get_free_heap_size());
}

/* ------------------------------------------------------------------ */
/* Lenh dieu khien dung chung cho UART va TCP                          */
/* ------------------------------------------------------------------ */

static int pick_device(void)
{
    if (s_last_device >= 0 && s_last_device < bt_a2dp_device_count()) {
        return s_last_device;
    }
    for (int i = 0; i < bt_a2dp_device_count(); i++) {
        if (bt_a2dp_device_is_audio(i)) {
            return i;
        }
    }
    return bt_a2dp_device_count() > 0 ? 0 : -1;
}

/* Phat file `f`; tu ket noi A2DP (thiet bi dung gan nhat / loa dau tien). */
static void play_index(int f)
{
    if (s_file_count == 0) {
        console_printf("Khong co file .mp3 trong SPIFFS.\r\n");
        return;
    }
    if (f < 0 || f >= s_file_count) {
        console_printf("File %d khong hop le (0..%d). Goi 'list' de xem.\r\n",
                       f, s_file_count - 1);
        return;
    }

    if (!bt_a2dp_is_connected()) {
        int dev = pick_device();
        if (dev < 0) {
            console_printf("Chua co thiet bi Bluetooth. Goi 'scan' truoc.\r\n");
            return;
        }
        if (!connect_device(dev)) {
            return;
        }
    }

    start_file(f);
}

static void print_status(void)
{
    console_printf("A2DP: %s%s\r\n", bt_a2dp_state_str(),
                   bt_a2dp_is_suspended() ? " (pause)" : "");
    if (s_current_file >= 0 && s_current_file < s_file_count) {
        console_printf("Bai: %d/%d  %s\r\n", s_current_file, s_file_count - 1,
                       s_files[s_current_file]);
    } else {
        console_printf("Bai: chua co\r\n");
    }
    console_printf("Giai ma: %s\r\n",
                   mp3_player_is_paused() ? "pause"
                   : (mp3_player_is_running() ? "dang chay" : "dung"));
    console_printf("Thiet bi BT: %d (dung: %d)\r\n",
                   bt_a2dp_device_count(), s_last_device);
    console_printf("File MP3: %d\r\n", s_file_count);
    console_printf("Wi-Fi/TCP: %s (AP %s), heap %u\r\n",
                   bt_link_is_connected() ? "da ket noi" :
                   (bt_link_is_ready() ? "dang cho ket noi" : "chua san sang"),
                   BT_LINK_AP_SSID,
                   (unsigned)esp_get_free_heap_size());
}

static bool parse_int_arg(const char *arg, int *out)
{
    if (arg == NULL || *arg == '\0') {
        return false;
    }
    errno = 0;
    char *end = NULL;
    long v = strtol(arg, &end, 10);
    if (end == arg || errno == ERANGE) {
        return false;
    }
    while (*end == ' ' || *end == '\t') {
        end++;
    }
    if (*end != '\0' || v < INT_MIN || v > INT_MAX) {
        return false;
    }
    *out = (int)v;
    return true;
}

static int find_device_by_name(const char *name)
{
    if (name == NULL || *name == '\0') {
        return -1;
    }

    for (int i = 0; i < bt_a2dp_device_count(); i++) {
        const bt_dev_t *d = bt_a2dp_device(i);
        if (d != NULL && strcasecmp(name, d->name) == 0) {
            return i;
        }
    }
    return -1;
}

static int find_file_by_name(const char *name)
{
    if (name == NULL || *name == '\0') {
        return -1;
    }

    size_t name_len = strlen(name);
    bool has_extension = name_len >= 4 &&
                         strcasecmp(name + name_len - 4, ".mp3") == 0;
    for (int i = 0; i < s_file_count; i++) {
        if (strcasecmp(name, s_files[i]) == 0) {
            return i;
        }
    }

    if (!has_extension) {
        for (int i = 0; i < s_file_count; i++) {
            size_t file_len = strlen(s_files[i]);
            if (file_len > 4 && strlen(name) == file_len - 4 &&
                strncasecmp(name, s_files[i], file_len - 4) == 0 &&
                name[file_len - 4] == '\0') {
                return i;
            }
        }
    }
    return -1;
}

/* Parse `cm02 <device index|name> [file index|name]`. */
static bool parse_cm02_args(const char *arg, int *device, int *file)
{
    char input[BT_LINK_TEXT_LEN];
    snprintf(input, sizeof(input), "%s", arg != NULL ? arg : "");

    char *start = input;
    while (*start == ' ' || *start == '\t') {
        start++;
    }
    size_t len = strlen(start);
    while (len > 0 && (start[len - 1] == ' ' || start[len - 1] == '\t')) {
        start[--len] = '\0';
    }
    if (*start == '\0') {
        return false;
    }

    *device = -1;
    *file = -1;

    char *device_end = start;
    int index = -1;
    while (*device_end != '\0' && *device_end != ' ' && *device_end != '\t') {
        device_end++;
    }
    if (*device_end != '\0') {
        char saved = *device_end;
        *device_end = '\0';
        bool numeric = parse_int_arg(start, &index);
        *device_end = saved;
        if (numeric) {
            *device = index;
        }
    } else if (parse_int_arg(start, &index)) {
        *device = index;
    }

    char *file_arg = NULL;
    if (*device < 0) {
        size_t best_len = 0;
        int best_index = -1;
        int count = bt_a2dp_device_count();
        for (int i = 0; i < count; i++) {
            const bt_dev_t *d = bt_a2dp_device(i);
            if (d == NULL) {
                continue;
            }
            size_t name_len = strlen(d->name);
            if (name_len <= best_len || strncasecmp(start, d->name, name_len) != 0 ||
                (start[name_len] != '\0' && start[name_len] != ' ' &&
                 start[name_len] != '\t')) {
                continue;
            }
            best_len = name_len;
            best_index = i;
        }
        if (best_index < 0) {
            return false;
        }
        *device = best_index;
        file_arg = start + best_len;
    } else {
        file_arg = device_end;
    }

    while (*file_arg == ' ' || *file_arg == '\t') {
        file_arg++;
    }
    if (*file_arg == '\0') {
        return true;
    }
    if (parse_int_arg(file_arg, file)) {
        return *file >= 0;
    }

    *file = find_file_by_name(file_arg);
    return *file >= 0;
}

static int parse_file_selection(const char *selection)
{
    if (selection == NULL) {
        return -1;
    }
    while (*selection == ' ' || *selection == '\t') {
        selection++;
    }
    if (strncasecmp(selection, "cm02", 4) == 0 &&
        (selection[4] == ' ' || selection[4] == '\t')) {
        selection += 4;
        while (*selection == ' ' || *selection == '\t') {
            selection++;
        }
    }

    int file = -1;
    if (parse_int_arg(selection, &file)) {
        return file >= 0 && file < s_file_count ? file : -1;
    }
    return find_file_by_name(selection);
}

static bool execute_cm02(const char *arg)
{
    int device = -1;
    int file = -1;
    if (!parse_cm02_args(arg, &device, &file)) {
        console_printf("Dung: cm02 <ten thiet bi> [ten bai hat]\r\n");
        return false;
    }

    if (device < 0 || device >= bt_a2dp_device_count()) {
        console_printf("Thiet bi khong hop le. Hay gui 'devices' de xem so thu tu.\r\n");
        return false;
    }
    const bt_dev_t *d = bt_a2dp_device(device);
    if (d == NULL) {
        console_printf("Thiet bi khong hop le.\r\n");
        return false;
    }
    if (file >= s_file_count) {
        console_printf("Bai hat khong hop le. Hay chon ten bai trong danh sach.\r\n");
        return false;
    }

    if (!bt_a2dp_is_connected() || s_last_device != device) {
        if (!connect_device(device)) {
            console_printf("Ket noi '%s' that bai.\r\n", d->name);
            return false;
        }
    }
    console_printf("Da ket noi '%s'.\r\n", d->name);

    if (file < 0) {
        if (!s_reply_active) {
            print_files();
        }
        return true;
    }

    start_file(file);
    return true;
}

static void send_cm02_event(const char *event)
{
    if (!s_reply_active || event == NULL) {
        return;
    }

    char message[48];
    snprintf(message, sizeof(message), "cm02-event:%s", event);
    bt_link_send_status(s_reply_mac, s_reply_seq, message);
}

static void execute_remote_cm02(const char *arg)
{
    bool waiting_for_device = s_remote_cm02_stage == CM02_REMOTE_DEVICE &&
                              memcmp(s_remote_cm02_mac, s_reply_mac, sizeof(s_reply_mac)) == 0;
    bool waiting_for_mode = s_remote_cm02_stage == CM02_REMOTE_MODE &&
                            memcmp(s_remote_cm02_mac, s_reply_mac, sizeof(s_reply_mac)) == 0;
    bool waiting_for_file = s_remote_cm02_stage == CM02_REMOTE_FILE &&
                            memcmp(s_remote_cm02_mac, s_reply_mac, sizeof(s_reply_mac)) == 0;

    if (arg == NULL || *arg == '\0') {
        s_remote_cm02_stage = CM02_REMOTE_DEVICE;
        s_remote_cm02_device = -1;
        memcpy(s_remote_cm02_mac, s_reply_mac, sizeof(s_remote_cm02_mac));
        if (bt_a2dp_device_count() == 0) {
            s_remote_cm02_stage = CM02_REMOTE_IDLE;
            console_printf("Chua co thiet bi. Gui cm01 de quet Bluetooth truoc.\r\n");
            send_cm02_event("devices-empty");
            return;
        }
        print_devices();
        console_printf("Chon so thiet bi hoac gui 'cm02 <ten thiet bi>'.\r\n");
        send_cm02_event("devices-ready");
        return;
    }

    if (arg != NULL && strcasecmp(arg, "cancel") == 0) {
        s_remote_cm02_stage = CM02_REMOTE_IDLE;
        return;
    }

    if (waiting_for_mode) {
        if (arg != NULL &&
            (strcasecmp(arg, "local") == 0 || strcmp(arg, "1") == 0)) {
            if (s_file_count == 0) {
                s_remote_cm02_stage = CM02_REMOTE_IDLE;
                console_printf("Khong co bai MP3 trong SPIFFS tren WROOM.\r\n");
                send_cm02_event("files-empty");
                return;
            }
            s_remote_cm02_stage = CM02_REMOTE_FILE;
            print_files();
            console_printf("Chon ten/so bai trong danh sach.\r\n");
            send_cm02_event("files-ready");
            return;
        }
        if (arg != NULL &&
            (strcasecmp(arg, "stream") == 0 || strcmp(arg, "2") == 0)) {
            s_current_file = -1;
            s_was_playing = false;
            s_remote_cm02_stage = CM02_REMOTE_IDLE;
            console_printf("WROOM san sang nhan PCM stream tu S3.\r\n");
            send_cm02_event("stream-ready");
            return;
        }
        s_remote_cm02_stage = CM02_REMOTE_IDLE;
        console_printf("Nguon phat khong hop le. Gui 'cm02 local' hoac 'cm02 stream'.\r\n");
        send_cm02_event("connect-failed");
        return;
    }

    if (waiting_for_file) {
        int file = parse_file_selection(arg);
        if (file < 0) {
            s_remote_cm02_stage = CM02_REMOTE_IDLE;
            console_printf("Ten/so bai khong hop le. Gui cm02 de bat dau lai.\r\n");
            send_cm02_event("connect-failed");
            return;
        }
        const bt_dev_t *d = bt_a2dp_device(s_remote_cm02_device);
        if (d == NULL || !bt_a2dp_is_connected() ||
            s_last_device != s_remote_cm02_device) {
            s_remote_cm02_stage = CM02_REMOTE_IDLE;
            console_printf("Ket noi loa da mat. Gui cm02 de chon lai thiet bi.\r\n");
            send_cm02_event("connect-failed");
            return;
        }
        char command[BT_LINK_TEXT_LEN];
        snprintf(command, sizeof(command), "cm02 %s %s", d->name, s_files[file]);
        s_remote_cm02_stage = CM02_REMOTE_IDLE;
        execute_cm02(command + 5);
        return;
    }

    int device = -1;
    int file = -1;
    if (!parse_cm02_args(arg, &device, &file)) {
        s_remote_cm02_stage = CM02_REMOTE_IDLE;
        console_printf("Khong nhan dang duoc lua chon. Gui cm02 de bat dau lai.\r\n");
        send_cm02_event("connect-failed");
        return;
    }

    if (waiting_for_device) {
        const bt_dev_t *d = bt_a2dp_device(device);
        if (d == NULL) {
            s_remote_cm02_stage = CM02_REMOTE_IDLE;
            console_printf("Thiet bi khong hop le. Gui cm02 de chon lai.\r\n");
            send_cm02_event("connect-failed");
            return;
        }
        if (file >= 0) {
            if (file >= s_file_count) {
                s_remote_cm02_stage = CM02_REMOTE_IDLE;
                console_printf("Bai hat khong hop le.\r\n");
                send_cm02_event("connect-failed");
                return;
            }
            char command[BT_LINK_TEXT_LEN];
            snprintf(command, sizeof(command), "cm02 %s %s", d->name, s_files[file]);
            s_remote_cm02_stage = CM02_REMOTE_IDLE;
            execute_cm02(command + 5);
            return;
        }
        char command[BT_LINK_TEXT_LEN];
        snprintf(command, sizeof(command), "cm02 %s", d->name);
        s_remote_cm02_stage = CM02_REMOTE_IDLE;
        if (!execute_cm02(command + 5)) {
            send_cm02_event("connect-failed");
            return;
        }
        if (!bt_a2dp_is_connected() || s_last_device != device) {
            send_cm02_event("connect-failed");
            return;
        }
        s_remote_cm02_device = device;
        s_remote_cm02_stage = CM02_REMOTE_MODE;
        memcpy(s_remote_cm02_mac, s_reply_mac, sizeof(s_remote_cm02_mac));
        console_printf("Chon nguon phat: 1) local (cm02 local), 2) stream (cm02 stream).\r\n");
        send_cm02_event("playback-ready");
        return;
    }

    s_remote_cm02_stage = CM02_REMOTE_IDLE;
    execute_cm02(arg);
}

static void execute_command(const char *raw)
{
    char line[BT_LINK_TEXT_LEN];
    snprintf(line, sizeof(line), "%s", raw != NULL ? raw : "");

    char *p = line;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    size_t n = strlen(p);
    while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == '\t')) {
        p[--n] = '\0';
    }
    if (*p == '\0') {
        return;
    }

    char *arg = strchr(p, ' ');
    if (arg != NULL) {
        *arg++ = '\0';
        while (*arg == ' ' || *arg == '\t') {
            arg++;
        }
    }

    for (char *c = p; *c != '\0'; c++) {
        *c = (char)tolower((unsigned char)*c);
    }

    if (line_is_digits(p)) {
        console_printf("So menu don khong duoc ho tro; hay dung cm00..cm09 hoac lenh chu.\r\n");
        return;
    }

    if (strcmp(p, "cm02") != 0) {
        s_remote_cm02_stage = CM02_REMOTE_IDLE;
    }

    /* Ma menu dung chung voi ESP32-S3: cm00..cm09 (xem print_menu).
     * Tham so (neu co) nam trong `arg`:
     *   cm02 <thiet bi> [file]   cm03 1|2   cm07 <file>   cm08 1|2 */
    if (p[0] == 'c' && p[1] == 'm' &&
        isdigit((unsigned char)p[2]) && isdigit((unsigned char)p[3]) &&
        p[4] == '\0') {
        int code = (p[2] - '0') * 10 + (p[3] - '0');
        int v1 = -1;
        if (arg != NULL && *arg != '\0') {
            char *end = NULL;
            v1 = (int)strtol(arg, &end, 10);
            if (end == arg) {
                v1 = -1;
            }
        }

        switch (code) {
        case 0:
            print_menu();
            break;
        case 1:
            do_scan();
            break;
        case 2:
            if (s_reply_active) {
                execute_remote_cm02(arg);
                break;
            }
            s_remote_cm02_stage = CM02_REMOTE_IDLE;
            if (arg == NULL || *arg == '\0') {
                do_play();
                break;
            }
            execute_cm02(arg);
            break;
        case 3:
            if (v1 == 2) {
                do_stop();
            } else if (v1 == 1) {
                s_was_playing = false;
                mp3_player_stop();
                bt_a2dp_suspend();
                console_printf("Da dung phat (van giu ket noi A2DP).\r\n");
            } else if ((arg == NULL || *arg == '\0') && !s_reply_active) {
                do_stop_menu();
            } else {
                console_printf("Dung: cm03 1|2 (1=dung phat, 2=ngat ket noi)\r\n");
            }
            break;
        case 4:
            print_files();
            break;
        case 5:
            print_status();
            break;
        case 6:
            do_replay();
            break;
        case 7:
            if ((arg == NULL || *arg == '\0') && !s_reply_active) {
                do_choose_play();
            } else if (v1 < 0) {
                console_printf("Dung: cm07 <file> (xem 'list')\r\n");
            } else {
                play_index(v1);
            }
            break;
        case 8:
            if (v1 == 1) {
                execute_command("pause");
            } else if (v1 == 2) {
                execute_command("resume");
            } else if ((arg == NULL || *arg == '\0') && !s_reply_active) {
                do_pause_menu();
            } else {
                console_printf("Dung: cm08 1|2 (1=pause, 2=resume)\r\n");
            }
            break;
        case 9:
            print_control_help();
            break;
        default:
            console_printf("Ma menu '%s' khong hop le (cm00..cm09).\r\n", p);
            break;
        }
        return;
    }

    if (strcmp(p, "help") == 0 || strcmp(p, "?") == 0) {
        print_control_help();
        return;
    }
    if (strcmp(p, "status") == 0 || strcmp(p, "state") == 0) {
        print_status();
        return;
    }
    if (strcmp(p, "list") == 0 || strcmp(p, "files") == 0) {
        print_files();
        return;
    }
    if (strcmp(p, "devices") == 0 || strcmp(p, "devs") == 0) {
        print_devices();
        return;
    }
    if (strcmp(p, "scan") == 0) {
        do_scan();
        return;
    }
    if (strcmp(p, "selftest") == 0 || strcmp(p, "test") == 0) {
        do_selftest();
        return;
    }
    if (strcmp(p, "ping") == 0) {
        console_printf("pong Wi-Fi/TCP connected=%d heap=%u\r\n",
                       (int)bt_link_is_connected(),
                       (unsigned)esp_get_free_heap_size());
        return;
    }
    if (strcmp(p, "connect") == 0) {
        int idx = 0;
        if (!parse_int_arg(arg, &idx)) {
            idx = find_device_by_name(arg);
            if (idx < 0) {
                console_printf("Dung: connect <so thu tu|ten thiet bi> (xem 'devices')\r\n");
                return;
            }
        }
        if (connect_device(idx)) {
            console_printf("Da ket noi thiet bi %d.\r\n", idx);
        }
        return;
    }
    if (strcmp(p, "play") == 0) {
        int idx = 0;
        if (arg == NULL || *arg == '\0') {
            idx = (s_current_file >= 0) ? s_current_file : 0;
        } else if (!parse_int_arg(arg, &idx)) {
            console_printf("Dung: play [so thu tu file] (xem 'list')\r\n");
            return;
        }
        play_index(idx);
        return;
    }
    if (strcmp(p, "replay") == 0) {
        if (s_current_file < 0) {
            console_printf("Chua co bai nao duoc phat.\r\n");
            return;
        }
        play_index(s_current_file);
        return;
    }
    if (strcmp(p, "pause") == 0) {
        if (!bt_a2dp_is_connected()) {
            console_printf("Chua ket noi A2DP.\r\n");
            return;
        }
        bool stream_active = mp3_player_is_streaming();
        mp3_player_pause();
        bt_a2dp_suspend();
        if (stream_active) {
            bt_link_broadcast_status("stream:pause");
        }
        console_printf("Da pause (giu ket noi). Goi 'resume' de tiep tuc.\r\n");
        return;
    }
    if (strcmp(p, "resume") == 0 || strcmp(p, "continue") == 0) {
        if (!bt_a2dp_is_connected()) {
            console_printf("Chua ket noi A2DP.\r\n");
            return;
        }
        if (!mp3_player_is_running() && s_current_file >= 0 &&
            s_current_file < s_file_count) {
            start_file(s_current_file);         /* bai da phat xong / bi stop */
        } else {
            bool stream_active = mp3_player_is_streaming();
            mp3_player_resume();
            bt_a2dp_resume();
            if (stream_active) {
                bt_link_broadcast_status("stream:resume");
            }
            console_printf("Da resume.\r\n");
        }
        return;
    }
    if (strcmp(p, "stop") == 0) {
        s_was_playing = false;
        mp3_player_stop();
        bt_a2dp_suspend();
        console_printf("Da dung phat (van giu ket noi A2DP).\r\n");
        return;
    }
    if (strcmp(p, "disconnect") == 0 || strcmp(p, "off") == 0) {
        do_stop();
        return;
    }
    if (strcmp(p, "next") == 0 || strcmp(p, "prev") == 0 ||
        strcmp(p, "previous") == 0) {
        if (!bt_a2dp_is_connected()) {
            console_printf("Chua ket noi A2DP.\r\n");
            return;
        }
        if (s_file_count == 0) {
            console_printf("Khong co file .mp3 trong SPIFFS.\r\n");
            return;
        }
        int delta = (strcmp(p, "next") == 0) ? 1 : -1;
        int base = (s_current_file >= 0) ? s_current_file : 0;
        int f = (base + delta + s_file_count) % s_file_count;
        start_file(f);
        return;
    }
    if (strcmp(p, "menu") == 0 || strcmp(p, "music") == 0) {
        console_printf("Lenh '%s' khong duoc ho tro. Gui cm00 hoac chon 0 de in menu.\r\n", p);
        return;
    }

    console_printf("Lenh '%s' khong hop le. Goi 'help' de xem danh sach.\r\n", p);
}

/* Callback chay trong task bt_link khi co lenh TCP tu ESP32-S3. */
static void on_remote_command(const char *command, const uint8_t src[6], uint8_t seq)
{
    ESP_LOGI(TAG, "lenh TCP: %s", command);

    if (line_is_digits(command)) {
        bt_link_send_status(src, seq,
                            "tu choi so don qua TCP; dung ma cm00..cm09 hoac lenh chu");
        return;
    }

    /* ACK ngay truoc khi chay lenh: scan mat ~13-40 giay, connect co the thu
     * 3 lan. Neu khong ACK som, S3 se bao "khong nhan duoc phan hoi" trong
     * luc WROOM van dang lam viec binh thuong. */
    char ack[BT_LINK_TEXT_LEN];
    snprintf(ack, sizeof(ack), "da nhan lenh: %s", command);
    bt_link_send_status(src, seq, ack);

    if (s_cmd_mutex != NULL) {
        xSemaphoreTake(s_cmd_mutex, portMAX_DELAY);
    }
    memcpy(s_reply_mac, src, 6);
    s_reply_seq = seq;
    s_reply_active = true;

    execute_command(command);

    s_reply_active = false;
    if (s_cmd_mutex != NULL) {
        xSemaphoreGive(s_cmd_mutex);
    }
}

/* ------------------------------------------------------------------ */

void app_main(void)
{
    console_init();
    vTaskDelay(pdMS_TO_TICKS(200));
    console_flush_rx();

    s_cmd_mutex = xSemaphoreCreateMutex();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        console_printf("NVS init that bai: %s\r\n", esp_err_to_name(ret));
        return;
    }

    esp_vfs_spiffs_conf_t conf = {
        .base_path = MOUNT_POINT,
        .partition_label = PARTITION_LABEL,
        .max_files = 5,
        .format_if_mount_failed = true,
    };
    ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        console_printf("Mount SPIFFS that bai (%s). Nap filesystem: tools\\upload_fs.ps1\r\n",
                       esp_err_to_name(ret));
        return;
    }

    size_t total = 0, used = 0;
    if (esp_spiffs_info(PARTITION_LABEL, &total, &used) == ESP_OK) {
        console_printf("SPIFFS: %u/%u byte da dung\r\n", (unsigned)used, (unsigned)total);
    }
    scan_mp3_files();

    /* Tao task giai ma + ring buffer truoc khi Bluetooth chiem heap. */
    if (mp3_player_init() != ESP_OK) {
        console_printf("Khoi tao bo giai ma that bai (het RAM).\r\n");
    }

    console_printf("Khoi tao Bluetooth...\r\n");
    ret = bt_a2dp_init();
    if (ret != ESP_OK) {
        console_printf("Khoi tao Bluetooth that bai: %s\r\n", esp_err_to_name(ret));
        return;
    }
    ret = bt_a2dp_load_device_cache();
    if (ret == ESP_OK) {
        console_printf("Da nap danh sach Bluetooth da luu (%d thiet bi).\r\n",
                       bt_a2dp_device_count());
    } else if (ret == ESP_ERR_NVS_NOT_FOUND) {
        console_printf("Chua co danh sach Bluetooth da luu; gui cm01 de quet.\r\n");
    } else {
        console_printf("Nap danh sach Bluetooth da luu that bai: %s\r\n",
                       esp_err_to_name(ret));
    }
    console_printf("Bluetooth san sang (ten: \"%s\")\r\n", "ESP32_MP3_Speaker");

    /* Wi-Fi STA client joins the S3 SoftAP and opens a TCP control link. */
    console_printf("Ket noi Wi-Fi AP %s cua ESP32-S3...\r\n", BT_LINK_AP_SSID);
    bt_link_set_command_handler(on_remote_command);
    ret = bt_link_init();
    if (ret == ESP_OK) {
        console_printf("Wi-Fi/TCP dang ket noi S3 (port %u).\r\n",
                       (unsigned)BT_LINK_TCP_PORT);
    } else {
        console_printf("Wi-Fi/TCP init that bai (%s) - chi dieu khien duoc qua UART.\r\n",
                       esp_err_to_name(ret));
    }

    console_printf("RAM trong: %u byte\r\n", (unsigned)esp_get_free_heap_size());
    print_files();
    print_menu();
    console_flush_rx();     /* bo nhieu sinh ra trong luc khoi tao Bluetooth */

    char line[64];
    while (1) {
        if (console_read_line(line, sizeof(line), 200) == 1) {
            char *p = line;
            while (*p == ' ' || *p == '\t') {
                p++;
            }
            if (*p == '\0') {
                console_printf("Lenh: ");
                continue;
            }

            bool menu_only = strcasecmp(p, "cm00") == 0;

            if (s_cmd_mutex != NULL) {
                xSemaphoreTake(s_cmd_mutex, portMAX_DELAY);
            }
            execute_command(p);
            if (s_cmd_mutex != NULL) {
                xSemaphoreGive(s_cmd_mutex);
            }

            if (menu_only) {
                continue;
            }
            console_printf("[trang thai: %s]\r\n", bt_a2dp_state_str());
            console_printf("Lenh: ");
        }

        /* Mat ket noi thi dung bo giai ma (chi khi da ngat han, tranh dung som
         * luc A2DP vua bat stream) */
        bool running = mp3_player_is_running();
        if (running && !bt_a2dp_is_connected()) {
            s_was_playing = false;
            mp3_player_stop();
            console_printf("\r\n[mat ket noi Bluetooth - da dung phat]\r\nLenh: ");
        } else if (!running && s_was_playing) {
            s_was_playing = false;
            bt_a2dp_suspend();
            if (bt_a2dp_is_connected() && s_current_file >= 0 &&
                s_current_file < s_file_count) {
                console_printf("\r\n[Da phat xong: %s]\r\n", s_files[s_current_file]);
                console_printf("  cm06 Lap lai   cm07 Chon bai khac   cm03 Dung phat\r\nLenh: ");
            }
        }
    }
}
