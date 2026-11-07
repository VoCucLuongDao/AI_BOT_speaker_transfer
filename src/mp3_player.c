/*
 * Giai ma MP3 tu SPIFFS -> PCM 44100 Hz / 16-bit / stereo trong ring buffer.
 * A2DP source data callback se doc tu day.
 *
 * Bo giai ma: Helix (component esp-libhelix-mp3).
 *
 * Task giai ma duoc tao 1 lan luc khoi dong va giu nguyen (khong tao huy moi
 * lan phat) de tranh loi cap phat stack khi heap da bi phan manh boi A2DP.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "mp3_player.h"
#include "mp3dec.h"

#define TAG                 "mp3"
#define OUT_HZ              44100
#define OUT_CH              2
#define RING_BYTES          (8 * 1024)      /* ~46 ms PCM stereo; S3 lead 500 ms se lap day */
#define READ_CHUNK          1024
#define MAX_FRAME_BYTES     2048            /* > frame MP3 lon nhat (1441 byte) */
#define IN_CAP              (READ_CHUNK + MAX_FRAME_BYTES)
#define MAX_SRC_SAMPLES     1152            /* 1 frame MP3: toi da 1152 sample/kenh */
#define OUT_SAMPLES         1280            /* du cho 1152 sample @ 48 kHz -> 44.1 kHz */
#define PLAYER_TASK_STACK   (8 * 1024)

typedef struct {
    uint32_t frames;
    uint32_t errors;
    uint32_t pcm_samples;       /* so sample stereo sau resample */
    int sample_rate;
    int channels;
    int bitrate_kbps;
} decode_stats_t;

static StreamBufferHandle_t s_ring;
static StaticStreamBuffer_t s_ring_struct;
static uint8_t s_ring_storage[RING_BYTES];
static TaskHandle_t s_task;
static SemaphoreHandle_t s_done;        /* task bao da giai ma xong */
static volatile bool s_running;
static volatile bool s_stream_mode;
static volatile bool s_paused;          /* tam dung, giu nguyen vi tri file */
static char s_path[300];

/* Bo dem dung chung, cap static de khong phu thuoc malloc luc dang phat. */
static uint8_t s_in[IN_CAP];
static int16_t s_pcm[MAX_SRC_SAMPLES * 2];
static int16_t s_out[OUT_SAMPLES * OUT_CH];

/* Bo resample tuyen tinh, giu trang thai giua cac frame de khong bi dut */
static int16_t s_last[OUT_CH];
static double s_t;

static void player_task(void *arg);

static inline int16_t sample_at(const int16_t *pcm, int idx, int ch, int channels)
{
    if (idx < 0) {
        return 0;
    }
    if (ch >= channels) {
        return pcm[idx * channels];             /* mono -> nhan doi kenh */
    }
    return pcm[idx * channels + ch];
}

/* Chuyen 1 frame da giai ma sang 44100 Hz stereo, ghi vao out (toi da out_cap sample).
 * Tra ve so sample stereo da tao. */
static size_t resample_frame(const int16_t *pcm, int samples, int channels, int hz,
                             int16_t *out, size_t out_cap)
{
    if (samples <= 0 || hz <= 0) {
        return 0;
    }

    double step = (double)hz / (double)OUT_HZ;  /* so sample nguon cho 1 sample ra */
    size_t produced = 0;

    /* s_t = 0 ung voi s_last[]; s_t = k+1 ung voi pcm[k] */
    while (s_t < samples) {
        int idx = (int)s_t;
        double frac = s_t - idx;

        if ((produced + 1) * OUT_CH > out_cap) {
            break;
        }
        for (int c = 0; c < OUT_CH; c++) {
            int16_t a = (idx == 0) ? s_last[c] : sample_at(pcm, idx - 1, c, channels);
            int16_t b = sample_at(pcm, idx, c, channels);
            out[produced * OUT_CH + c] = (int16_t)(a + (b - a) * frac);
        }
        produced++;
        s_t += step;
    }

    s_t -= samples;
    if (s_t < 0) {
        s_t = 0;
    }
    for (int c = 0; c < OUT_CH; c++) {
        s_last[c] = sample_at(pcm, samples - 1, c, channels);
    }

    return produced;
}

/* Doc them du lieu vao in[] cho day IN_CAP. Tra ve so byte hien co. */
static size_t fill_input(FILE *f, uint8_t *in, size_t len, bool *eof)
{
    if (*eof || len >= IN_CAP) {
        return len;
    }
    size_t n = fread(in + len, 1, IN_CAP - len, f);
    if (n == 0) {
        *eof = true;
    }
    return len + n;
}

/*
 * Vong lap giai ma chung.
 * to_ring = true : day PCM vao ring cho A2DP (dung khi dang phat).
 * to_ring = false: chi dem sample (dung cho test giai ma).
 */
static void decode_loop(FILE *f, HMP3Decoder dec, uint8_t *in, int16_t *pcm, int16_t *out,
                        bool to_ring, decode_stats_t *st)
{
    bool eof = false;
    size_t len = fill_input(f, in, 0, &eof);

    s_t = 0;
    memset(s_last, 0, sizeof(s_last));

    while (to_ring ? s_running : true) {
        /* Pause: block here so the file position is preserved.  A2DP keeps
         * asking for PCM but mp3_player_read() returns 0 -> the data callback
         * sends silence, which is exactly what a paused source should do. */
        if (to_ring) {
            while (s_running && s_paused) {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            if (!s_running) {
                break;
            }
        }

        if (len < 4) {
            if (eof) {
                break;
            }
            len = fill_input(f, in, len, &eof);
            continue;
        }

        int off = MP3FindSyncWord(in, (int)len);
        if (off < 0) {
            /* Giu lai byte 0xFF cuoi cung (co the la nua dau sync word) */
            size_t keep = (in[len - 1] == 0xFF) ? 1 : 0;
            if (keep) {
                in[0] = in[len - 1];
            }
            len = keep;
            if (eof) {
                break;
            }
            len = fill_input(f, in, len, &eof);
            continue;
        }
        if (off > 0) {
            memmove(in, in + off, len - off);
            len -= off;
        }

        const unsigned char *p = in;
        size_t avail = len;
        int rc = MP3Decode(dec, &p, &avail, pcm, 0);

        if (rc == ERR_MP3_INDATA_UNDERFLOW) {
            /* Frame chua du du lieu: doc them, tru khi bo dem da day */
            size_t nl = fill_input(f, in, len, &eof);
            if (nl > len) {
                len = nl;
                continue;
            }
            if (eof) {
                break;
            }
            /* Bo dem day ma van thieu -> du lieu hong, nhay 1 byte */
            memmove(in, in + 1, len - 1);
            len--;
            continue;
        }

        size_t used = len - avail;      /* so byte MP3Decode da tieu thu */
        if (used == 0) {
            if (!eof) {
                size_t nl = fill_input(f, in, len, &eof);
                if (nl > len) {
                    len = nl;
                    continue;
                }
            }
            if (len <= 1) {
                break;
            }
            memmove(in, in + 1, len - 1);
            len--;
            continue;
        }
        memmove(in, in + used, len - used);
        len -= used;

        if (rc == 0) {
            MP3FrameInfo info;
            MP3GetLastFrameInfo(dec, &info);
            int samples = (info.nChans > 0) ? info.outputSamps / info.nChans : 0;

            if (samples > 0 && samples <= MAX_SRC_SAMPLES) {
                size_t out_samples = resample_frame(pcm, samples, info.nChans,
                                                    info.samprate, out, OUT_SAMPLES);
                size_t bytes = out_samples * OUT_CH * sizeof(int16_t);

                if (to_ring) {
                    size_t sent = 0;
                    while (sent < bytes && s_running) {
                        sent += xStreamBufferSend(s_ring, (uint8_t *)out + sent, bytes - sent,
                                                  pdMS_TO_TICKS(100));
                    }
                } else {
                    st->pcm_samples += out_samples;
                }

                st->frames++;
                if (st->frames == 1) {
                    st->sample_rate = info.samprate;
                    st->channels = info.nChans;
                    st->bitrate_kbps = info.bitrate / 1000;
                    ESP_LOGI(TAG, "Frame dau: %d Hz, %d kenh, %d kbps",
                             info.samprate, info.nChans, info.bitrate / 1000);
                }
                if (to_ring && st->frames == 200) {
                    ESP_LOGI(TAG, "stack con lai: %u byte",
                             (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
                }
            }
        } else if (rc != ERR_MP3_MAINDATA_UNDERFLOW) {
            st->errors++;
            if (st->errors <= 5 || (st->errors % 200) == 0) {
                ESP_LOGW(TAG, "Loi giai ma %d (da gap %lu lan)", rc,
                         (unsigned long)st->errors);
            }
        }

        len = fill_input(f, in, len, &eof);
    }
}

esp_err_t mp3_player_init(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }

    if (s_ring == NULL) {
        /* Buffer tinh trong .bss: giai phong 8 KB heap cho BT/Wi-Fi. */
        s_ring = xStreamBufferCreateStatic(RING_BYTES,
                                           OUT_CH * sizeof(int16_t),
                                           s_ring_storage, &s_ring_struct);
        if (s_ring == NULL) {
            ESP_LOGE(TAG, "Khong tao duoc ring buffer");
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_done == NULL) {
        s_done = xSemaphoreCreateBinary();
        if (s_done == NULL) {
            ESP_LOGE(TAG, "Khong tao duoc semaphore");
            return ESP_ERR_NO_MEM;
        }
    }

    if (xTaskCreate(player_task, "mp3_player", PLAYER_TASK_STACK, NULL, 5, &s_task) != pdPASS) {
        s_task = NULL;
        ESP_LOGE(TAG, "Khong tao duoc task phat nhac");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static void player_task(void *arg)
{
    (void)arg;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if (!s_running) {
            continue;
        }

        FILE *f = fopen(s_path, "rb");
        if (f == NULL) {
            ESP_LOGE(TAG, "Khong mo duoc '%s'", s_path);
            s_running = false;
            xSemaphoreGive(s_done);
            continue;
        }

        ESP_LOGI(TAG, "Bat dau phat: %s", s_path);

        HMP3Decoder dec = MP3InitDecoder();
        if (dec == NULL) {
            ESP_LOGE(TAG, "Khong tao duoc bo giai ma");
        } else {
            decode_stats_t st = {0};
            decode_loop(f, dec, s_in, s_pcm, s_out, true, &st);
            ESP_LOGI(TAG, "Phat xong (%lu frame MP3, %lu loi)",
                     (unsigned long)st.frames, (unsigned long)st.errors);
            MP3FreeDecoder(dec);
        }

        fclose(f);
        s_running = false;
        xSemaphoreGive(s_done);
    }
}

/* ------------------------------ test giai ma ------------------------------ */

static char s_test_path[300];
static mp3_selftest_result_t s_test_result;
static SemaphoreHandle_t s_test_done;

static void selftest_task(void *arg)
{
    (void)arg;

    memset(&s_test_result, 0, sizeof(s_test_result));

    FILE *f = fopen(s_test_path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Khong mo duoc '%s'", s_test_path);
        goto done;
    }

    HMP3Decoder dec = MP3InitDecoder();
    if (dec == NULL) {
        ESP_LOGE(TAG, "Khong tao duoc bo giai ma");
        fclose(f);
        goto done;
    }

    decode_stats_t st = {0};
    decode_loop(f, dec, s_in, s_pcm, s_out, false, &st);
    MP3FreeDecoder(dec);
    fclose(f);

    s_test_result.frames = st.frames;
    s_test_result.errors = st.errors;
    s_test_result.pcm_samples = st.pcm_samples;
    s_test_result.duration_ms = (uint32_t)((uint64_t)st.pcm_samples * 1000 / OUT_HZ);
    s_test_result.sample_rate = st.sample_rate;
    s_test_result.channels = st.channels;
    s_test_result.bitrate_kbps = st.bitrate_kbps;
    s_test_result.ok = (st.frames > 0);

done:
    xSemaphoreGive(s_test_done);
    vTaskDelete(NULL);
}

esp_err_t mp3_player_selftest(const char *path, mp3_selftest_result_t *out)
{
    if (s_running || path == NULL || out == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_test_done == NULL) {
        s_test_done = xSemaphoreCreateBinary();
        if (s_test_done == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    snprintf(s_test_path, sizeof(s_test_path), "%s", path);
    if (xTaskCreate(selftest_task, "mp3_selftest", PLAYER_TASK_STACK, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    if (xSemaphoreTake(s_test_done, pdMS_TO_TICKS(120000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    *out = s_test_result;
    return out->ok ? ESP_OK : ESP_FAIL;
}

/* ------------------------------ API phat nhac ------------------------------ */

esp_err_t mp3_player_start(const char *path)
{
    if (path == NULL || s_running) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = mp3_player_init();
    if (ret != ESP_OK) {
        return ret;
    }

    snprintf(s_path, sizeof(s_path), "%s", path);

    s_stream_mode = false;
    xStreamBufferReset(s_ring);
    xSemaphoreTake(s_done, 0);          /* xoa tin hieu "xong" cu */

    s_paused = false;
    s_running = true;
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

void mp3_player_stop(void)
{
    s_paused = false;
    s_stream_mode = false;

    if (!s_running) {
        if (s_ring != NULL) {
            xStreamBufferReset(s_ring);
        }
        return;
    }

    s_running = false;

    /* Cho task nha bo dem va ket thuc file hien tai (toi da 1 giay) */
    if (s_done != NULL) {
        xSemaphoreTake(s_done, pdMS_TO_TICKS(1000));
    }

    if (s_ring != NULL) {
        xStreamBufferReset(s_ring);
    }
}

esp_err_t mp3_player_stream_start(void)
{
    esp_err_t ret = mp3_player_init();
    if (ret != ESP_OK) {
        return ret;
    }

    mp3_player_stop();
    xStreamBufferReset(s_ring);
    s_paused = false;
    s_stream_mode = true;
    return ESP_OK;
}

static uint32_t s_dbg_writes;
static uint32_t s_dbg_write_bytes;
static uint32_t s_dbg_write_full;

bool mp3_player_stream_write(const void *data, size_t length)
{
    if (s_ring == NULL || data == NULL || length == 0) {
        return false;
    }

    const uint8_t *bytes = (const uint8_t *)data;
    size_t written = 0;
    while (written < length && s_stream_mode) {
        if (s_paused) {
            return true;
        }
        if (!s_stream_mode) {
            return false;
        }
        size_t count = xStreamBufferSend(s_ring, bytes + written, length - written,
                                        pdMS_TO_TICKS(1000));
        if (count == 0) {
            s_dbg_write_full++;
            if (s_paused) {
                return true;
            }
            ESP_LOGW(TAG, "Incoming PCM buffer remained full");
            return false;
        }
        written += count;
    }
    s_dbg_writes++;
    s_dbg_write_bytes += (uint32_t)written;
    if ((s_dbg_writes % 200) == 0) {
        ESP_LOGI(TAG, "stream_write: %lu goi, %lu byte, %lu lan day (heap %u)",
                 (unsigned long)s_dbg_writes, (unsigned long)s_dbg_write_bytes,
                 (unsigned long)s_dbg_write_full,
                 (unsigned)esp_get_free_heap_size());
    }
    return written == length || s_paused;
}

void mp3_player_stream_end(void)
{
    s_stream_mode = false;
    s_paused = false;
}

void mp3_player_stream_stop(void)
{
    mp3_player_stop();
}

bool mp3_player_is_streaming(void)
{
    return s_stream_mode;
}

int32_t mp3_player_read(void *buf, int32_t len)
{
    if (s_ring == NULL || buf == NULL || len <= 0) {
        return 0;
    }
    return (int32_t)xStreamBufferReceive(s_ring, buf, (size_t)len, 0);
}

bool mp3_player_is_running(void)
{
    return s_running || s_stream_mode;
}

void mp3_player_pause(void)
{
    if (s_running || s_stream_mode) {
        s_paused = true;
    }
}

void mp3_player_resume(void)
{
    s_paused = false;
}

bool mp3_player_is_paused(void)
{
    return (s_running || s_stream_mode) && s_paused;
}
