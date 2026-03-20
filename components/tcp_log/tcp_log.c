#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "tcp_log.h"

#define TCP_LOG_PORT     8888
#define TCP_LOG_BUF_SIZE 512
#define TAG              "TCP_LOG"

static int s_client_sock = -1;
static vprintf_like_t s_orig_vprintf = NULL;

static int tcp_log_vprintf(const char *fmt, va_list args)
{
    int ret = s_orig_vprintf(fmt, args);

    if (s_client_sock >= 0) {
        char buf[TCP_LOG_BUF_SIZE];
        va_list args_copy;
        va_copy(args_copy, args);
        int len = vsnprintf(buf, sizeof(buf), fmt, args_copy);
        va_end(args_copy);

        if (len > 0) {
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
    int server_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_sock < 0) {
        ESP_LOGE(TAG, "Failed to create socket");
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(TCP_LOG_PORT),
        .sin_addr.s_addr = INADDR_ANY,
    };

    if (bind(server_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "Bind failed");
        close(server_sock);
        vTaskDelete(NULL);
        return;
    }

    if (listen(server_sock, 1) < 0) {
        ESP_LOGE(TAG, "Listen failed");
        close(server_sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "WiFi log server ready — connect with: nc <gateway-ip> 8888");

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client = accept(server_sock, (struct sockaddr *)&client_addr, &client_len);
        if (client < 0) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        char client_ip[16];
        inet_ntoa_r(client_addr.sin_addr, client_ip, sizeof(client_ip));
        ESP_LOGI(TAG, "WiFi monitor connected from %s", client_ip);

        const char *banner =
            "\r\n=== ESP32 SMS Gateway - WiFi Log Monitor ===\r\n"
            "=== Press Ctrl+C to disconnect             ===\r\n\r\n";
        send(client, banner, strlen(banner), 0);

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
    s_orig_vprintf = esp_log_set_vprintf(tcp_log_vprintf);
    xTaskCreate(tcp_log_task, "tcp_log", 4096, NULL, 3, NULL);
    ESP_LOGI(TAG, "TCP log initialised on port %d", TCP_LOG_PORT);
}
