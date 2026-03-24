#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "tcp_log.h"

#define TCP_LOG_PORT      8888
#define TCP_LOG_BUF_SIZE  256
#define RING_BUF_LINES    200
#define TAG               "TCP_LOG"

static char     s_ring[RING_BUF_LINES][TCP_LOG_BUF_SIZE];
static int      s_ring_head  = 0;
static int      s_ring_count = 0;
static SemaphoreHandle_t s_ring_mutex = NULL;

static void ring_push(const char *line, int len)
{
    if (!s_ring_mutex) return;
    if (xSemaphoreTake(s_ring_mutex, pdMS_TO_TICKS(10)) != pdTRUE) return;
    int copy_len = len < TCP_LOG_BUF_SIZE - 1 ? len : TCP_LOG_BUF_SIZE - 1;
    memcpy(s_ring[s_ring_head], line, copy_len);
    s_ring[s_ring_head][copy_len] = '\0';
    s_ring_head = (s_ring_head + 1) % RING_BUF_LINES;
    if (s_ring_count < RING_BUF_LINES) s_ring_count++;
    xSemaphoreGive(s_ring_mutex);
}

static void ring_replay(int client_sock)
{
    if (!s_ring_mutex) return;
    if (xSemaphoreTake(s_ring_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    const char *hdr = "--- Boot log history ---\r\n";
    send(client_sock, hdr, strlen(hdr), 0);
    int start = s_ring_count < RING_BUF_LINES ? 0 : s_ring_head;
    for (int i = 0; i < s_ring_count; i++) {
        int idx = (start + i) % RING_BUF_LINES;
        int len = strlen(s_ring[idx]);
        if (len > 0) send(client_sock, s_ring[idx], len, 0);
    }
    const char *sep = "--- Live log ---\r\n";
    send(client_sock, sep, strlen(sep), 0);
    xSemaphoreGive(s_ring_mutex);
}

static int s_client_sock = -1;
static vprintf_like_t s_orig_vprintf = NULL;

static int tcp_log_vprintf(const char *fmt, va_list args)
{
    int ret = s_orig_vprintf(fmt, args);
    char buf[TCP_LOG_BUF_SIZE];
    va_list args_copy;
    va_copy(args_copy, args);
    int len = vsnprintf(buf, sizeof(buf), fmt, args_copy);
    va_end(args_copy);
    if (len > 0) {
        ring_push(buf, len);
        if (s_client_sock >= 0) {
            if (send(s_client_sock, buf, len, MSG_DONTWAIT) < 0) {
                close(s_client_sock);
                s_client_sock = -1;
            }
        }
    }
    return ret;
}

static void tcp_log_task(void *arg)
{
    /* Wait for network stack to be ready before creating socket */
    vTaskDelay(pdMS_TO_TICKS(2000));

    int server_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_sock < 0) { ESP_LOGE(TAG, "Failed to create socket"); vTaskDelete(NULL); return; }
    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {
        .sin_family = AF_INET, .sin_port = htons(TCP_LOG_PORT), .sin_addr.s_addr = INADDR_ANY,
    };
    if (bind(server_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "Bind failed"); close(server_sock); vTaskDelete(NULL); return;
    }
    if (listen(server_sock, 1) < 0) {
        ESP_LOGE(TAG, "Listen failed"); close(server_sock); vTaskDelete(NULL); return;
    }
    ESP_LOGI(TAG, "WiFi log server ready — connect with: nc <gateway-ip> 8888");
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client = accept(server_sock, (struct sockaddr *)&client_addr, &client_len);
        if (client < 0) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
        char client_ip[16];
        inet_ntoa_r(client_addr.sin_addr, client_ip, sizeof(client_ip));
        ESP_LOGI(TAG, "WiFi monitor connected from %s", client_ip);
        const char *banner =
            "\r\n=== ESP32 SMS Gateway - WiFi Log Monitor ===\r\n"
            "=== Press Ctrl+C to disconnect             ===\r\n\r\n";
        send(client, banner, strlen(banner), 0);
        ring_replay(client);
        s_client_sock = client;
        char discard[16];
        while (recv(client, discard, sizeof(discard), 0) > 0) {}
        s_client_sock = -1;
        close(client);
        ESP_LOGI(TAG, "WiFi monitor disconnected from %s", client_ip);
    }
}

void tcp_log_init(void)
{
    s_ring_mutex   = xSemaphoreCreateMutex();
    s_orig_vprintf = esp_log_set_vprintf(tcp_log_vprintf);
    xTaskCreate(tcp_log_task, "tcp_log", 4096, NULL, 3, NULL);
    ESP_LOGI(TAG, "TCP log initialised on port %d (history: %d lines)",
             TCP_LOG_PORT, RING_BUF_LINES);
}
