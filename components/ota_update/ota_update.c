#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_system.h"
#include "ota_update.h"

static const char *TAG = "OTA";

#define OTA_URL_MAX  256
#define OTA_BUF_SIZE 4096

static char s_url[OTA_URL_MAX];
static char s_status[64] = "idle";

static void ota_task(void *arg)
{
    ESP_LOGI(TAG, "Starting OTA from: %s", s_url);
    snprintf(s_status, sizeof(s_status), "downloading");

    esp_http_client_config_t http_cfg = {
        .url        = s_url,
        .timeout_ms = 30000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        snprintf(s_status, sizeof(s_status), "failed: http init error");
        vTaskDelete(NULL);
        return;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        snprintf(s_status, sizeof(s_status), "failed: cannot connect");
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        vTaskDelete(NULL);
        return;
    }

    int content_len = esp_http_client_fetch_headers(client);
    ESP_LOGI(TAG, "Firmware size: %d bytes", content_len);

    esp_ota_handle_t ota_handle;
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        snprintf(s_status, sizeof(s_status), "failed: no OTA partition");
        ESP_LOGE(TAG, "No OTA partition found");
        esp_http_client_cleanup(client);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Writing to partition: %s at offset 0x%lx",
             update_partition->label, update_partition->address);

    err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        snprintf(s_status, sizeof(s_status), "failed: ota begin error");
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        vTaskDelete(NULL);
        return;
    }

    char *buf = malloc(OTA_BUF_SIZE);
    if (!buf) {
        snprintf(s_status, sizeof(s_status), "failed: OOM");
        esp_ota_abort(ota_handle);
        esp_http_client_cleanup(client);
        vTaskDelete(NULL);
        return;
    }

    int total = 0;
    int read_len;
    while ((read_len = esp_http_client_read(client, buf, OTA_BUF_SIZE)) > 0) {
        err = esp_ota_write(ota_handle, buf, read_len);
        if (err != ESP_OK) {
            snprintf(s_status, sizeof(s_status), "failed: write error");
            ESP_LOGE(TAG, "OTA write failed: %s", esp_err_to_name(err));
            free(buf);
            esp_ota_abort(ota_handle);
            esp_http_client_cleanup(client);
            vTaskDelete(NULL);
            return;
        }
        total += read_len;
        ESP_LOGI(TAG, "Written %d bytes...", total);
    }

    free(buf);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        snprintf(s_status, sizeof(s_status), "failed: ota end error");
        ESP_LOGE(TAG, "OTA end failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        snprintf(s_status, sizeof(s_status), "failed: set boot partition");
        ESP_LOGE(TAG, "Set boot partition failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    snprintf(s_status, sizeof(s_status), "success - rebooting");
    ESP_LOGI(TAG, "OTA successful! Rebooting in 2s...");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
}

esp_err_t ota_update_start(const char *url)
{
    if (strcmp(s_status, "downloading") == 0) {
        ESP_LOGW(TAG, "OTA already in progress.");
        return ESP_ERR_INVALID_STATE;
    }
    strlcpy(s_url, url, sizeof(s_url));
    snprintf(s_status, sizeof(s_status), "starting");
    xTaskCreate(ota_task, "ota_task", 8192, NULL, 5, NULL);
    return ESP_OK;
}

const char *ota_update_status(void)
{
    return s_status;
}
