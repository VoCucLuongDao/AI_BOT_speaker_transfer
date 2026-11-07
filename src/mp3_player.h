#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Tao task giai ma + ring buffer 1 lan luc khoi dong (nen goi truoc khi init
 * Bluetooth de tranh phan manh heap). Goi lai nhieu lan khong sao. */
esp_err_t mp3_player_init(void);

/* Bat dau giai ma file mp3 va day PCM (44.1 kHz, 16-bit, stereo) vao ring buffer.
 * Tra ve ESP_OK neu task giai ma duoc tao. */
esp_err_t mp3_player_start(const char *path);

/* Dung phat, giai phong task. */
void mp3_player_stop(void);

/* Doc PCM cho A2DP data callback (khong block). Tra ve so byte doc duoc (0..len). */
int32_t mp3_player_read(void *buf, int32_t len);

bool mp3_player_is_running(void);

/* Receive decoded 44.1 kHz, 16-bit stereo PCM from the S3 over TCP. */
esp_err_t mp3_player_stream_start(void);
bool mp3_player_stream_write(const void *data, size_t length);
void mp3_player_stream_end(void);
void mp3_player_stream_stop(void);
bool mp3_player_is_streaming(void);

/* Tam dung / tiep tuc.  Vi tri trong file duoc giu nguyen, A2DP gui im lang
 * trong luc pause (data callback van duoc goi). */
void mp3_player_pause(void);
void mp3_player_resume(void);
bool mp3_player_is_paused(void);

/* Ket qua test giai ma (khong phat ra Bluetooth). */
typedef struct {
    bool ok;
    uint32_t frames;
    uint32_t errors;
    uint32_t pcm_samples;       /* sample stereo 44100 Hz sau resample */
    uint32_t duration_ms;
    int sample_rate;
    int channels;
    int bitrate_kbps;
} mp3_selftest_result_t;

/* Giai ma toan bo file de kiem tra bo giai ma Helix (khong can Bluetooth). */
esp_err_t mp3_player_selftest(const char *path, mp3_selftest_result_t *out);
