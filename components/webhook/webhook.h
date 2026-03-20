#pragma once
#include "sms_store.h"

#ifdef __cplusplus
extern "C" {
#endif

void webhook_init(void);
void webhook_send(const sms_message_t *msg);

#ifdef __cplusplus
}
#endif
