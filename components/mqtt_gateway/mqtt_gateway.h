#pragma once
#include "esp_err.h"
#include "sms_store.h"

#ifdef __cplusplus
extern "C" {
#endif

void mqtt_gw_init(void);
void mqtt_gw_publish_inbound(const sms_message_t *msg);
void mqtt_gw_publish_status(void);
void mqtt_gw_set_send_status(const char *status);
bool mqtt_gw_is_connected(void);

#ifdef __cplusplus
}
#endif
