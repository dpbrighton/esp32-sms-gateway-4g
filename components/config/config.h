#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GW_API_KEY_LEN   33

typedef struct {
    char  wifi_ssid[64];
    char  wifi_password[64];
    int   wifi_max_retry;
    char  mqtt_broker_uri[128];
    char  mqtt_topic_prefix[64];
    char  mqtt_username[64];
    char  mqtt_password[64];
    char  webhook_url[256];
    bool  webhook_enabled;
    char  api_key[GW_API_KEY_LEN];
} gw_config_t;

void        gw_config_init(void);
gw_config_t *gw_config_get(void);
esp_err_t   gw_config_save(const gw_config_t *cfg);

#ifdef __cplusplus
}
#endif
