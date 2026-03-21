#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "sms_store.h"
#include "a7670e.h"
#include "config.h"
#include "ota_update.h"
#include "rest_api.h"

static const char *TAG = "REST_API";
#define JSON_BUF_SIZE  8192
#define BODY_BUF_SIZE  512
#define CFG_BUF_SIZE   1024

static void json_resp(httpd_req_t *req, int code, const char *json)
{
    httpd_resp_set_status(req, code==200?"200 OK":code==400?"400 Bad Request":
                               code==401?"401 Unauthorized":code==404?"404 Not Found":
                               "500 Internal Server Error");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, json, strlen(json));
}

static bool check_auth(httpd_req_t *req)
{
    char auth[80]={0};
    if(httpd_req_get_hdr_value_str(req,"Authorization",auth,sizeof(auth))!=ESP_OK) return false;
    gw_config_t *cfg=gw_config_get();
    char expected[80];
    snprintf(expected,sizeof(expected),"Bearer %s",cfg->api_key);
    return strcmp(auth,expected)==0;
}

static int read_body(httpd_req_t *req, char *buf, size_t buf_size)
{
    int total=req->content_len;
    if(total<=0||total>=(int)buf_size) return -1;
    int received=0;
    while(received<total){
        int r=httpd_req_recv(req,buf+received,total-received);
        if(r<=0) return -1;
        received+=r;
    }
    buf[received]='\0';
    return received;
}

static bool json_get_str(const char *json, const char *key, char *out, size_t out_len)
{
    char search[64];
    snprintf(search,sizeof(search),"\"%s\"",key);
    const char *p=strstr(json,search);
    if(!p) return false;
    p+=strlen(search);
    while(*p==' '||*p==':') p++;
    if(*p!='"') return false;
    p++;
    const char *end=strchr(p,'"');
    if(!end) return false;
    size_t len=end-p;
    if(len>=out_len) len=out_len-1;
    memcpy(out,p,len);
    out[len]='\0';
    return true;
}

static int sms_to_json(char *buf, size_t buf_size, const sms_message_t *m)
{
    return snprintf(buf,buf_size,
        "{\"id\":%lu,\"direction\":%d,\"status\":%d,"
        "\"sender\":\"%s\",\"body\":\"%s\",\"timestamp\":\"%s\"}",
        (unsigned long)m->id,m->direction,m->status,
        m->sender,m->body,m->timestamp);
}

static esp_err_t handler_status(httpd_req_t *req)
{
    a7670e_status_t st={0};
    a7670e_get_status(&st);
    char buf[512];
    snprintf(buf,sizeof(buf),
        "{\"powered_on\":%s,\"sim_ready\":%s,\"registered\":%s,"
        "\"rssi\":%d,\"operator\":\"%s\",\"imei\":\"%s\",\"firmware\":\"%s\"}",
        st.powered_on?"true":"false",st.sim_ready?"true":"false",
        st.registered?"true":"false",
        (st.rssi == 99) ? -999 : (st.rssi * 2) - 113,st.operator_name,st.imei,st.firmware);
    json_resp(req,200,buf);
    return ESP_OK;
}

static esp_err_t handler_sms_list(httpd_req_t *req, bool inbox)
{
    char *buf=malloc(JSON_BUF_SIZE);
    if(!buf){json_resp(req,500,"{\"error\":\"OOM\"}");return ESP_OK;}
    int count=inbox?sms_store_inbox_count():sms_store_outbox_count();
    int pos=0;
    buf[pos++]='[';
    for(int i=count-1;i>=0;i--){
        sms_message_t m;
        bool got=inbox?sms_store_inbox_get(i,&m):sms_store_outbox_get(i,&m);
        if(!got) continue;
        if(pos>1) buf[pos++]=',';
        pos+=sms_to_json(buf+pos,JSON_BUF_SIZE-pos-2,&m);
        if(pos>=JSON_BUF_SIZE-200) break;
    }
    buf[pos++]=']';
    buf[pos]='\0';
    json_resp(req,200,buf);
    free(buf);
    return ESP_OK;
}

static esp_err_t handler_inbox(httpd_req_t *req)  {return handler_sms_list(req,true);}
static esp_err_t handler_outbox(httpd_req_t *req) {return handler_sms_list(req,false);}

static esp_err_t handler_send(httpd_req_t *req)
{
    if(!check_auth(req)){json_resp(req,401,"{\"error\":\"Unauthorized\"}");return ESP_OK;}
    char body[BODY_BUF_SIZE];
    if(read_body(req,body,sizeof(body))<0){json_resp(req,400,"{\"error\":\"Bad body\"}");return ESP_OK;}
    char to[32]={0},text[320]={0};
    if(!json_get_str(body,"to",to,sizeof(to))||!json_get_str(body,"body",text,sizeof(text))){
        json_resp(req,400,"{\"error\":\"Missing to or body\"}");return ESP_OK;
    }
    esp_err_t err=a7670e_send_sms(to,text);
    sms_message_t msg={0};
    msg.direction=SMS_DIR_OUTBOUND;
    msg.status=(err==ESP_OK)?SMS_STATUS_SENT:SMS_STATUS_FAILED;
    strlcpy(msg.sender,to,sizeof(msg.sender));
    strlcpy(msg.body,text,sizeof(msg.body));
    sms_store_outbox_add(&msg);
    json_resp(req,200,err==ESP_OK?"{\"success\":true}":"{\"success\":false,\"error\":\"Send failed\"}");
    return ESP_OK;
}

static esp_err_t handler_delete(httpd_req_t *req)
{
    if(!check_auth(req)){json_resp(req,401,"{\"error\":\"Unauthorized\"}");return ESP_OK;}
    const char *last=strrchr(req->uri,'/');
    if(!last){json_resp(req,400,"{\"error\":\"Bad URI\"}");return ESP_OK;}
    uint32_t id=(uint32_t)atol(last+1);
    if(id==0){json_resp(req,400,"{\"error\":\"Invalid ID\"}");return ESP_OK;}
    if(sms_store_inbox_delete(id)==ESP_OK||sms_store_outbox_delete(id)==ESP_OK)
        json_resp(req,200,"{\"success\":true}");
    else
        json_resp(req,404,"{\"error\":\"Not found\"}");
    return ESP_OK;
}

static esp_err_t handler_config_get(httpd_req_t *req)
{
    if(!check_auth(req)){json_resp(req,401,"{\"error\":\"Unauthorized\"}");return ESP_OK;}
    gw_config_t *cfg=gw_config_get();
    char *buf=malloc(CFG_BUF_SIZE);
    if(!buf){json_resp(req,500,"{\"error\":\"OOM\"}");return ESP_OK;}
    snprintf(buf,CFG_BUF_SIZE,
        "{\"wifi_ssid\":\"%s\",\"mqtt_broker_uri\":\"%s\","
        "\"mqtt_topic_prefix\":\"%s\",\"webhook_url\":\"%s\","
        "\"webhook_enabled\":%s}",
        cfg->wifi_ssid,cfg->mqtt_broker_uri,cfg->mqtt_topic_prefix,
        cfg->webhook_url,cfg->webhook_enabled?"true":"false");
    json_resp(req,200,buf);
    free(buf);
    return ESP_OK;
}

static esp_err_t handler_config_post(httpd_req_t *req)
{
    if(!check_auth(req)){json_resp(req,401,"{\"error\":\"Unauthorized\"}");return ESP_OK;}
    char body[BODY_BUF_SIZE];
    if(read_body(req,body,sizeof(body))<0){json_resp(req,400,"{\"error\":\"Bad body\"}");return ESP_OK;}
    gw_config_t cfg;
    memcpy(&cfg,gw_config_get(),sizeof(gw_config_t));
    json_get_str(body,"wifi_ssid",      cfg.wifi_ssid,        sizeof(cfg.wifi_ssid));
    json_get_str(body,"wifi_password",  cfg.wifi_password,    sizeof(cfg.wifi_password));
    json_get_str(body,"mqtt_broker_uri",cfg.mqtt_broker_uri,  sizeof(cfg.mqtt_broker_uri));
    json_get_str(body,"mqtt_prefix",    cfg.mqtt_topic_prefix,sizeof(cfg.mqtt_topic_prefix));
    json_get_str(body,"webhook_url",    cfg.webhook_url,      sizeof(cfg.webhook_url));
    json_get_str(body,"api_key",        cfg.api_key,          sizeof(cfg.api_key));
    char wh[8];
    if(json_get_str(body,"webhook_enabled",wh,sizeof(wh)))
        cfg.webhook_enabled=(strcmp(wh,"true")==0);
    gw_config_save(&cfg);
    json_resp(req,200,"{\"success\":true}");
    return ESP_OK;
}

static esp_err_t handler_options(httpd_req_t *req)
{
    httpd_resp_set_hdr(req,"Access-Control-Allow-Origin","*");
    httpd_resp_set_hdr(req,"Access-Control-Allow-Methods","GET,POST,DELETE,OPTIONS");
    httpd_resp_set_hdr(req,"Access-Control-Allow-Headers","Content-Type,Authorization");
    httpd_resp_send(req,NULL,0);
    return ESP_OK;
}

static esp_err_t handler_ota_update(httpd_req_t *req)
{
    if(!check_auth(req)){json_resp(req,401,"{\"error\":\"Unauthorized\"}");return ESP_OK;}
    char body[BODY_BUF_SIZE];
    if(read_body(req,body,sizeof(body))<0){json_resp(req,400,"{\"error\":\"Bad body\"}");return ESP_OK;}
    char url[256]={0};
    if(!json_get_str(body,"url",url,sizeof(url))){
        json_resp(req,400,"{\"error\":\"Missing url\"}");return ESP_OK;
    }
    esp_err_t err=ota_update_start(url);
    if(err==ESP_OK)
        json_resp(req,200,"{\"success\":true,\"message\":\"OTA started\"}");
    else
        json_resp(req,200,"{\"success\":false,\"error\":\"OTA already in progress\"}");
    return ESP_OK;
}

static esp_err_t handler_ota_status(httpd_req_t *req)
{
    char buf[128];
    snprintf(buf,sizeof(buf),"{\"status\":\"%s\"}",ota_update_status());
    json_resp(req,200,buf);
    return ESP_OK;
}

void rest_api_register(httpd_handle_t server)
{
    httpd_uri_t r0 = {.uri="/api/status",     .method=HTTP_GET,    .handler=handler_status,     .user_ctx=NULL,.is_websocket=false};
    httpd_uri_t r1 = {.uri="/api/sms/inbox",  .method=HTTP_GET,    .handler=handler_inbox,      .user_ctx=NULL,.is_websocket=false};
    httpd_uri_t r2 = {.uri="/api/sms/outbox", .method=HTTP_GET,    .handler=handler_outbox,     .user_ctx=NULL,.is_websocket=false};
    httpd_uri_t r3 = {.uri="/api/sms/send",   .method=HTTP_POST,   .handler=handler_send,       .user_ctx=NULL,.is_websocket=false};
    httpd_uri_t r4 = {.uri="/api/sms/*",      .method=HTTP_DELETE, .handler=handler_delete,     .user_ctx=NULL,.is_websocket=false};
    httpd_uri_t r5 = {.uri="/api/config",     .method=HTTP_GET,    .handler=handler_config_get, .user_ctx=NULL,.is_websocket=false};
    httpd_uri_t r6 = {.uri="/api/config",     .method=HTTP_POST,   .handler=handler_config_post,.user_ctx=NULL,.is_websocket=false};
    httpd_uri_t r7 = {.uri="/api/*",          .method=HTTP_OPTIONS,.handler=handler_options,    .user_ctx=NULL,.is_websocket=false};
    httpd_uri_t r8 = {.uri="/api/ota/update", .method=HTTP_POST,   .handler=handler_ota_update, .user_ctx=NULL,.is_websocket=false};
    httpd_uri_t r9 = {.uri="/api/ota/status", .method=HTTP_GET,    .handler=handler_ota_status, .user_ctx=NULL,.is_websocket=false};

    httpd_register_uri_handler(server,&r0);
    httpd_register_uri_handler(server,&r1);
    httpd_register_uri_handler(server,&r2);
    httpd_register_uri_handler(server,&r3);
    httpd_register_uri_handler(server,&r4);
    httpd_register_uri_handler(server,&r5);
    httpd_register_uri_handler(server,&r6);
    httpd_register_uri_handler(server,&r7);
    httpd_register_uri_handler(server,&r8);
    httpd_register_uri_handler(server,&r9);

    ESP_LOGI(TAG,"REST API registered");
}
