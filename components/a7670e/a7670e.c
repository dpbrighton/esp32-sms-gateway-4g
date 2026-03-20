#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "a7670e.h"
#include "sms_store.h"

static const char *TAG = "A7670E";

#define UART_BUF_SIZE       (2048)
#define AT_TIMEOUT_MS       (5000)
#define AT_LONG_TIMEOUT_MS  (30000)
#define MAX_RESPONSE_LEN    (1024)
#define READER_TASK_STACK   (4096)
#define READER_TASK_PRIO    (10)

/* ------------------------------------------------------------------ */
/*  Internal state                                                      */
/* ------------------------------------------------------------------ */
static struct {
    a7670e_config_t    cfg;
    SemaphoreHandle_t  at_mutex;       /* Serialise AT commands        */
    SemaphoreHandle_t  at_response_sem;/* Signal AT response arrived   */
    char               at_response[MAX_RESPONSE_LEN];
    bool               waiting_for_response;
    a7670e_status_t    status;
} s_modem;

/* ------------------------------------------------------------------ */
/*  Low-level UART helpers                                              */
/* ------------------------------------------------------------------ */
static void uart_send(const char *data)
{
    uart_write_bytes(s_modem.cfg.uart_num, data, strlen(data));
}

static void uart_send_raw(const uint8_t *data, size_t len)
{
    uart_write_bytes(s_modem.cfg.uart_num, (const char *)data, len);
}

/* ------------------------------------------------------------------ */
/*  Parse +CMT (inbound SMS in text mode)                              */
/* ------------------------------------------------------------------ */
static void parse_cmt(const char *line, const char *next_line)
{
    /* +CMT: "+447700900000","","25/06/01,12:00:00+00" */
    sms_message_t msg = {0};
    msg.direction = SMS_DIR_INBOUND;

    /* Extract sender number */
    const char *p = strchr(line, '"');
    if (p) {
        p++;
        const char *end = strchr(p, '"');
        if (end) {
            size_t len = end - p;
            if (len >= sizeof(msg.sender)) len = sizeof(msg.sender) - 1;
            memcpy(msg.sender, p, len);
        }
    }

    /* Extract timestamp (3rd quoted field) */
    int quote_count = 0;
    p = line;
    while (*p && quote_count < 5) {
        if (*p == '"') quote_count++;
        p++;
    }
    if (quote_count == 5) {
        const char *end = strchr(p, '"');
        if (end) {
            size_t len = end - p;
            if (len >= sizeof(msg.timestamp)) len = sizeof(msg.timestamp) - 1;
            memcpy(msg.timestamp, p, len);
        }
    }

    /* Message body is next_line */
    if (next_line) {
        strlcpy(msg.body, next_line, sizeof(msg.body));
    }

    msg.index = -1; /* Not stored on SIM in this case */

    ESP_LOGI(TAG, "Inbound SMS from %s: %s", msg.sender, msg.body);

    if (s_modem.cfg.on_sms_received) {
        s_modem.cfg.on_sms_received(&msg);
    }
}

/* ------------------------------------------------------------------ */
/*  UART reader task — processes all modem output                      */
/* ------------------------------------------------------------------ */
static void uart_reader_task(void *arg)
{
    static char line_buf[512];
    static char prev_line[512];
    static int  line_pos = 0;
    static bool prev_was_cmt = false;

    uint8_t ch;
    while (1) {
        int bytes = uart_read_bytes(s_modem.cfg.uart_num, &ch, 1, pdMS_TO_TICKS(20));
        if (bytes <= 0) continue;

        if (ch == '\r') continue; /* Ignore CR */

        if (ch == '\n') {
            line_buf[line_pos] = '\0';

            if (line_pos > 0) {
                ESP_LOGD(TAG, "RX: %s", line_buf);

                if (prev_was_cmt) {
                    /* This line is the SMS body */
                    parse_cmt(prev_line, line_buf);
                    prev_was_cmt = false;
                } else if (strncmp(line_buf, "+CMT:", 5) == 0) {
                    strlcpy(prev_line, line_buf, sizeof(prev_line));
                    prev_was_cmt = true;
                } else if (strncmp(line_buf, "+CREG:", 6) == 0) {
                    int n, stat;
                    if (sscanf(line_buf, "+CREG: %d,%d", &n, &stat) == 2 ||
                        sscanf(line_buf, "+CREG: %d", &stat) == 1) {
                        s_modem.status.registered = (stat == 1 || stat == 5);
                    }
                } else if (strncmp(line_buf, "+CSQ:", 5) == 0) {
                    int rssi, ber;
                    if (sscanf(line_buf, "+CSQ: %d,%d", &rssi, &ber) == 2) {
                        s_modem.status.rssi = rssi;
                    }
                }

                /* If we're waiting for an AT response, capture it */
                if (s_modem.waiting_for_response) {
                    size_t cur_len = strlen(s_modem.at_response);
                    if (cur_len + line_pos + 2 < MAX_RESPONSE_LEN) {
                        if (cur_len > 0) {
                            s_modem.at_response[cur_len] = '\n';
                            cur_len++;
                        }
                        memcpy(s_modem.at_response + cur_len, line_buf, line_pos + 1);
                    }
                    /* Signal completion on OK / ERROR / > */
                    if (strcmp(line_buf, "OK") == 0 ||
                        strcmp(line_buf, "ERROR") == 0 ||
                        strncmp(line_buf, "+CMS ERROR:", 11) == 0 ||
                        strncmp(line_buf, "+CME ERROR:", 11) == 0 ||
                        strncmp(line_buf, ">", 1) == 0 ||
                        strcmp(line_buf, "SEND OK") == 0) {
                        xSemaphoreGive(s_modem.at_response_sem);
                    }
                }
            }
            line_pos = 0;
        } else {
            if (line_pos < (int)sizeof(line_buf) - 1) {
                line_buf[line_pos++] = (char)ch;
            }
            /* Detect '> ' prompt which has no newline terminator */
            if (line_pos >= 1 && line_buf[0] == '>' ) {
                line_buf[line_pos] = '\0';
                if (s_modem.waiting_for_response) {
                    strlcpy(s_modem.at_response, ">", MAX_RESPONSE_LEN);
                    xSemaphoreGive(s_modem.at_response_sem);
                }
                line_pos = 0;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/*  AT command helper: send command, wait for response                 */
/* ------------------------------------------------------------------ */
static esp_err_t at_cmd(const char *cmd, uint32_t timeout_ms,
                         char *resp_out, size_t resp_out_len)
{
    xSemaphoreTake(s_modem.at_mutex, portMAX_DELAY);

    memset(s_modem.at_response, 0, sizeof(s_modem.at_response));
    s_modem.waiting_for_response = true;

    uart_send(cmd);
    uart_send("\r\n");

    ESP_LOGD(TAG, "TX: %s", cmd);

    BaseType_t got = xSemaphoreTake(s_modem.at_response_sem, pdMS_TO_TICKS(timeout_ms));
    s_modem.waiting_for_response = false;

    if (resp_out && resp_out_len > 0) {
        strlcpy(resp_out, s_modem.at_response, resp_out_len);
    }

    xSemaphoreGive(s_modem.at_mutex);

    if (!got) {
        ESP_LOGW(TAG, "AT timeout: %s", cmd);
        return ESP_ERR_TIMEOUT;
    }

    if (strstr(s_modem.at_response, "ERROR")) {
        ESP_LOGW(TAG, "AT error for: %s — %s", cmd, s_modem.at_response);
        return ESP_FAIL;
    }

    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Power-on sequence for A7670E                                       */
/* ------------------------------------------------------------------ */
static void modem_power_on(void)
{
    gpio_config_t io_conf = {
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
        .pin_bit_mask = (1ULL << s_modem.cfg.pwrkey_pin) |
                        (1ULL << s_modem.cfg.reset_pin),
    };
    gpio_config(&io_conf);

    /* PWRKEY pulse: HIGH → wait 100ms → LOW → wait 2s → HIGH */
    gpio_set_level(s_modem.cfg.pwrkey_pin, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(s_modem.cfg.pwrkey_pin, 0);
    vTaskDelay(pdMS_TO_TICKS(2000));
    gpio_set_level(s_modem.cfg.pwrkey_pin, 1);

    /* Wait for module to boot */
    vTaskDelay(pdMS_TO_TICKS(5000));
    ESP_LOGI(TAG, "Modem power-on sequence complete.");
}

/* ------------------------------------------------------------------ */
/*  Initialise modem                                                    */
/* ------------------------------------------------------------------ */
static esp_err_t modem_setup(void)
{
    char resp[256];

    /* Autobaud sync */
    for (int i = 0; i < 5; i++) {
        if (at_cmd("AT", 1000, resp, sizeof(resp)) == ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    at_cmd("ATE0",          1000, NULL, 0);     /* Echo off              */
    at_cmd("AT+CMGF=1",     1000, NULL, 0);     /* Text mode SMS         */
    at_cmd("AT+CNMI=2,2,0,0,0", 1000, NULL, 0);/* Route SMS to serial   */
    at_cmd("AT+CREG=1",     1000, NULL, 0);     /* Enable registration URC */
    at_cmd("AT+CSCS=\"GSM\"",1000, NULL, 0);    /* GSM character set     */

    /* Read IMEI */
    if (at_cmd("AT+GSN", 2000, resp, sizeof(resp)) == ESP_OK) {
        /* Response is IMEI on first non-empty line */
        char *nl = strchr(resp, '\n');
        if (nl) *nl = '\0';
        strlcpy(s_modem.status.imei, resp, sizeof(s_modem.status.imei));
    }

    /* Read firmware */
    if (at_cmd("AT+CGMR", 2000, resp, sizeof(resp)) == ESP_OK) {
        char *nl = strchr(resp, '\n');
        if (nl) *nl = '\0';
        strlcpy(s_modem.status.firmware, resp, sizeof(s_modem.status.firmware));
    }

    /* Check SIM */
    if (at_cmd("AT+CPIN?", 3000, resp, sizeof(resp)) == ESP_OK) {
        s_modem.status.sim_ready = (strstr(resp, "READY") != NULL);
    }

    /* Wait for network registration */
    ESP_LOGI(TAG, "Waiting for network registration...");
    for (int i = 0; i < 30; i++) {
        if (at_cmd("AT+CREG?", 2000, resp, sizeof(resp)) == ESP_OK) {
            int n, stat;
            if (sscanf(resp, "+CREG: %d,%d", &n, &stat) == 2 ||
                sscanf(resp, "+CREG: %d", &stat) == 1) {
                if (stat == 1 || stat == 5) {
                    s_modem.status.registered = true;
                    ESP_LOGI(TAG, "Network registered.");
                    break;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (!s_modem.status.registered) {
        ESP_LOGW(TAG, "Failed to register on network.");
    }

    /* Read operator */
    at_cmd("AT+COPS=3,0", 2000, resp, sizeof(resp));
    if (at_cmd("AT+COPS?", 3000, resp, sizeof(resp)) == ESP_OK) {
        char *q1 = strchr(resp, '"');
        if (q1) {
            q1++;
            char *q2 = strchr(q1, '"');
            if (q2) {
                size_t len = q2 - q1;
                if (len >= sizeof(s_modem.status.operator_name))
                    len = sizeof(s_modem.status.operator_name) - 1;
                memcpy(s_modem.status.operator_name, q1, len);
                s_modem.status.operator_name[len] = '\0';
            }
        }
    }

    /* Signal quality */
    if (at_cmd("AT+CSQ", 2000, resp, sizeof(resp)) == ESP_OK) {
        int rssi, ber;
        if (sscanf(resp, "+CSQ: %d,%d", &rssi, &ber) == 2) {
            s_modem.status.rssi = rssi;
        }
    }

    s_modem.status.powered_on = true;
    ESP_LOGI(TAG, "Modem ready. IMEI=%s, Op=%s, RSSI=%d",
             s_modem.status.imei,
             s_modem.status.operator_name,
             s_modem.status.rssi);

    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */
esp_err_t a7670e_init(const a7670e_config_t *cfg)
{
    memcpy(&s_modem.cfg, cfg, sizeof(a7670e_config_t));

    s_modem.at_mutex        = xSemaphoreCreateMutex();
    s_modem.at_response_sem = xSemaphoreCreateBinary();

    /* UART init */
    uart_config_t uart_cfg = {
        .baud_rate  = cfg->baud_rate,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
    };
    ESP_ERROR_CHECK(uart_param_config(cfg->uart_num, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(cfg->uart_num, cfg->tx_pin, cfg->rx_pin,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(cfg->uart_num, UART_BUF_SIZE * 2,
                                         UART_BUF_SIZE * 2, 0, NULL, 0));

    /* Start UART reader task */
    xTaskCreate(uart_reader_task, "a7670e_rx", READER_TASK_STACK, NULL,
                READER_TASK_PRIO, NULL);

    /* Power on and configure modem */
    modem_power_on();
    return modem_setup();
}

esp_err_t a7670e_send_sms(const char *number, const char *text)
{
    if (!s_modem.status.registered) {
        ESP_LOGE(TAG, "Cannot send SMS: not registered on network.");
        return ESP_ERR_INVALID_STATE;
    }

    char cmd[64];
    snprintf(cmd, sizeof(cmd), "AT+CMGS=\"%s\"", number);

    xSemaphoreTake(s_modem.at_mutex, portMAX_DELAY);

    /* Send AT+CMGS="number" and wait for '>' prompt */
    memset(s_modem.at_response, 0, sizeof(s_modem.at_response));
    s_modem.waiting_for_response = true;

    uart_send(cmd);
    uart_send("\r\n");

    xSemaphoreTake(s_modem.at_response_sem, pdMS_TO_TICKS(5000));

    if (!strstr(s_modem.at_response, ">") && !strstr(s_modem.at_response, "> ")) {
        s_modem.waiting_for_response = false;
        xSemaphoreGive(s_modem.at_mutex);
        ESP_LOGE(TAG, "No '>' prompt from modem.");
        return ESP_FAIL;
    }

    /* Send message body + Ctrl-Z */
    memset(s_modem.at_response, 0, sizeof(s_modem.at_response));
    uart_send(text);
    uint8_t ctrlz = 0x1A;
    uart_send_raw(&ctrlz, 1);

    /* Wait for SEND OK */
    BaseType_t sent = xSemaphoreTake(s_modem.at_response_sem, pdMS_TO_TICKS(AT_LONG_TIMEOUT_MS));
    s_modem.waiting_for_response = false;
    xSemaphoreGive(s_modem.at_mutex);

    if (!sent || !strstr(s_modem.at_response, "OK")) {
        ESP_LOGE(TAG, "SMS send failed: %s", s_modem.at_response);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "SMS sent to %s.", number);
    return ESP_OK;
}

esp_err_t a7670e_get_status(a7670e_status_t *out_status)
{
    /* Refresh signal quality */
    char resp[64];
    if (at_cmd("AT+CSQ", 2000, resp, sizeof(resp)) == ESP_OK) {
        int rssi, ber;
        if (sscanf(resp, "+CSQ: %d,%d", &rssi, &ber) == 2) {
            s_modem.status.rssi = rssi;
        }
    }
    memcpy(out_status, &s_modem.status, sizeof(a7670e_status_t));
    return ESP_OK;
}

esp_err_t a7670e_delete_sms(int index)
{
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+CMGD=%d", index);
    return at_cmd(cmd, 3000, NULL, 0);
}

esp_err_t a7670e_reset(void)
{
    gpio_set_level(s_modem.cfg.reset_pin, 0);
    vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(s_modem.cfg.reset_pin, 1);
    vTaskDelay(pdMS_TO_TICKS(5000));
    return modem_setup();
}
