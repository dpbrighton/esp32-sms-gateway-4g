#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start OTA update from given URL.
 *        Runs asynchronously - device reboots on success.
 * @param url  HTTP/HTTPS URL to firmware .bin file
 */
esp_err_t ota_update_start(const char *url);

/** @brief Get current OTA status string for UI display */
const char *ota_update_status(void);

#ifdef __cplusplus
}
#endif
