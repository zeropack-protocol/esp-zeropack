#include "zeropack/transport.h"

#include <arpa/inet.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include "freertos/projdefs.h"
#include "zeropack/core.h"

#if __has_include(<lwip/sockets.h>)
#include "lwip/sockets.h"
#endif

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

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

static const char* TAG = "ZP transport";

static StackType_t s_rx_stack[CONFIG_ZP_STACK_SIZE];
static StaticTask_t s_rx_tcb;

static int s_sock = -1;

static zp_ssize_t esp_transport_writev(const zp_iovec_t* iov, int iovcnt) {
    if (s_sock < 0) return -1;
    return writev(s_sock, (const struct iovec*)iov, iovcnt);
}

static zp_ssize_t esp_transport_recv(void* buf, size_t max_len) {
    if (s_sock < 0) return -1;
    return recv(s_sock, buf, max_len, 0);
}

static void esp_transport_close(void) {
    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }
}

static void zp_rx_task(void* pvParameters) {
    (void)pvParameters;

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

        zp_send_handshake();

        ESP_LOGI(
            TAG,
            "Listening to packets from server (%s:%u)",
            CONFIG_ZP_SERVER_IP,
            CONFIG_ZP_SERVER_PORT
        );

        while (1) {
            size_t avail = 0;
            uint8_t* buf = zp_rx_get_buffer(&avail);

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

            zp_rx_commit((size_t)len);
        }

        esp_transport_close();
        vTaskDelay(pdMS_TO_TICKS(CONFIG_ZP_RECONNECT_DELAY_MS));
    }
}

static int esp_transport_start(void) {
    xTaskCreateStatic(
        zp_rx_task,
        "zp_rx",
        CONFIG_ZP_STACK_SIZE,
        NULL,
        5,
        s_rx_stack,
        &s_rx_tcb
    );
    return 0;
}

static const zp_transport_t s_esp_transport = {
    .start = esp_transport_start,
    .writev = esp_transport_writev,
    .recv = esp_transport_recv,
    .close = esp_transport_close,
};

const zp_transport_t* zp_transport = &s_esp_transport;
