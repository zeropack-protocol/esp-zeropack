#include "zeropack/tcp.h"

#include "freertos/idf_additions.h"
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
                vTaskDelay(pdMS_TO_TICKS(5));
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

            zp_rx_commit(ctx, (size_t)len);
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
#if __has_include(<lwip/sockets.h>)
    return lwip_writev(tcp->sock, (const struct iovec*)iov, iovcnt);
#else
    return writev(tcp->sock, (const struct iovec*)iov, iovcnt);
#endif
}

void zp_tcp_lock(zp_ctx_t* ctx, zp_lock_target_t target) {
    zp_tcp_t* tcp = ctx->transport;

    if (tcp && tcp->locks[target]) {
        xSemaphoreTake(tcp->locks[target], portMAX_DELAY);
    }
}

void zp_tcp_unlock(zp_ctx_t* ctx, zp_lock_target_t target) {
    zp_tcp_t* tcp = ctx->transport;

    if (tcp && tcp->locks[target]) {
        xSemaphoreGive(tcp->locks[target]);
    }
}

int zp_tcp_start(zp_ctx_t* ctx, zp_tcp_t* transport) {
    if (!ctx || !transport || ctx->transport) return -1;

    transport->sock = -1;
    transport->locks[ZP_LOCK_TX] =
        xSemaphoreCreateMutexStatic(&transport->lock_bufs[ZP_LOCK_TX]);
    transport->locks[ZP_LOCK_POOL] =
        xSemaphoreCreateMutexStatic(&transport->lock_bufs[ZP_LOCK_POOL]);

    ctx->lock = ctx->lock ? ctx->lock : zp_tcp_lock;
    ctx->unlock = ctx->unlock ? ctx->unlock : zp_tcp_unlock;
    ctx->writev = ctx->writev ? ctx->writev : zp_tcp_writev;

    if (!zp_init(ctx)) return -1;

    ctx->transport = transport;

    transport->task = xTaskCreateStatic(
        zp_tcp_task,
        "zp_tcp_task",
        CONFIG_ZP_STACK_SIZE,
        ctx,
        5,
        transport->stack,
        &transport->tcb
    );

    if (!transport->task) {
        ctx->transport = NULL;
        return -1;
    };

    return 0;
}
