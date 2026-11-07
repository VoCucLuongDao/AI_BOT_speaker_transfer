#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define UART_PORT       UART_NUM_1
#define UART_TX_GPIO    17
#define UART_RX_GPIO    16
#define UART_BAUD_RATE  115200
#define UART_BUFFER_SIZE 256

/* Do dai toi da cua mot dong (khong tinh ky tu ket thuc) */
#define LINE_MAX_LEN    128

/* Bat len 1 de gui "Hello from ESP32 #n" moi 2 giay (de kiem tra chieu ESP32 -> PC) */
#define ENABLE_HEARTBEAT 0

static const char *TAG = "uart1_test";

static void handle_line(const char *line, size_t len)
{
    ESP_LOGI(TAG, "Line (%u bytes): %s", (unsigned)len, line);

    /* Echo lai nguyen dong kem ket thuc dong de ben PC biet da xu ly xong */
    uart_write_bytes(UART_PORT, line, len);
    uart_write_bytes(UART_PORT, "\r\n", 2);
}

void app_main(void)
{
    const uart_config_t uart_config = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, UART_BUFFER_SIZE * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, UART_TX_GPIO, UART_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    ESP_LOGI(TAG, "UART1 ready: TX=GPIO%d RX=GPIO%d @%d", UART_TX_GPIO, UART_RX_GPIO,
             UART_BAUD_RATE);

    uint8_t chunk[64];
    char line[LINE_MAX_LEN + 1];
    size_t line_len = 0;
    bool escape_pending = false;   /* dang cho ky tu sau dau '\' */

    uart_write_bytes(UART_PORT, "Hello World!\n", 14);

    while (1) {
        int bytes_read = uart_read_bytes(UART_PORT, chunk, sizeof(chunk), pdMS_TO_TICKS(20));

        for (int i = 0; i < bytes_read; i++) {
            char ch = (char)chunk[i];

            /* Bo qua byte rac / ky tu dieu khien khi mo cong (CH340 hay ban vai byte luc mo port) */
            if ((unsigned char)ch < 0x20 && ch != '\n' && ch != '\r') {
                continue;
            }
            if ((unsigned char)ch == 0x7f) {
                continue;
            }

            if (escape_pending) {
                escape_pending = false;
                switch (ch) {
                case 'n':   /* "\n" literal -> ket thuc dong */
                    line[line_len] = '\0';
                    handle_line(line, line_len);
                    line_len = 0;
                    break;
                case 'r':   /* "\r" literal -> bo qua */
                    break;
                case 't':
                    if (line_len < LINE_MAX_LEN) line[line_len++] = '\t';
                    break;
                case '\\':
                    if (line_len < LINE_MAX_LEN) line[line_len++] = '\\';
                    break;
                default:
                    if (line_len < LINE_MAX_LEN) line[line_len++] = '\\';
                    if (line_len < LINE_MAX_LEN) line[line_len++] = ch;
                    break;
                }
                continue;
            }

            if (ch == '\\') {
                escape_pending = true;
            } else if (ch == '\n') {            /* newline that */
                line[line_len] = '\0';
                handle_line(line, line_len);
                line_len = 0;
            } else if (ch == '\r') {           /* CR that -> bo qua */
                continue;
            } else if (line_len < LINE_MAX_LEN) {
                line[line_len++] = ch;
            } else {
                /* Dong qua dai: bo va cho lai tu dau dong */
                line_len = 0;
                ESP_LOGW(TAG, "Line too long, dropped");
            }
        }
    }
}
