#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"

#include "config.h"
#include "a7670e.h"
#include "sms_store.h"
#include "web_server.h"
#include "rest_api.h"
#include "mqtt_gateway.h"
#include "webhook.h"
#include "ota_update.h"
#include "tcp_log.h"

static const char *TAG = "SMS_GW_MAIN";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

/* A7670E pin definitions - change these to match your wiring */
#define MODEM_UART_NUM    1
#define MODEM_UART_BAUD   115200
#define MODEM_TX_PIN      17
#define MODEM_RX_PIN      16
#define MODEM_PWRKEY_PIN  4
#define MODEM_RESET_PIN   5

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        s_retry_num++;
        ESP_LOGW(TAG, "WiFi disconnected, retrying... (attempt %d)", s_retry_num);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, &instance_got_ip));

    gw_config_t *gw_cfg = gw_config_get();
    wifi_config_t wifi_cfg = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = { .capable = true, .required = false },
        },
    };
    strlcpy((char *)wifi_cfg.sta.ssid,     gw_cfg->wifi_ssid,     sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, gw_cfg->wifi_password, sizeof(wifi_cfg.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);  /* disable power save - mains powered device */

    ESP_LOGI(TAG, "Connecting to SSID: %s", gw_cfg->wifi_ssid);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, portMAX_DELAY);
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected.");
        return ESP_OK;
    }
    ESP_LOGE(TAG, "WiFi failed to connect.");
    return ESP_FAIL;
}

/* Watchdog task - reboots if WiFi lost for >5 minutes */
static void watchdog_task(void *arg)
{
    static int s_offline_seconds = 0;
    const int REBOOT_AFTER_SECONDS = 300;  /* 5 minutes */

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));  /* check every 60 seconds */

        if (!(xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT)) {
            s_offline_seconds += 60;
            ESP_LOGW(TAG, "Watchdog: WiFi offline for %d seconds", s_offline_seconds);
            if (s_offline_seconds >= REBOOT_AFTER_SECONDS) {
                ESP_LOGE(TAG, "Watchdog: offline too long, rebooting...");
                esp_restart();
            }
        } else {
            if (s_offline_seconds > 0) {
                ESP_LOGI(TAG, "Watchdog: WiFi back online, resetting counter");
            }
            s_offline_seconds = 0;
        }
    }
}

static void on_sms_received(const sms_message_t *msg)
{
    ESP_LOGI(TAG, "SMS from %s: %s", msg->sender, msg->body);
    sms_store_inbox_add(msg);
    web_server_ws_broadcast_sms(msg);
    mqtt_gw_publish_inbound(msg);
    webhook_send(msg);
}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32 SMS Gateway starting...");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    gw_config_init();
    sms_store_init();

    a7670e_config_t modem_cfg = {
        .uart_num        = MODEM_UART_NUM,
        .baud_rate       = MODEM_UART_BAUD,
        .tx_pin          = MODEM_TX_PIN,
        .rx_pin          = MODEM_RX_PIN,
        .pwrkey_pin      = MODEM_PWRKEY_PIN,
        .reset_pin       = MODEM_RESET_PIN,
        .on_sms_received = on_sms_received,
    };
    ESP_ERROR_CHECK(a7670e_init(&modem_cfg));

    if (wifi_init_sta() != ESP_OK) {
        ESP_LOGW(TAG, "WiFi unavailable - running in modem-only mode.");
    }

    tcp_log_init();
    /* Start watchdog - monitors WiFi and reboots if offline too long */
    xTaskCreate(watchdog_task, "watchdog", 2048, NULL, 2, NULL);

    httpd_handle_t server = web_server_start();
    if (server) {
        rest_api_register(server);
        ESP_LOGI(TAG, "Web UI: http://<device-ip>/");
    }

    mqtt_gw_init();
    webhook_init();

    ESP_LOGI(TAG, "Gateway ready.");
}
