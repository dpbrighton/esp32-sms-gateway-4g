#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "sms_store.h"
#include "config.h"
#include "webhook.h"

static const char *TAG = "WEBHOOK";

#define WEBHOOK_QUEUE_SIZE 8

static QueueHandle_t s_queue;

typedef struct {
    char sender[SMS_SENDER_MAX_LEN];
    char body[SMS_BODY_MAX_LEN];
    char timestamp[SMS_TIMESTAMP_MAX];
} webhook_payload_t;

/* ------------------------------------------------------------------ */
/*  Worker task — runs HTTP POST off the main path                     */
/* ------------------------------------------------------------------ */
static void webhook_task(void *arg)
{
    webhook_payload_t item;
    while (1) {
        if (xQueueReceive(s_queue, &item, portMAX_DELAY) != pdTRUE) continue;

        gw_config_t *cfg = gw_config_get();
        if (!cfg->webhook_enabled || cfg->webhook_url[0] == '\0') continue;

        char json[600];
        snprintf(json, sizeof(json),
                 "{\"sender\":\"%s\",\"body\":\"%s\",\"timestamp\":\"%s\"}",
                 item.sender, item.body, item.timestamp);

        esp_http_client_config_t http_cfg = {
            .url            = cfg->webhook_url,
            .method         = HTTP_METHOD_POST,
            .timeout_ms     = 10000,
            .skip_cert_common_name_check = true,
            .crt_bundle_attach       = esp_crt_bundle_attach,  /* Allow self-signed for LAN */
        };

        esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_header(client, "X-SMS-Gateway", "ESP32");
        esp_http_client_set_post_field(client, json, strlen(json));

        esp_err_t err = esp_http_client_perform(client);
        if (err == ESP_OK) {
            int status = esp_http_client_get_status_code(client);
            ESP_LOGI(TAG, "Webhook delivered, HTTP %d", status);
        } else {
            ESP_LOGW(TAG, "Webhook failed: %s", esp_err_to_name(err));
        }
        esp_http_client_cleanup(client);
    }
}

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */
void webhook_init(void)
{
    s_queue = xQueueCreate(WEBHOOK_QUEUE_SIZE, sizeof(webhook_payload_t));
    xTaskCreate(webhook_task, "webhook", 8192, NULL, 5, NULL);
    ESP_LOGI(TAG, "Webhook worker started.");
}

void webhook_send(const sms_message_t *msg)
{
    gw_config_t *cfg = gw_config_get();
    if (!cfg->webhook_enabled) return;

    webhook_payload_t item;
    strlcpy(item.sender,    msg->sender,    sizeof(item.sender));
    strlcpy(item.body,      msg->body,      sizeof(item.body));
    strlcpy(item.timestamp, msg->timestamp, sizeof(item.timestamp));

    if (xQueueSend(s_queue, &item, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Webhook queue full — dropping.");
    }
}
