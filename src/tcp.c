#include "zeropack/tcp.h"

#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "portmacro.h"

#if __has_include(<lwip/sockets.h>)
#include "lwip/sockets.h"
#else  // for testing in host (linux)
#include <arpa/inet.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

#include "esp_log.h"
#include "zeropack.h"

static const char* TAG = "ZP-TCP";

static void zp_tcp_task(void* pvParameters) {
    zp_ctx_t* ctx = pvParameters;
    zp_tcp_t* tcp = ctx->transport;

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(tcp->port),
        .sin_addr.s_addr = inet_addr(tcp->host),
    };

    bool waiting_logged = false;
    while (1) {
        int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (sock < 0) {
            ESP_LOGE(TAG, "Could not open socket: %d", sock);
            vTaskDelay(pdMS_TO_TICKS(1500));
            continue;
        }

        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
            if (!waiting_logged) {
                ESP_LOGW(TAG, "Waiting for server..");
                waiting_logged = true;
            }
            close(sock);
            vTaskDelay(pdMS_TO_TICKS(tcp->reconnect_ms));
            continue;
        }

        waiting_logged = false;
        tcp->sock = sock;

        zp_send_handshake(ctx);

        ESP_LOGI(
            TAG,
            "Listening to packets from server (%s:%u)",
            tcp->host,
            tcp->port
        );

        while (1) {
            size_t avail = zp_rx_avail(ctx);
            uint8_t* buf = zp_rx_buf(ctx);

            if (!buf || avail == 0) {
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
                continue;
            }

            ssize_t len = recv(sock, buf, avail, 0);

            if (len < 0) {
                ESP_LOGE(
                    TAG, "recv() failed or socket closed, restarting connection"
                );
                break;
            } else if (len == 0) {
                ESP_LOGW(TAG, "Server disconnected");
                break;
            }

            ctx->rx_head += len;

            if (tcp->app_task) {
                xTaskNotifyGive(tcp->app_task);
            }
        }

        int old_sock = tcp->sock;
        tcp->sock = -1;
        if (old_sock >= 0) {
            close(old_sock);
        }
        vTaskDelay(pdMS_TO_TICKS(tcp->reconnect_ms));
    }
}

zp_ssize_t zp_tcp_writev(zp_ctx_t* ctx, const zp_iovec_t* iov, int iovcnt) {
    zp_tcp_t* tcp = ctx->transport;
    if (!tcp || tcp->sock < 0) return -1;

    xSemaphoreTake(tcp->writev_lock, portMAX_DELAY);

    zp_ssize_t ret;
#if __has_include(<lwip/sockets.h>)
    ret = lwip_writev(tcp->sock, (const struct iovec*)iov, iovcnt);
#else
    ret = writev(tcp->sock, (const struct iovec*)iov, iovcnt);
#endif

    xSemaphoreGive(tcp->writev_lock);
    return ret;
}

int zp_tcp_start(zp_ctx_t* ctx, zp_tcp_t* transport) {
    if (!ctx || !transport || ctx->transport) return -1;

    transport->sock = -1;

    transport->writev_lock =
        xSemaphoreCreateMutexStatic(&transport->writev_lock_buf);

    ctx->writev = ctx->writev ? ctx->writev : zp_tcp_writev;

    if (!zp_init(ctx)) return -1;

    ctx->transport = transport;

    if (!transport->app_task) {
        transport->app_task = xTaskGetCurrentTaskHandle();
    }

    transport->tcp_task = xTaskCreateStatic(
        zp_tcp_task,
        "zp_tcp_task",
        CONFIG_ZP_STACK_SIZE,
        ctx,
        5,
        transport->stack,
        &transport->tcb
    );

    if (!transport->tcp_task) {
        ctx->transport = NULL;
        return -1;
    };

    return 0;
}
