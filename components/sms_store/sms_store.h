#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SMS_SENDER_MAX_LEN   32
#define SMS_BODY_MAX_LEN     320
#define SMS_TIMESTAMP_MAX    24

typedef enum {
    SMS_DIR_INBOUND  = 0,
    SMS_DIR_OUTBOUND = 1,
} sms_direction_t;

typedef enum {
    SMS_STATUS_UNREAD   = 0,
    SMS_STATUS_READ     = 1,
    SMS_STATUS_SENT     = 2,
    SMS_STATUS_FAILED   = 3,
    SMS_STATUS_PENDING  = 4,
} sms_status_t;

typedef struct {
    uint32_t          id;
    int               index;
    sms_direction_t   direction;
    sms_status_t      status;
    char              sender[SMS_SENDER_MAX_LEN];
    char              body[SMS_BODY_MAX_LEN];
    char              timestamp[SMS_TIMESTAMP_MAX];
} sms_message_t;

void      sms_store_init(void);
esp_err_t sms_store_inbox_add(const sms_message_t *msg);
int       sms_store_inbox_count(void);
bool      sms_store_inbox_get(int index, sms_message_t *out);
esp_err_t sms_store_inbox_mark_read(uint32_t id);
esp_err_t sms_store_inbox_delete(uint32_t id);
void      sms_store_inbox_clear(void);
esp_err_t sms_store_outbox_add(const sms_message_t *msg);
int       sms_store_outbox_count(void);
bool      sms_store_outbox_get(int index, sms_message_t *out);
esp_err_t sms_store_outbox_delete(uint32_t id);
void      sms_store_outbox_clear(void);
uint32_t  sms_store_next_id(void);

#ifdef __cplusplus
}
#endif
