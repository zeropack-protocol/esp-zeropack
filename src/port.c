#include "port.h"

#include <arpa/inet.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "portmacro.h"
#include "sdkconfig.h"
#include "zeropack/core.h"
#include "zeropack/types.h"

_Static_assert(
    sizeof(CONFIG_ZP_SERVER_IP) > 1,
    "ZeroPack: Server IP is required. Set it up in menuconfig -> Component "
    "config -> ZeroPack."
);

_Static_assert(
    CONFIG_ZP_SERVER_PORT > 0,
    "ZeroPack: Server port is required. Set it up in menuconfig -> "
    "Component config -> ZeroPack."
);

static const char* TAG = "ZP";

static int s_sock = -1;
static StackType_t s_rx_stack[CONFIG_ZP_STACK_SIZE];
static StaticTask_t s_rx_tcb;

static StaticSemaphore_t s_lock_bufs[2];
static SemaphoreHandle_t s_locks[2];

__attribute__((constructor)) static void esp_locks_init(void) {
    s_locks[ZP_LOCK_TX] = xSemaphoreCreateMutexStatic(&s_lock_bufs[ZP_LOCK_TX]);
    s_locks[ZP_LOCK_POOL] =
        xSemaphoreCreateMutexStatic(&s_lock_bufs[ZP_LOCK_POOL]);
}

void esp_lock(zp_lock_target_t target, void* _) {
    xSemaphoreTake(s_locks[target], portMAX_DELAY);
}

void esp_unlock(zp_lock_target_t target, void* _) {
    xSemaphoreGive(s_locks[target]);
}

zp_ssize_t esp_writev(const zp_iovec_t* iov, int iovcnt) {
    if (s_sock < 0) return -1;
    return lwip_writev(s_sock, (const struct iovec*)iov, iovcnt);
}

static void zp_rx_task(void* pvParameters) {
    zp_ctx_t* ctx = pvParameters;

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(CONFIG_ZP_SERVER_PORT),
        .sin_addr.s_addr = inet_addr(CONFIG_ZP_SERVER_IP),
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
            vTaskDelay(pdMS_TO_TICKS(CONFIG_ZP_RECONNECT_DELAY_MS));
            continue;
        }

        waiting_logged = false;
        s_sock = sock;

        zp_send_handshake(ctx);

        ESP_LOGI(
            TAG,
            "Listening to packets from server (%s:%u)",
            CONFIG_ZP_SERVER_IP,
            CONFIG_ZP_SERVER_PORT
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

        close(s_sock);
        vTaskDelay(pdMS_TO_TICKS(CONFIG_ZP_RECONNECT_DELAY_MS));
    }
}

void esp_zp_start_task(zp_ctx_t* ctx) {
    xTaskCreateStatic(
        zp_rx_task, "zp_rx", CONFIG_ZP_STACK_SIZE, ctx, 5, s_rx_stack, &s_rx_tcb
    );
}
