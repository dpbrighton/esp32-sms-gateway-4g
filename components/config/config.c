#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "config.h"

static const char *TAG    = "GW_CONFIG";
static const char *NVS_NS = "gw_config";
static gw_config_t s_config;

void gw_config_init(void)
{
    memset(&s_config, 0, sizeof(s_config));

    /* Hardcoded defaults - change via POST /api/config or NVS */
    strlcpy(s_config.wifi_ssid,         "YOUR_WIFI_SSID",                              sizeof(s_config.wifi_ssid));
    strlcpy(s_config.wifi_password,     "YOUR_WIFI_PASSWORD",                         sizeof(s_config.wifi_password));
    s_config.wifi_max_retry = 5;
    strlcpy(s_config.mqtt_broker_uri,   "mqtt://YOUR_BROKER_IP:1883", sizeof(s_config.mqtt_broker_uri));
	strlcpy(s_config.mqtt_topic_prefix, "sms_gateway_4g",           sizeof(s_config.mqtt_topic_prefix));
	strlcpy(s_config.mqtt_username,     "YOUR_MQTT_USERNAME",                sizeof(s_config.mqtt_username));
	strlcpy(s_config.mqtt_password,     "YOUR_MQTT_PASSWORD",               sizeof(s_config.mqtt_password));
    strlcpy(s_config.webhook_url,       "http://your-server.example.com/sms", sizeof(s_config.webhook_url));
    s_config.webhook_enabled = false;
    strlcpy(s_config.api_key,           "YOUR_API_KEY_32_CHARS_MIN",      sizeof(s_config.api_key));

    /* Override from NVS if saved values exist */
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len;
#define NVS_GET_STR(key, field) \
        len = sizeof(s_config.field); \
        nvs_get_str(nvs, key, s_config.field, &len);

        NVS_GET_STR("wifi_ssid",   wifi_ssid);
        NVS_GET_STR("wifi_pass",   wifi_password);
        NVS_GET_STR("mqtt_uri",    mqtt_broker_uri);
        NVS_GET_STR("mqtt_prefix", mqtt_topic_prefix);
        NVS_GET_STR("mqtt_user",   mqtt_username);
        NVS_GET_STR("mqtt_pass",   mqtt_password);
        NVS_GET_STR("webhook_url", webhook_url);
        NVS_GET_STR("api_key",     api_key);

        uint8_t wh_en = 0;
        nvs_get_u8(nvs, "webhook_en", &wh_en);
        s_config.webhook_enabled = (bool)wh_en;

        nvs_close(nvs);
        ESP_LOGI(TAG, "Config loaded from NVS.");
    } else {
        ESP_LOGI(TAG, "No NVS config - using defaults.");
    }
}

gw_config_t *gw_config_get(void)
{
    return &s_config;
}

esp_err_t gw_config_save(const gw_config_t *cfg)
{
    memcpy(&s_config, cfg, sizeof(gw_config_t));

    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(NVS_NS, NVS_READWRITE, &nvs));

    nvs_set_str(nvs, "wifi_ssid",   s_config.wifi_ssid);
    nvs_set_str(nvs, "wifi_pass",   s_config.wifi_password);
    nvs_set_str(nvs, "mqtt_uri",    s_config.mqtt_broker_uri);
    nvs_set_str(nvs, "mqtt_prefix", s_config.mqtt_topic_prefix);
    nvs_set_str(nvs, "mqtt_user",   s_config.mqtt_username);
    nvs_set_str(nvs, "mqtt_pass",   s_config.mqtt_password);
    nvs_set_str(nvs, "webhook_url", s_config.webhook_url);
    nvs_set_str(nvs, "api_key",     s_config.api_key);
    nvs_set_u8 (nvs, "webhook_en",  (uint8_t)s_config.webhook_enabled);

    esp_err_t err = nvs_commit(nvs);
    nvs_close(nvs);
    ESP_LOGI(TAG, "Config saved to NVS.");
    return err;
}
