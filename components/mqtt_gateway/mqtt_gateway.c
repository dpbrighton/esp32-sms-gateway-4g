/**
 * mqtt_gateway.c
 *
 * MQTT client for ESP32 SMS Gateway.
 * Implements Home Assistant MQTT auto-discovery so all entities appear
 * automatically in HA with no manual YAML configuration required.
 *
 * Entities exposed to Home Assistant:
 *   - binary_sensor : Gateway online/offline (driven by LWT)
 *   - text          : Recipient phone number
 *   - text          : Message body
 *   - button        : Send SMS
 *   - sensor        : Last incoming sender
 *   - sensor        : Last incoming message body
 *   - sensor        : Last incoming timestamp
 *   - sensor        : Signal strength (dBm)
 *   - sensor        : Network operator
 *   - sensor        : Send status
 *   - sensor        : Messages sent count
 *   - sensor        : Messages received count
 *   - device_automation trigger : Message Received (fires on every inbound SMS)
 *   - device_automation action  : Send SMS (usable in HA automation action UI)
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "cJSON.h"

#include "mqtt_gateway.h"
#include "config.h"
#include "a7670e.h"
#include "sms_store.h"

static const char *TAG = "MQTT_GW";

#define TOPIC_FMT  "%s/%s"
#define HA_DISC_FMT "homeassistant/%s/sms_gateway_4g/%s/config"

static esp_mqtt_client_handle_t s_client     = NULL;
static bool                     s_connected  = false;
static char                     s_prefix[80] = "sms_gateway_4g";
static uint32_t                 s_sent_count = 0;
static char                     s_last_sent_to[64]   = {0};
static char                     s_last_sent_body[256] = {0};
static uint32_t                 s_recv_count = 0;

static char s_number_buf[64]   = {0};
static char s_message_buf[320] = {0};

static void publish_discovery(void);
static void mqtt_gw_publish_sent_details(const char *number, const char *body);
static void publish_online(void);
static void handle_incoming_command(const char *topic, const char *data, int data_len);
static void status_timer_cb(TimerHandle_t xTimer);

static int pub(const char *topic, const char *payload, int qos, int retain)
{
    if (!s_connected || !s_client) return -1;
    return esp_mqtt_client_publish(s_client, topic, payload, 0, qos, retain);
}

static void build_topic(char *buf, size_t len, const char *suffix)
{
    snprintf(buf, len, TOPIC_FMT, s_prefix, suffix);
}

static int pub_rel(const char *suffix, const char *payload, int qos, int retain)
{
    char topic[128];
    build_topic(topic, sizeof(topic), suffix);
    return pub(topic, payload, qos, retain);
}

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {

    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Connected to broker.");
        s_connected = true;

        {
            char t[128];
            build_topic(t, sizeof(t), "send");
            esp_mqtt_client_subscribe(s_client, t, 1);
            build_topic(t, sizeof(t), "send_button/press");
            esp_mqtt_client_subscribe(s_client, t, 1);
            build_topic(t, sizeof(t), "number/set");
            esp_mqtt_client_subscribe(s_client, t, 1);
            build_topic(t, sizeof(t), "message/set");
            esp_mqtt_client_subscribe(s_client, t, 1);
        }

        publish_online();
        /* Only publish discovery on first connect - HA retains them */
        static bool s_discovery_done = false;
        if (!s_discovery_done) {
            s_discovery_done = true;
            publish_discovery();
        }

        pub_rel("number/state",      s_number_buf[0]  ? s_number_buf  : "", 1, 1);
        pub_rel("message/state",     s_message_buf[0] ? s_message_buf : "", 1, 1);
        pub_rel("send_button/state", "idle", 1, 1);
        mqtt_gw_publish_status();
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Disconnected from broker.");
        s_connected = false;
        break;

    case MQTT_EVENT_DATA:
        if (event->topic && event->data) {
            handle_incoming_command(event->topic, event->data, event->data_len);
        }
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error.");
        break;

    default:
        break;
    }
}

static void handle_incoming_command(const char *topic, const char *data, int data_len)
{
    char t_send[128], t_button[128], t_number[128], t_message[128];
    build_topic(t_send,    sizeof(t_send),    "send");
    build_topic(t_button,  sizeof(t_button),  "send_button/press");
    build_topic(t_number,  sizeof(t_number),  "number/set");
    build_topic(t_message, sizeof(t_message), "message/set");

    if (strncmp(topic, t_number, strlen(t_number)) == 0) {
        size_t len = data_len < (int)sizeof(s_number_buf) - 1
                     ? data_len : sizeof(s_number_buf) - 1;
        memcpy(s_number_buf, data, len);
        s_number_buf[len] = '\0';
        pub_rel("number/state", s_number_buf, 1, 1);
        ESP_LOGI(TAG, "Number set to: %s", s_number_buf);
        return;
    }

    if (strncmp(topic, t_message, strlen(t_message)) == 0) {
        size_t len = data_len < (int)sizeof(s_message_buf) - 1
                     ? data_len : sizeof(s_message_buf) - 1;
        memcpy(s_message_buf, data, len);
        s_message_buf[len] = '\0';
        pub_rel("message/state", s_message_buf, 1, 1);
        ESP_LOGI(TAG, "Message set to: %s", s_message_buf);
        return;
    }

    if (strncmp(topic, t_button, strlen(t_button)) == 0) {
        if (s_number_buf[0] == '\0') {
            ESP_LOGW(TAG, "Send button pressed but number is empty.");
            mqtt_gw_set_send_status("failed");
            return;
        }
        if (s_message_buf[0] == '\0') {
            ESP_LOGW(TAG, "Send button pressed but message is empty.");
            mqtt_gw_set_send_status("failed");
            return;
        }
        ESP_LOGI(TAG, "Send button: sending to %s", s_number_buf);
        mqtt_gw_set_send_status("sending");
        pub_rel("send_button/state", "sending", 1, 1);

        esp_err_t err = a7670e_send_sms(s_number_buf, s_message_buf);
        if (err == ESP_OK) {
            s_sent_count++;
            mqtt_gw_set_send_status("sent");
            mqtt_gw_publish_sent_details(s_number_buf, s_message_buf);
            sms_message_t out = {0};
            out.id        = sms_store_next_id();
            out.direction = SMS_DIR_OUTBOUND;
            out.status    = SMS_STATUS_SENT;
            strlcpy(out.sender, s_number_buf,  sizeof(out.sender));
            strlcpy(out.body,   s_message_buf, sizeof(out.body));
            sms_store_outbox_add(&out);
        } else {
            mqtt_gw_set_send_status("failed");
        }
        pub_rel("send_button/state", "idle", 1, 1);
        return;
    }

    if (strncmp(topic, t_send, strlen(t_send)) == 0) {
        char *buf = malloc(data_len + 1);
        if (!buf) return;
        memcpy(buf, data, data_len);
        buf[data_len] = '\0';

        cJSON *root = cJSON_Parse(buf);
        free(buf);
        if (!root) {
            ESP_LOGE(TAG, "send: invalid JSON");
            return;
        }

        cJSON *j_num  = cJSON_GetObjectItem(root, "number");
        cJSON *j_body = cJSON_GetObjectItem(root, "body");

        if (!cJSON_IsString(j_num) || !cJSON_IsString(j_body)) {
            ESP_LOGE(TAG, "send: JSON missing 'number' or 'body'");
            cJSON_Delete(root);
            return;
        }

        const char *number = j_num->valuestring;
        const char *body   = j_body->valuestring;
        ESP_LOGI(TAG, "Automation send to %s", number);

        mqtt_gw_set_send_status("sending");
        esp_err_t err = a7670e_send_sms(number, body);
        if (err == ESP_OK) {
            s_sent_count++;
            mqtt_gw_set_send_status("sent");
            mqtt_gw_publish_sent_details(number, body);
            sms_message_t out = {0};
            out.id        = sms_store_next_id();
            out.direction = SMS_DIR_OUTBOUND;
            out.status    = SMS_STATUS_SENT;
            strlcpy(out.sender, number, sizeof(out.sender));
            strlcpy(out.body,   body,   sizeof(out.body));
            sms_store_outbox_add(&out);
        } else {
            mqtt_gw_set_send_status("failed");
        }
        cJSON_Delete(root);
        return;
    }
}

static void publish_online(void)
{
    pub_rel("status", "online", 1, 1);
}

static void mqtt_gw_publish_sent_details(const char *number, const char *body)
{
    strlcpy(s_last_sent_to,   number, sizeof(s_last_sent_to));
    strlcpy(s_last_sent_body, body,   sizeof(s_last_sent_body));
    pub_rel("last_sent_to",   s_last_sent_to,   1, 1);
    pub_rel("last_sent_body", s_last_sent_body, 1, 1);
}

void mqtt_gw_set_send_status(const char *status)
{
    pub_rel("send_status", status, 1, 1);
}

bool mqtt_gw_is_connected(void)
{
    return s_connected;
}

void mqtt_gw_publish_status(void)
{
    if (!s_connected) return;

    a7670e_status_t st = {0};
    if (a7670e_get_status(&st) == ESP_OK) {
        char buf[16];
        int dbm = (st.rssi == 99) ? -999 : (st.rssi * 2) - 113;
        snprintf(buf, sizeof(buf), "%d", dbm);
        pub_rel("signal",   buf,              1, 1);
        pub_rel("operator", st.operator_name, 1, 1);
    }

    char buf[16];
    snprintf(buf, sizeof(buf), "%" PRIu32, s_sent_count);
    pub_rel("sent_count", buf, 1, 1);

    snprintf(buf, sizeof(buf), "%" PRIu32, s_recv_count);
    pub_rel("received_count", buf, 1, 1);
}

static void status_timer_cb(TimerHandle_t xTimer)
{
    (void)xTimer;
    mqtt_gw_publish_status();
}

void mqtt_gw_publish_inbound(const sms_message_t *msg)
{
    if (!s_connected || !msg) return;

    s_recv_count++;

    pub_rel("incoming/from",  msg->sender,    1, 1);
    pub_rel("incoming/body",  msg->body,      1, 1);
    pub_rel("incoming/time",  msg->timestamp, 1, 1);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "from",      msg->sender);
    cJSON_AddStringToObject(root, "body",      msg->body);
    cJSON_AddStringToObject(root, "timestamp", msg->timestamp);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json) {
        pub_rel("incoming", json, 1, 0);
        free(json);
    }

    char evt_topic[160];
    snprintf(evt_topic, sizeof(evt_topic),
             "homeassistant/device_automation/sms_gateway_4g/message_received/event");

    cJSON *evt = cJSON_CreateObject();
    cJSON_AddStringToObject(evt, "from",      msg->sender);
    cJSON_AddStringToObject(evt, "body",      msg->body);
    cJSON_AddStringToObject(evt, "timestamp", msg->timestamp);
    char *evt_json = cJSON_PrintUnformatted(evt);
    cJSON_Delete(evt);
    if (evt_json) {
        pub(evt_topic, evt_json, 1, 0);
        free(evt_json);
    }

    char buf[16];
    snprintf(buf, sizeof(buf), "%" PRIu32, s_recv_count);
    pub_rel("received_count", buf, 1, 1);

    ESP_LOGI(TAG, "Inbound SMS published from %s", msg->sender);
}

static cJSON *build_device_block(void)
{
    cJSON *dev = cJSON_CreateObject();
    cJSON *ids = cJSON_CreateArray();
    cJSON_AddItemToArray(ids, cJSON_CreateString("sms_gateway_4g"));
    cJSON_AddItemToObject(dev, "identifiers",  ids);
    cJSON_AddStringToObject(dev, "name",         "SMS Gateway 4G");
    cJSON_AddStringToObject(dev, "model",        "ESP32 + A7670E");
    cJSON_AddStringToObject(dev, "manufacturer", "DIY");
    return dev;
}

static void disc_pub(const char *ha_component, const char *object_id, cJSON *payload)
{
    char topic[160];
    snprintf(topic, sizeof(topic), HA_DISC_FMT, ha_component, object_id);
    char *json = cJSON_PrintUnformatted(payload);
    if (json) {
        pub(topic, json, 1, 1);
        free(json);
    }
}

static void publish_discovery(void)
{
    char state_topic[128];
    char cmd_topic[128];

    ESP_LOGI(TAG, "Publishing HA auto-discovery payloads...");

    /* 1. Gateway online/offline binary sensor */
    {
        cJSON *p = cJSON_CreateObject();
        build_topic(state_topic, sizeof(state_topic), "status");
        cJSON_AddStringToObject(p, "name",         "Gateway Status");
        cJSON_AddStringToObject(p, "unique_id",    "sms_gw4g_status");
        cJSON_AddStringToObject(p, "state_topic",  state_topic);
        cJSON_AddStringToObject(p, "payload_on",   "online");
        cJSON_AddStringToObject(p, "payload_off",  "offline");
        cJSON_AddStringToObject(p, "device_class", "connectivity");
        cJSON_AddItemToObject(p,   "device",       build_device_block());
        disc_pub("binary_sensor", "gateway_status", p);
        cJSON_Delete(p);
    }

    /* 2. Recipient number text entity */
    {
        cJSON *p = cJSON_CreateObject();
        build_topic(state_topic, sizeof(state_topic), "number/state");
        build_topic(cmd_topic,   sizeof(cmd_topic),   "number/set");
        cJSON_AddStringToObject(p, "name",          "Recipient Number");
        cJSON_AddStringToObject(p, "unique_id",     "sms_gw4g_number");
        cJSON_AddStringToObject(p, "state_topic",   state_topic);
        cJSON_AddStringToObject(p, "command_topic", cmd_topic);
        cJSON_AddNumberToObject(p, "min",           0);
        cJSON_AddNumberToObject(p, "max",           20);
        cJSON_AddStringToObject(p, "pattern",       "^[+0-9]{0,20}$");
        cJSON_AddStringToObject(p, "icon",          "mdi:phone");
        cJSON_AddItemToObject(p,   "device",        build_device_block());
        disc_pub("text", "number", p);
        cJSON_Delete(p);
    }

    /* 3. Message body text entity */
    {
        cJSON *p = cJSON_CreateObject();
        build_topic(state_topic, sizeof(state_topic), "message/state");
        build_topic(cmd_topic,   sizeof(cmd_topic),   "message/set");
        cJSON_AddStringToObject(p, "name",          "Outgoing Message");
        cJSON_AddStringToObject(p, "unique_id",     "sms_gw4g_message");
        cJSON_AddStringToObject(p, "state_topic",   state_topic);
        cJSON_AddStringToObject(p, "command_topic", cmd_topic);
        cJSON_AddNumberToObject(p, "min",           0);
        cJSON_AddNumberToObject(p, "max",           255);
        cJSON_AddStringToObject(p, "mode",          "text");
        cJSON_AddStringToObject(p, "pattern",       "^[\\s\\S]{0,320}$");
        cJSON_AddStringToObject(p, "icon",          "mdi:message-text");
        cJSON_AddItemToObject(p,   "device",        build_device_block());
        disc_pub("text", "message", p);
        cJSON_Delete(p);
    }

    /* 4. Send button */
    {
        cJSON *p = cJSON_CreateObject();
        build_topic(cmd_topic, sizeof(cmd_topic), "send_button/press");
        cJSON_AddStringToObject(p, "name",          "Send SMS");
        cJSON_AddStringToObject(p, "unique_id",     "sms_gw4g_send_btn");
        cJSON_AddStringToObject(p, "command_topic", cmd_topic);
        cJSON_AddStringToObject(p, "payload_press", "PRESS");
        cJSON_AddStringToObject(p, "icon",          "mdi:send");
        cJSON_AddItemToObject(p,   "device",        build_device_block());
        disc_pub("button", "send_button", p);
        cJSON_Delete(p);
    }

    /* 5. Incoming sender */
    {
        cJSON *p = cJSON_CreateObject();
        build_topic(state_topic, sizeof(state_topic), "incoming/from");
        cJSON_AddStringToObject(p, "name",        "Last Sender");
        cJSON_AddStringToObject(p, "unique_id",   "sms_gw4g_incoming_from");
        cJSON_AddStringToObject(p, "state_topic", state_topic);
        cJSON_AddStringToObject(p, "icon",        "mdi:phone-incoming");
        cJSON_AddItemToObject(p,   "device",      build_device_block());
        disc_pub("sensor", "incoming_from", p);
        cJSON_Delete(p);
    }

    /* 6. Incoming message body */
    {
        cJSON *p = cJSON_CreateObject();
        build_topic(state_topic, sizeof(state_topic), "incoming/body");
        cJSON_AddStringToObject(p, "name",        "Last Message");
        cJSON_AddStringToObject(p, "unique_id",   "sms_gw4g_incoming_body");
        cJSON_AddStringToObject(p, "state_topic", state_topic);
        cJSON_AddStringToObject(p, "icon",        "mdi:message-arrow-left");
        cJSON_AddItemToObject(p,   "device",      build_device_block());
        disc_pub("sensor", "incoming_body", p);
        cJSON_Delete(p);
    }

    /* 7. Incoming timestamp */
    {
        cJSON *p = cJSON_CreateObject();
        build_topic(state_topic, sizeof(state_topic), "incoming/time");
        cJSON_AddStringToObject(p, "name",        "Last Received Time");
        cJSON_AddStringToObject(p, "unique_id",   "sms_gw4g_incoming_time");
        cJSON_AddStringToObject(p, "state_topic", state_topic);
        cJSON_AddStringToObject(p, "icon",        "mdi:clock-in");
        cJSON_AddItemToObject(p,   "device",      build_device_block());
        disc_pub("sensor", "incoming_time", p);
        cJSON_Delete(p);
    }

    /* 8. Signal strength */
    {
        cJSON *p = cJSON_CreateObject();
        build_topic(state_topic, sizeof(state_topic), "signal");
        cJSON_AddStringToObject(p, "name",                "Signal Strength");
        cJSON_AddStringToObject(p, "unique_id",           "sms_gw4g_signal");
        cJSON_AddStringToObject(p, "state_topic",         state_topic);
        cJSON_AddStringToObject(p, "unit_of_measurement", "dBm");
        cJSON_AddStringToObject(p, "device_class",        "signal_strength");
        cJSON_AddStringToObject(p, "state_class",         "measurement");
        cJSON_AddStringToObject(p, "entity_category",     "diagnostic");
        cJSON_AddItemToObject(p,   "device",              build_device_block());
        disc_pub("sensor", "signal", p);
        cJSON_Delete(p);
    }

    /* 9. Network operator */
    {
        cJSON *p = cJSON_CreateObject();
        build_topic(state_topic, sizeof(state_topic), "operator");
        cJSON_AddStringToObject(p, "name",            "Network Operator");
        cJSON_AddStringToObject(p, "unique_id",       "sms_gw4g_operator");
        cJSON_AddStringToObject(p, "state_topic",     state_topic);
        cJSON_AddStringToObject(p, "icon",            "mdi:antenna");
        cJSON_AddStringToObject(p, "entity_category", "diagnostic");
        cJSON_AddItemToObject(p,   "device",          build_device_block());
        disc_pub("sensor", "operator", p);
        cJSON_Delete(p);
    }

    /* 10. Send status */
    {
        cJSON *p = cJSON_CreateObject();
        build_topic(state_topic, sizeof(state_topic), "send_status");
        cJSON_AddStringToObject(p, "name",        "Send Status");
        cJSON_AddStringToObject(p, "unique_id",   "sms_gw4g_send_status");
        cJSON_AddStringToObject(p, "state_topic", state_topic);
        cJSON_AddStringToObject(p, "icon",        "mdi:message-check");
        cJSON_AddItemToObject(p,   "device",      build_device_block());
        disc_pub("sensor", "send_status", p);
        cJSON_Delete(p);
    }


    /* 10b. Last sent to */
    {
        cJSON *p = cJSON_CreateObject();
        build_topic(state_topic, sizeof(state_topic), "last_sent_to");
        cJSON_AddStringToObject(p, "name",        "Last Sent To");
        cJSON_AddStringToObject(p, "unique_id",   "sms_gw4g_last_sent_to");
        cJSON_AddStringToObject(p, "state_topic", state_topic);
        cJSON_AddStringToObject(p, "icon",        "mdi:phone-outgoing");
        cJSON_AddItemToObject(p,   "device",      build_device_block());
        disc_pub("sensor", "last_sent_to", p);
        cJSON_Delete(p);
    }

    /* 10c. Last sent body */
    {
        cJSON *p = cJSON_CreateObject();
        build_topic(state_topic, sizeof(state_topic), "last_sent_body");
        cJSON_AddStringToObject(p, "name",        "Last Sent Message");
        cJSON_AddStringToObject(p, "unique_id",   "sms_gw4g_last_sent_body");
        cJSON_AddStringToObject(p, "state_topic", state_topic);
        cJSON_AddStringToObject(p, "icon",        "mdi:message-arrow-right");
        cJSON_AddItemToObject(p,   "device",      build_device_block());
        disc_pub("sensor", "last_sent_body", p);
        cJSON_Delete(p);
    }

    /* 11. Messages sent count */
    {
        cJSON *p = cJSON_CreateObject();
        build_topic(state_topic, sizeof(state_topic), "sent_count");
        cJSON_AddStringToObject(p, "name",                "Messages Sent");
        cJSON_AddStringToObject(p, "unique_id",           "sms_gw4g_sent_count");
        cJSON_AddStringToObject(p, "state_topic",         state_topic);
        cJSON_AddStringToObject(p, "unit_of_measurement", "messages");
        cJSON_AddStringToObject(p, "state_class",         "total_increasing");
        cJSON_AddStringToObject(p, "icon",                "mdi:message-arrow-right");
        cJSON_AddStringToObject(p, "entity_category",     "diagnostic");
        cJSON_AddItemToObject(p,   "device",              build_device_block());
        disc_pub("sensor", "sent_count", p);
        cJSON_Delete(p);
    }

    /* 12. Messages received count */
    {
        cJSON *p = cJSON_CreateObject();
        build_topic(state_topic, sizeof(state_topic), "received_count");
        cJSON_AddStringToObject(p, "name",                "Messages Received");
        cJSON_AddStringToObject(p, "unique_id",           "sms_gw4g_received_count");
        cJSON_AddStringToObject(p, "state_topic",         state_topic);
        cJSON_AddStringToObject(p, "unit_of_measurement", "messages");
        cJSON_AddStringToObject(p, "state_class",         "total_increasing");
        cJSON_AddStringToObject(p, "icon",                "mdi:message-arrow-left");
        cJSON_AddStringToObject(p, "entity_category",     "diagnostic");
        cJSON_AddItemToObject(p,   "device",              build_device_block());
        disc_pub("sensor", "received_count", p);
        cJSON_Delete(p);
    }

    /* 13. Device automation TRIGGER: message received */
    {
        char disc_topic[160];
        snprintf(disc_topic, sizeof(disc_topic),
                 "homeassistant/device_automation/sms_gateway_4g/message_received/config");
        char event_topic[128];
        snprintf(event_topic, sizeof(event_topic),
                 "homeassistant/device_automation/sms_gateway_4g/message_received/event");
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "automation_type", "trigger");
        cJSON_AddStringToObject(p, "type",            "message_received");
        cJSON_AddStringToObject(p, "subtype",         "message_received");
        cJSON_AddStringToObject(p, "topic",           event_topic);
        cJSON_AddStringToObject(p, "platform",        "device");
        cJSON_AddItemToObject(p,   "device",          build_device_block());
        char *json = cJSON_PrintUnformatted(p);
        if (json) { pub(disc_topic, json, 1, 1); free(json); }
        cJSON_Delete(p);
    }

    /* 14. Device automation ACTION: send SMS */
    {
        char disc_topic[160];
        snprintf(disc_topic, sizeof(disc_topic),
                 "homeassistant/device_automation/sms_gateway_4g/send_sms/config");
        build_topic(cmd_topic, sizeof(cmd_topic), "send");
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "automation_type", "action");
        cJSON_AddStringToObject(p, "type",            "send_sms");
        cJSON_AddStringToObject(p, "subtype",         "send_sms");
        cJSON_AddStringToObject(p, "topic",           cmd_topic);
        cJSON_AddStringToObject(p, "platform",        "device");
        cJSON_AddItemToObject(p,   "device",          build_device_block());
        char *json = cJSON_PrintUnformatted(p);
        if (json) { pub(disc_topic, json, 1, 1); free(json); }
        cJSON_Delete(p);
    }

    ESP_LOGI(TAG, "HA auto-discovery complete - 16 entities registered.");
}

void mqtt_gw_wifi_connected(void)
{
    if (s_client) {
        ESP_LOGI(TAG, "WiFi up - starting MQTT client.");
        esp_mqtt_client_start(s_client);
    }
}

void mqtt_gw_wifi_disconnected(void)
{
    if (s_client) {
        ESP_LOGI(TAG, "WiFi down - stopping MQTT client.");
        s_connected = false;
        esp_mqtt_client_stop(s_client);
    }
}

void mqtt_gw_init(void)
{
    gw_config_t *cfg = gw_config_get();

    if (cfg->mqtt_broker_uri[0] == '\0') {
        ESP_LOGW(TAG, "No MQTT broker configured - MQTT disabled.");
        return;
    }

    if (cfg->mqtt_topic_prefix[0] != '\0') {
        strlcpy(s_prefix, cfg->mqtt_topic_prefix, sizeof(s_prefix));
    }

    char lwt_topic[128];
    build_topic(lwt_topic, sizeof(lwt_topic), "status");

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri                  = cfg->mqtt_broker_uri,
        .credentials.username                = cfg->mqtt_username,
        .credentials.authentication.password = cfg->mqtt_password,
        .session.last_will.topic             = lwt_topic,
        .session.last_will.msg               = "offline",
        .session.last_will.msg_len           = 7,
        .session.last_will.qos               = 1,
        .session.last_will.retain            = 1,
        .session.keepalive                   = 120,
        .network.reconnect_timeout_ms        = 10000,
    };

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "Failed to create MQTT client.");
        return;
    }

    ESP_ERROR_CHECK(esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                                    mqtt_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(s_client));

    TimerHandle_t timer = xTimerCreate("mqtt_status", pdMS_TO_TICKS(60000),
                                        pdTRUE, NULL, status_timer_cb);
    if (timer) xTimerStart(timer, 0);

    ESP_LOGI(TAG, "MQTT client started. Broker: %s  Prefix: %s",
             cfg->mqtt_broker_uri, s_prefix);
}
