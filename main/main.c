#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "mdns.h"
#include "esp_eth_phy_lan87xx.h"
#include "driver/gpio.h"

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

#define MODEM_UART_NUM    1
#define MODEM_UART_BAUD   115200
#define MODEM_TX_PIN      17
#define MODEM_RX_PIN      5
#define MODEM_PWRKEY_PIN  4
#define MODEM_RESET_PIN   14

#define NET_CONNECTED_BIT BIT0

static EventGroupHandle_t s_net_event_group;
static bool s_using_ethernet = false;

static void on_got_ip(ip_event_got_ip_t *event)
{
    ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    xEventGroupSetBits(s_net_event_group, NET_CONNECTED_BIT);
    mqtt_gw_wifi_connected();
}

static void on_lost_ip(void)
{
    xEventGroupClearBits(s_net_event_group, NET_CONNECTED_BIT);
    mqtt_gw_wifi_disconnected();
}

static void eth_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == ETH_EVENT) {
        switch (event_id) {
        case ETHERNET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Ethernet link up.");
            break;
        case ETHERNET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "Ethernet link down.");
            on_lost_ip();
            break;
        default:
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_ETH_GOT_IP) {
        on_got_ip((ip_event_got_ip_t *)event_data);
    }
}

static int s_wifi_retry = 0;
static TimerHandle_t s_wifi_retry_timer = NULL;

static uint32_t wifi_backoff_ms(int attempt)
{
    uint32_t delay = 2000;
    for (int i = 0; i < attempt && i < 5; i++) delay *= 2;
    return delay > 60000 ? 60000 : delay;
}

static void wifi_retry_timer_cb(TimerHandle_t timer)
{
    ESP_LOGI(TAG, "WiFi retry timer fired, attempting reconnect...");
    esp_wifi_connect();
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
        s_wifi_retry++;
        uint32_t delay_ms = wifi_backoff_ms(s_wifi_retry);
        ESP_LOGW(TAG, "WiFi disconnected (reason=%d), retry %d in %lus...",
                 disc->reason, s_wifi_retry, (unsigned long)(delay_ms / 1000));
        on_lost_ip();
        if (s_wifi_retry_timer == NULL) {
            s_wifi_retry_timer = xTimerCreate("wifi_retry", pdMS_TO_TICKS(delay_ms),
                                               pdFALSE, NULL, wifi_retry_timer_cb);
        } else {
            xTimerChangePeriod(s_wifi_retry_timer, pdMS_TO_TICKS(delay_ms), 0);
        }
        xTimerStart(s_wifi_retry_timer, 0);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_wifi_retry = 0;
        if (s_wifi_retry_timer) xTimerStop(s_wifi_retry_timer, 0);
        on_got_ip((ip_event_got_ip_t *)event_data);
    }
}

static esp_err_t eth_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << 16),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(16, 1);
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Set hostname so router shows friendly name */
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&netif_cfg);
    esp_netif_set_hostname(eth_netif, "smsgateway");

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_config.smi_gpio.mdc_num  = 23;
    emac_config.smi_gpio.mdio_num = 18;
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr       = 1;
    phy_config.reset_gpio_num = -1;
    esp_eth_phy_t *phy = esp_eth_phy_new_lan87xx(&phy_config);

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = NULL;

    esp_err_t err = esp_eth_driver_install(&eth_config, &eth_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Ethernet driver install failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_ERROR_CHECK(esp_netif_attach(eth_netif,
                    esp_eth_new_netif_glue(eth_handle)));
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                                &eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                                &eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_eth_start(eth_handle));

    ESP_LOGI(TAG, "Ethernet started, waiting for link...");
    EventBits_t bits = xEventGroupWaitBits(s_net_event_group, NET_CONNECTED_BIT,
                                            pdFALSE, pdFALSE, pdMS_TO_TICKS(8000));
    if (bits & NET_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Ethernet connected.");
        s_using_ethernet = true;
        return ESP_OK;
    }
    ESP_LOGW(TAG, "Ethernet did not connect within timeout.");
    return ESP_FAIL;
}

static esp_err_t wifi_init_sta(void)
{
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(sta_netif, "smsgateway");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                &wifi_event_handler, NULL));

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
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G);

    ESP_LOGI(TAG, "WiFi connecting to SSID: %s", gw_cfg->wifi_ssid);

    EventBits_t bits = xEventGroupWaitBits(s_net_event_group, NET_CONNECTED_BIT,
                                            pdFALSE, pdFALSE, portMAX_DELAY);
    if (bits & NET_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected.");
        return ESP_OK;
    }
    return ESP_FAIL;
}

static void watchdog_task(void *arg)
{
    static int s_offline_seconds = 0;
    const int REBOOT_AFTER_SECONDS = 300;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));
        if (!(xEventGroupGetBits(s_net_event_group) & NET_CONNECTED_BIT)) {
            s_offline_seconds += 60;
            ESP_LOGW(TAG, "Watchdog: network offline for %d seconds", s_offline_seconds);
            if (s_offline_seconds >= REBOOT_AFTER_SECONDS) {
                ESP_LOGE(TAG, "Watchdog: offline too long, rebooting...");
                esp_restart();
            }
        } else {
            if (s_offline_seconds > 0)
                ESP_LOGI(TAG, "Watchdog: network back online, resetting counter");
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

    s_net_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_LOGI(TAG, "Trying Ethernet first...");
    if (eth_init() != ESP_OK) {
        ESP_LOGW(TAG, "Ethernet not available, falling back to WiFi...");
        if (wifi_init_sta() != ESP_OK) {
            ESP_LOGW(TAG, "WiFi also unavailable - running in modem-only mode.");
        }
    }

    tcp_log_init();

    /* mDNS - accessible as http://smsgateway.local/ */
    mdns_init();
    mdns_hostname_set("smsgateway");
    mdns_instance_name_set("ESP32 SMS Gateway");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    xTaskCreate(watchdog_task, "watchdog", 2048, NULL, 2, NULL);

    httpd_handle_t server = web_server_start();
    if (server) {
        rest_api_register(server);
        ESP_LOGI(TAG, "Web UI: http://<device-ip>/");
    }

    mqtt_gw_init();
    webhook_init();

    ESP_LOGI(TAG, "Gateway ready. Network: %s",
             s_using_ethernet ? "Ethernet" : "WiFi");
}
