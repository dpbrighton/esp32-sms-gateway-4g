#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "sms_store.h"

static const char *TAG = "SMS_STORE";

#define CONFIG_SMS_MAX_INBOX  50
#define CONFIG_SMS_MAX_OUTBOX 50

static sms_message_t s_inbox[CONFIG_SMS_MAX_INBOX];
static int           s_inbox_count = 0;
static sms_message_t s_outbox[CONFIG_SMS_MAX_OUTBOX];
static int           s_outbox_count = 0;
static SemaphoreHandle_t s_mutex;
static uint32_t          s_id_counter = 1;

void sms_store_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    memset(s_inbox,  0, sizeof(s_inbox));
    memset(s_outbox, 0, sizeof(s_outbox));
    ESP_LOGI(TAG, "SMS store initialised.");
}

uint32_t sms_store_next_id(void) { return s_id_counter++; }

esp_err_t sms_store_inbox_add(const sms_message_t *msg)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_inbox_count >= CONFIG_SMS_MAX_INBOX) {
        memmove(&s_inbox[0], &s_inbox[1], sizeof(sms_message_t) * (CONFIG_SMS_MAX_INBOX - 1));
        s_inbox_count = CONFIG_SMS_MAX_INBOX - 1;
    }
    memcpy(&s_inbox[s_inbox_count], msg, sizeof(sms_message_t));
    s_inbox[s_inbox_count].id = sms_store_next_id();
    s_inbox[s_inbox_count].status = SMS_STATUS_UNREAD;
    s_inbox_count++;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

int sms_store_inbox_count(void) { return s_inbox_count; }

bool sms_store_inbox_get(int index, sms_message_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool ok = (index >= 0 && index < s_inbox_count);
    if (ok) memcpy(out, &s_inbox[index], sizeof(sms_message_t));
    xSemaphoreGive(s_mutex);
    return ok;
}

esp_err_t sms_store_inbox_mark_read(uint32_t id)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_inbox_count; i++) {
        if (s_inbox[i].id == id) {
            s_inbox[i].status = SMS_STATUS_READ;
            xSemaphoreGive(s_mutex);
            return ESP_OK;
        }
    }
    xSemaphoreGive(s_mutex);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t sms_store_inbox_delete(uint32_t id)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_inbox_count; i++) {
        if (s_inbox[i].id == id) {
            memmove(&s_inbox[i], &s_inbox[i+1],
                    sizeof(sms_message_t) * (s_inbox_count - i - 1));
            s_inbox_count--;
            xSemaphoreGive(s_mutex);
            return ESP_OK;
        }
    }
    xSemaphoreGive(s_mutex);
    return ESP_ERR_NOT_FOUND;
}

void sms_store_inbox_clear(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_inbox_count = 0;
    xSemaphoreGive(s_mutex);
}

esp_err_t sms_store_outbox_add(const sms_message_t *msg)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_outbox_count >= CONFIG_SMS_MAX_OUTBOX) {
        memmove(&s_outbox[0], &s_outbox[1], sizeof(sms_message_t) * (CONFIG_SMS_MAX_OUTBOX - 1));
        s_outbox_count = CONFIG_SMS_MAX_OUTBOX - 1;
    }
    memcpy(&s_outbox[s_outbox_count], msg, sizeof(sms_message_t));
    s_outbox[s_outbox_count].id = sms_store_next_id();
    s_outbox_count++;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

int sms_store_outbox_count(void) { return s_outbox_count; }

bool sms_store_outbox_get(int index, sms_message_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool ok = (index >= 0 && index < s_outbox_count);
    if (ok) memcpy(out, &s_outbox[index], sizeof(sms_message_t));
    xSemaphoreGive(s_mutex);
    return ok;
}

esp_err_t sms_store_outbox_delete(uint32_t id)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_outbox_count; i++) {
        if (s_outbox[i].id == id) {
            memmove(&s_outbox[i], &s_outbox[i+1],
                    sizeof(sms_message_t) * (s_outbox_count - i - 1));
            s_outbox_count--;
            xSemaphoreGive(s_mutex);
            return ESP_OK;
        }
    }
    xSemaphoreGive(s_mutex);
    return ESP_ERR_NOT_FOUND;
}

void sms_store_outbox_clear(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_outbox_count = 0;
    xSemaphoreGive(s_mutex);
}
