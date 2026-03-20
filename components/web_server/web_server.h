#pragma once
#include "esp_http_server.h"
#include "sms_store.h"

#ifdef __cplusplus
extern "C" {
#endif

httpd_handle_t web_server_start(void);
void           web_server_stop(httpd_handle_t server);
void           web_server_ws_broadcast_sms(const sms_message_t *msg);

#ifdef __cplusplus
}
#endif
