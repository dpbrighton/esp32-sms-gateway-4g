#pragma once
#include <stdint.h>
#include "esp_err.h"
#include "sms_store.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*a7670e_sms_cb_t)(const sms_message_t *msg);

typedef struct {
    int      uart_num;
    int      baud_rate;
    int      tx_pin;
    int      rx_pin;
    int      pwrkey_pin;
    int      reset_pin;
    a7670e_sms_cb_t on_sms_received;
} a7670e_config_t;

typedef struct {
    bool    powered_on;
    bool    sim_ready;
    bool    registered;
    int     rssi;
    char    operator_name[32];
    char    imei[20];
    char    firmware[32];
} a7670e_status_t;

esp_err_t a7670e_init(const a7670e_config_t *cfg);
esp_err_t a7670e_send_sms(const char *number, const char *text);
esp_err_t a7670e_get_status(a7670e_status_t *out_status);
esp_err_t a7670e_delete_sms(int index);
esp_err_t a7670e_reset(void);

#ifdef __cplusplus
}
#endif
