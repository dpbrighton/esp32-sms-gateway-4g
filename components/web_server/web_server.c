#include <string.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "sms_store.h"
#include "a7670e.h"
#include "config.h"
#include "web_server.h"

static const char *TAG = "WEB_SERVER";

#define MAX_WS_CLIENTS 4
static int s_ws_fds[MAX_WS_CLIENTS];
static httpd_handle_t s_server_handle = NULL;

static const char *INDEX_HTML =
"<!DOCTYPE html><html lang='en'><head><meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
"<title>SMS Gateway</title>"
"<style>"
":root{--bg:#0f1117;--surface:#1a1d2e;--border:#2a2d3e;--accent:#00d4ff;--accent2:#7c3aed;"
"--text:#e2e8f0;--muted:#64748b;--success:#10b981;--error:#ef4444}"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{background:var(--bg);color:var(--text);font-family:monospace;min-height:100vh}"
"header{background:var(--surface);border-bottom:1px solid var(--border);padding:1rem 2rem;"
"display:flex;align-items:center;gap:1rem}"
"header .logo{font-size:1.4rem;font-weight:700;color:var(--accent);letter-spacing:2px}"
".dot{width:8px;height:8px;border-radius:50%;background:var(--success);animation:pulse 2s infinite}"
"@keyframes pulse{0%,100%{opacity:1}50%{opacity:.4}}"
".minfo{margin-left:auto;display:flex;gap:1.5rem;font-size:.75rem;color:var(--muted)}"
".minfo span b{color:var(--accent)}"
"main{display:grid;grid-template-columns:1fr 1fr;gap:1.5rem;padding:1.5rem 2rem}"
"@media(max-width:900px){main{grid-template-columns:1fr}}"
"section{background:var(--surface);border:1px solid var(--border);border-radius:12px;padding:1.5rem}"
"h2{font-size:.85rem;font-weight:700;letter-spacing:2px;text-transform:uppercase;"
"color:var(--accent);margin-bottom:1.25rem}"
".form{display:flex;flex-direction:column;gap:.75rem}"
"input,textarea{background:var(--bg);border:1px solid var(--border);color:var(--text);"
"border-radius:8px;padding:.75rem 1rem;font-family:inherit;font-size:.875rem;resize:vertical}"
"input:focus,textarea:focus{outline:none;border-color:var(--accent)}"
"textarea{min-height:120px}"
"button{background:linear-gradient(135deg,var(--accent),var(--accent2));color:#fff;"
"border:none;border-radius:8px;padding:.75rem 1.5rem;font-family:inherit;font-weight:700;"
"font-size:.875rem;cursor:pointer;transition:.2s}"
"button:hover{opacity:.85}"
"button.sec{background:var(--bg);border:1px solid var(--border);color:var(--muted)}"
".cc{font-size:.7rem;color:var(--muted);text-align:right}"
".msgs{display:flex;flex-direction:column;gap:.75rem;max-height:500px;overflow-y:auto}"
".msg{background:var(--bg);border:1px solid var(--border);border-radius:10px;padding:1rem}"
".msg.unread{border-left:3px solid var(--accent)}"
".msg.out{border-left:3px solid var(--accent2)}"
".mh{display:flex;justify-content:space-between;margin-bottom:.5rem}"
".mfrom{font-weight:700;font-size:.875rem;color:var(--accent)}"
".mtime{font-size:.7rem;color:var(--muted)}"
".mbody{font-size:.875rem;line-height:1.6}"
".ma{margin-top:.5rem;display:flex;gap:.5rem}"
".ma button{padding:4px 10px;font-size:.7rem;border-radius:6px}"
".tabs{display:flex;gap:.5rem;margin-bottom:1rem}"
".tab{background:var(--bg);border:1px solid var(--border);border-radius:8px;"
"padding:.4rem 1rem;font-size:.75rem;cursor:pointer;color:var(--muted)}"
".tab.active{background:var(--accent);color:#000;border-color:var(--accent);font-weight:700}"
".toast{position:fixed;bottom:2rem;right:2rem;background:var(--surface);"
"border:1px solid var(--accent);border-radius:10px;padding:.75rem 1.25rem;"
"font-size:.8rem;z-index:999;display:none}"
".empty{color:var(--muted);font-size:.8rem;text-align:center;padding:2rem}"
"</style></head><body>"
"<header>"
"<div class='logo'>&#9656; SMS GATEWAY</div>"
"<div class='dot' id='dot'></div>"
"<div style='font-size:.75rem;color:var(--muted)' id='stxt'>Connecting...</div>"
"<div class='minfo'>"
"<span>SIGNAL <b id='sig'>--</b></span>"
"<span>NETWORK <b id='net'>--</b></span>"
"<span>IMEI <b id='imei'>--</b></span>"
"<span>BUILD <b id='bld'>--</b></span>"
"</div></header>"
"<main>"
"<section><h2>&#9998; COMPOSE</h2>"
"<div class='form'>"
"<input id='to' type='tel' placeholder='+447700900000'>"
"<textarea id='body' maxlength='320' placeholder='Message...'"
" oninput='document.getElementById(\"cc\").textContent=this.value.length+\"/160\"'></textarea>"
"<div class='cc'><span id='cc'>0/160</span></div>"
"<button onclick='sendSMS()'>SEND SMS</button>"
"</div></section>"
"<section><h2>&#9432; MODEM STATUS</h2>"
"<div id='mdet' style='font-size:.8rem;line-height:2;color:var(--muted)'>Loading...</div>"
"<br><button class='sec' onclick='refreshStatus()'>REFRESH</button>"
"<br><br>"
"<div style='margin-top:.5rem'>"
"<p style='font-size:.75rem;color:var(--muted);margin-bottom:.5rem'>FIRMWARE UPDATE</p>"
"<input id='otaurl' placeholder='http://192.168.0.x:8080/esp32_sms_gateway.bin'>"
"<br><br>"
"<button onclick='startOTA()'>FLASH UPDATE</button>"
"<div id='otastatus' style='font-size:.75rem;color:var(--muted);margin-top:.5rem'></div>"
"</div>"
"</section>"
"<section style='grid-column:1/-1'>"
"<h2>&#9993; MESSAGES <span id='mc' style='background:var(--accent2);color:#fff;"
"border-radius:99px;padding:2px 8px;font-size:.7rem'>0</span></h2>"
"<div class='tabs'>"
"<div class='tab active' onclick='switchTab(\"inbox\",this)'>INBOX</div>"
"<div class='tab' onclick='switchTab(\"outbox\",this)'>SENT</div>"
"</div>"
"<div class='msgs' id='ml'><div class='empty'>No messages yet.</div></div>"
"</section></main>"
"<div class='toast' id='toast'></div>"
"<script>"
"let ws,ct='inbox';"
"function init(){"
"refreshStatus();loadMsgs('inbox');"
"ws=new WebSocket('ws://'+location.host+'/ws');"
"ws.onopen=()=>{document.getElementById('dot').style.background='var(--success)';"
"document.getElementById('stxt').textContent='Connected';};"
"ws.onclose=()=>{document.getElementById('dot').style.background='var(--error)';"
"document.getElementById('stxt').textContent='Disconnected';setTimeout(init,3000);};"
"ws.onmessage=e=>{const d=JSON.parse(e.data);"
"if(d.type==='sms_received'){toast('SMS from '+d.sender);loadMsgs(ct);}};"
"}"
"function toast(m,d=3000){const t=document.getElementById('toast');"
"t.textContent=m;t.style.display='block';setTimeout(()=>t.style.display='none',d);}"
"async function sendSMS(){"
"const to=document.getElementById('to').value.trim();"
"const body=document.getElementById('body').value.trim();"
"if(!to||!body){toast('Fill in number and message');return;}"
"const r=await fetch('/api/sms/send',{method:'POST',"
"headers:{'Content-Type':'application/json','Authorization':'Bearer %%API_KEY%%'},"
"body:JSON.stringify({to,body})});"
"const j=await r.json();"
"if(j.success){toast('SMS sent!');document.getElementById('body').value='';"
"document.getElementById('cc').textContent='0/160';loadMsgs('outbox');}"
"else toast('Send failed: '+(j.error||'unknown'));}"
"async function refreshStatus(){"
"const r=await fetch('/api/status');const d=await r.json();"
"document.getElementById('net').textContent=d.operator||'--';"
"document.getElementById('imei').textContent=d.imei||'--';"
"document.getElementById('bld').textContent=d.build||'--';"
"document.getElementById('sig').textContent=d.rssi;"
"document.getElementById('mdet').innerHTML="
"'<div>Status: <b>'+(d.registered?'REGISTERED':'NO SERVICE')+'</b></div>'"
"+'<div>SIM: <b>'+(d.sim_ready?'Ready':'Not Ready')+'</b></div>'"
"+'<div>Operator: <b>'+d.operator+'</b></div>'"
"+'<div>IMEI: <b>'+d.imei+'</b></div>'"
"+'<div>Build: <b>'+d.build+'</b></div>'"
"+'<div>RSSI: <b>'+d.rssi+'</b></div>';}"
"async function loadMsgs(tab){"
"ct=tab;const r=await fetch('/api/sms/'+tab);const d=await r.json();"
"document.getElementById('mc').textContent=d.length;"
"const ml=document.getElementById('ml');"
"if(!d.length){ml.innerHTML='<div class=empty>No messages.</div>';return;}"
"ml.innerHTML=d.map(m=>"
"'<div class=\"msg '+(m.direction===0&&m.status===0?'unread':m.direction===1?'out':'')+'\">'"
"+'<div class=mh><span class=mfrom>'+esc(m.direction===0?m.sender:'To: '+m.sender)+'</span>'"
"+'<span class=mtime>'+esc(m.timestamp)+'</span></div>'"
"+'<div class=mbody>'+esc(m.body)+'</div>'"
"+'<div class=ma>'+(m.direction===0?'<button onclick=reply(\"'+esc(m.sender)+'\")>REPLY</button>':'')"
"+'<button class=sec onclick=del('+m.id+')>DELETE</button></div></div>').join('');}"
"function reply(n){document.getElementById('to').value=n;document.getElementById('body').focus();}"
"async function del(id){await fetch('/api/sms/'+id,{method:'DELETE'});loadMsgs(ct);}"
"function switchTab(t,el){document.querySelectorAll('.tab').forEach(x=>x.classList.remove('active'));"
"el.classList.add('active');loadMsgs(t);}"
"function esc(s){const d=document.createElement('div');d.textContent=s;return d.innerHTML;}"
"async function startOTA(){"
"const url=document.getElementById('otaurl').value.trim();"
"if(!url){toast('Enter firmware URL first');return;}"
"if(!confirm('Flash firmware from: '+url+'?'))return;"
"document.getElementById('otastatus').textContent='Starting OTA...';"
"const r=await fetch('/api/ota/update',{method:'POST',"
"headers:{'Content-Type':'application/json','Authorization':'Bearer %%API_KEY%%'},"
"body:JSON.stringify({url})});"
"const j=await r.json();"
"if(j.success){"
"document.getElementById('otastatus').textContent='Downloading - device will reboot when done...';"
"toast('OTA started!');"
"}else{"
"document.getElementById('otastatus').textContent='Failed: '+(j.error||'unknown');"
"}}"
"init();setInterval(refreshStatus,15000);"
"</script></body></html>";

static void ws_add(int fd){
    for(int i=0;i<MAX_WS_CLIENTS;i++) if(s_ws_fds[i]==0){s_ws_fds[i]=fd;return;}
}
static void ws_remove(int fd){
    for(int i=0;i<MAX_WS_CLIENTS;i++) if(s_ws_fds[i]==fd){s_ws_fds[i]=0;return;}
}

void web_server_ws_broadcast_sms(const sms_message_t *msg)
{
    if(!s_server_handle) return;
    char buf[700];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"sms_received\",\"sender\":\"%s\",\"body\":\"%s\",\"timestamp\":\"%s\"}",
             msg->sender, msg->body, msg->timestamp);

    for(int i=0;i<MAX_WS_CLIENTS;i++){
        if(s_ws_fds[i]==0) continue;
        httpd_ws_frame_t frame;
        memset(&frame, 0, sizeof(frame));
        frame.type    = HTTPD_WS_TYPE_TEXT;
        frame.payload = (uint8_t*)buf;
        frame.len     = strlen(buf);
        if(httpd_ws_send_frame_async(s_server_handle, s_ws_fds[i], &frame) != ESP_OK)
            ws_remove(s_ws_fds[i]);
    }
}

static esp_err_t handler_index(httpd_req_t *req)
{
    gw_config_t *cfg = gw_config_get();

    /* Replace %%API_KEY%% placeholder with real key from config */
    const char *placeholder = "%%API_KEY%%";
    const char *key = cfg->api_key;
    size_t html_len = strlen(INDEX_HTML);
    size_t key_len = strlen(key);
    size_t ph_len = strlen(placeholder);

    /* Count occurrences */
    int count = 0;
    const char *p = INDEX_HTML;
    while ((p = strstr(p, placeholder)) != NULL) { count++; p += ph_len; }

    /* Allocate buffer */
    size_t out_len = html_len + count * (key_len - ph_len) + 1;
    char *out = malloc(out_len);
    if (!out) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    /* Replace all occurrences */
    char *dst = out;
    const char *src = INDEX_HTML;
    while ((p = strstr(src, placeholder)) != NULL) {
        size_t before = p - src;
        memcpy(dst, src, before);
        dst += before;
        memcpy(dst, key, key_len);
        dst += key_len;
        src = p + ph_len;
    }
    strcpy(dst, src);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, out, strlen(out));
    free(out);
    return ESP_OK;
}

static esp_err_t handler_ws(httpd_req_t *req)
{
    if(req->method == HTTP_GET){
        ws_add(httpd_req_to_sockfd(req));
        ESP_LOGI(TAG, "WS client connected");
        return ESP_OK;
    }
    httpd_ws_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.type = HTTPD_WS_TYPE_TEXT;
    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if(ret != ESP_OK) return ret;
    if(frame.len){
        uint8_t *buf = calloc(1, frame.len+1);
        if(buf){
            frame.payload = buf;
            httpd_ws_recv_frame(req, &frame, frame.len);
            free(buf);
        }
    }
    if(frame.type == HTTPD_WS_TYPE_CLOSE)
        ws_remove(httpd_req_to_sockfd(req));
    return ESP_OK;
}

httpd_handle_t web_server_start(void)
{
    memset(s_ws_fds, 0, sizeof(s_ws_fds));
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_open_sockets = 4;
    cfg.max_uri_handlers = 14;
    cfg.lru_purge_enable = true;

    if(httpd_start(&s_server_handle, &cfg) != ESP_OK){
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return NULL;
    }

    httpd_uri_t idx = {.uri="/",  .method=HTTP_GET, .handler=handler_index, .user_ctx=NULL, .is_websocket=false};
    httpd_uri_t wsu = {.uri="/ws",.method=HTTP_GET, .handler=handler_ws,    .user_ctx=NULL, .is_websocket=true};

    httpd_register_uri_handler(s_server_handle, &idx);
    httpd_register_uri_handler(s_server_handle, &wsu);

    ESP_LOGI(TAG, "HTTP server started");
    return s_server_handle;
}

void web_server_stop(httpd_handle_t server)
{
    httpd_stop(server);
    s_server_handle = NULL;
}
