#pragma once
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

void rest_api_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
