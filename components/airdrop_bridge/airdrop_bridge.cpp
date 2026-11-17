// ESP32Drop receive window for esp32-photoframe.
//
// Nothing in ESP32Drop is modified. Its two ESP32 port translation units are
// compiled into this component (see CMakeLists.txt), its headers are included
// as published, and the only concession is components/esp32drop_compat's
// Arduino.h, which supplies millis()/delay()/Serial.
#include "airdrop_bridge.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

// Only the receive half of ESP32Drop is needed here. Including ESP32Drop.h
// would also pull the sender/BLE headers into this build.
#include "ESP32AWDL.h"
#include "airdrop/port/ad_port_esp32.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "airdrop";

// photoframe's image directory; see main/config.h (IMAGE_DIRECTORY).
#define AIRDROP_OUTPUT_DIR "/storage/images"

static bool s_started;
static volatile bool s_ready;
static char s_path[160];
static uint32_t s_bytes;

static bool airdrop_on_file(void *ctx, const struct AdFile *f)
{
    (void)ctx;

    const char *ext = NULL;
    if (f->type == AD_FILE_JPEG) {
        ext = "jpg";
    } else if (f->type == AD_FILE_PNG) {
        ext = "png";
    } else if (f->type == AD_FILE_APPLEDOUBLE) {
        // Finder metadata sidecar that rides along with every Mac transfer.
        return true;
    } else {
        ESP_LOGW(TAG, "ignoring %s: unsupported type %s", f->name, ad_cpio_type_name(f->type));
        return false;
    }

    if (s_ready) {
        // First displayable image wins; a transfer may carry several files.
        ESP_LOGI(TAG, "ignoring additional file %s", f->name);
        return true;
    }

    mkdir(AIRDROP_OUTPUT_DIR, 0775);
    snprintf(s_path, sizeof(s_path), "%s/airdrop-%lu.%s", AIRDROP_OUTPUT_DIR,
             (unsigned long)(esp_timer_get_time() / 1000ULL), ext);

    FILE *fp = fopen(s_path, "wb");
    if (!fp) {
        ESP_LOGE(TAG, "cannot write %s", s_path);
        s_path[0] = '\0';
        return false;
    }
    size_t written = fwrite(f->data, 1, f->len, fp);
    fclose(fp);
    if (written != f->len) {
        ESP_LOGE(TAG, "short write to %s (%u of %u)", s_path, (unsigned)written, (unsigned)f->len);
        remove(s_path);
        s_path[0] = '\0';
        return false;
    }

    s_bytes = f->len;
    ESP_LOGI(TAG, "received %s (%u bytes) -> %s", f->name, (unsigned)f->len, s_path);
    s_ready = true;
    return true;
}

bool airdrop_bridge_boot_requested(void)
{
#if AIRDROP_BRIDGE_TRIGGER_GPIO < 0
    return false;
#else
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << (int)AIRDROP_BRIDGE_TRIGGER_GPIO;
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = AIRDROP_BRIDGE_TRIGGER_ACTIVE_LOW ? GPIO_PULLUP_ENABLE
                                                       : GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = AIRDROP_BRIDGE_TRIGGER_ACTIVE_LOW ? GPIO_PULLDOWN_DISABLE
                                                         : GPIO_PULLDOWN_ENABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;

    if (gpio_config(&cfg) != ESP_OK) {
        return false;
    }
    // Let the pull settle before sampling a mechanical button.
    vTaskDelay(pdMS_TO_TICKS(20));

    int level = gpio_get_level((gpio_num_t)AIRDROP_BRIDGE_TRIGGER_GPIO);
    bool held = AIRDROP_BRIDGE_TRIGGER_ACTIVE_LOW ? (level == 0) : (level != 0);
    if (held) {
        ESP_LOGI(TAG, "AirDrop boot window requested (GPIO %d)", (int)AIRDROP_BRIDGE_TRIGGER_GPIO);
    }
    return held;
#endif
}

static void airdrop_log_diagnostics(void)
{
    char line[200];
    while (ad_diag_read_line(line, sizeof(line))) {
        ESP_LOGI(TAG, "%s", line);
    }
    uint32_t dropped = ad_diag_dropped();
    if (dropped) {
        ESP_LOGW(TAG, "%u diagnostic lines dropped", (unsigned)dropped);
    }
}

static esp_err_t airdrop_prepare_networking(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return err;
    }

    // ESP32Drop tolerates these already existing; photoframe may have
    // initialized some of them already.
    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    return ESP_OK;
}

void airdrop_bridge_abort(void)
{
    if (!s_started) {
        return;
    }
    // awdl_end() may refuse while a peer is still being acknowledged. Keeping
    // s_started true in that case is deliberate: the next attempt (or a real
    // reboot) still needs to know the radio is ours.
    esp_err_t err = awdl_end();
    if (err == ESP_OK) {
        s_started = false;
        ESP_LOGI(TAG, "AWDL released");
    } else {
        ESP_LOGW(TAG, "awdl_end did not complete: %s", esp_err_to_name(err));
    }
}

esp_err_t airdrop_bridge_receive_to_file(char *out_path, size_t out_path_cap, uint32_t timeout_ms)
{
    if (!out_path || out_path_cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out_path[0] = '\0';
    s_ready = false;
    s_path[0] = '\0';

    esp_err_t err = airdrop_prepare_networking();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "network bring-up failed: %s", esp_err_to_name(err));
        return err;
    }

    err = awdl_begin();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "awdl_begin failed: %s", esp_err_to_name(err));
        return err;
    }
    s_started = true;

    ad_on_file(airdrop_on_file, NULL);
    int rc = ad_begin(AIRDROP_BRIDGE_NAME, AIRDROP_BRIDGE_MAX_RECEIVE_BYTES);
    if (rc != 0) {
        ESP_LOGE(TAG, "ad_begin failed (%d)", rc);
        airdrop_bridge_abort();
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "listening as \"%s\" for %u s", AIRDROP_BRIDGE_NAME,
             (unsigned)(timeout_ms / 1000));

    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    uint32_t last_logged_bytes = 0;
    while (!s_ready && xTaskGetTickCount() < deadline) {
        // The only call ESP32Drop requires to be pumped; callbacks run on this
        // task, which is why writing the file above is legal.
        ad_poll();
        airdrop_log_diagnostics();

        struct AdStatus st;
        ad_stats_read(&st);
        if (st.rx_active && st.rx_bytes - last_logged_bytes > (256u * 1024u)) {
            last_logged_bytes = st.rx_bytes;
            ESP_LOGI(TAG, "receiving: %u KB", (unsigned)(st.rx_bytes / 1024));
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (!s_ready) {
        ESP_LOGW(TAG, "no AirDrop transfer within the window");
        airdrop_bridge_abort();
        return ESP_ERR_TIMEOUT;
    }

    // Let the archive finish so the sender is told the transfer succeeded
    // instead of watching its progress bar die at the last block. Bounded, and
    // only while a transfer is actually in flight.
    const TickType_t grace = xTaskGetTickCount() + pdMS_TO_TICKS(3000);
    while (xTaskGetTickCount() < grace) {
        struct AdStatus st;
        ad_stats_read(&st);
        if (!st.rx_active) {
            break;
        }
        ad_poll();
        airdrop_log_diagnostics();
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    airdrop_bridge_abort();

    if (strlen(s_path) + 1 > out_path_cap) {
        return ESP_ERR_INVALID_SIZE;
    }
    strcpy(out_path, s_path);
    ESP_LOGI(TAG, "AirDrop image ready: %s (%u bytes)", out_path, (unsigned)s_bytes);
    return ESP_OK;
}
