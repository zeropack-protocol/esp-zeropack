# esp-zeropack

ESP-IDF component providing FreeRTOS platform support and transports for the [ZeroPack](https://github.com/zeropack-protocol/zeropack) binary protocol.

---

## Supported Transports

- [x] **TCP Sockets (Wi-Fi / Ethernet)** &mdash; Background FreeRTOS worker via lwIP BSD sockets.
- [ ] **Bluetooth Low Energy (BLE)** &mdash; GATT service via NimBLE.
- [ ] **Hardware UART** &mdash; Direct interrupt/ring-buffer serial transport.
- [ ] **ESP-NOW** &mdash; Connectionless peer-to-peer transmission between ESP32 nodes.
- [ ] **TLS / Secure Sockets** &mdash; MbedTLS encrypted transport.
- [ ] **WebSockets** &mdash; Binary framing over WebSocket connection.
- [ ] **Cellular 4G/LTE** &mdash; PPP / modem transport via `esp_modem`.

---

## Features

- **Zero Global State & Multi-Instance:** Every `zp_tcp_t` embeds its own dedicated `StaticTask_t`, task stack, and `StaticSemaphore_t` write mutex. Run multiple connections with zero memory contention.
- **Zero Dynamic Heap Allocation:** All task and synchronization memory is statically allocated in BSS.
- **In-Place Initialization:** Initializes directly via `zp_init(&s_zp.ctx)` using C designated initializers.
- **Submodule Core:** Directly pulls the canonical, freestanding [ZeroPack C11 core](https://github.com/zeropack-protocol/zeropack).
- **Kconfig Tunable:** Task stack size configurable via `idf.py menuconfig`.

---

## Installation

### Option 1: ESP-IDF Component Manager (Recommended)

Add `esp-zeropack` to your project's `main/idf_component.yml`:

```yaml
dependencies:
  esp-zeropack:
    git: https://github.com/zeropack-protocol/esp-zeropack.git
```

### Option 2: Git Submodule

Clone directly into your project's `components/` directory:

```bash
git submodule add --recursive https://github.com/zeropack-protocol/esp-zeropack.git components/esp-zeropack
```

---

## Configuration

Run `idf.py menuconfig` and navigate to:

```text
Component config --->
    ZeroPack Configuration --->
        (4096) RX task stack size (bytes)
```

Host address, port, and reconnection timing can be configured per-instance directly on `zp_tcp_t`.

---

## Usage

In your application code (e.g., `main/app_main.c`):

```c
#include "esp_log.h"
#include "zeropack/tcp.h"

// 1. Declare application schema (structs & actions)
#define APP_TYPES(X) \
    X(Telemetry, TELEMETRY_FIELDS)

#define TELEMETRY_FIELDS(S, F, A) \
    F(S, f32, temperature)        \
    F(S, u32, uptime_ms)

#define APP_ACTIONS(V, T, A) \
    V(ACTION_PING)           \
    T(ACTION_TELEMETRY, Telemetry)

ZP_DECLARE(APP_TYPES, APP_ACTIONS)
ZP_GENERATE_SCHEMA(app_schema, APP_TYPES, APP_ACTIONS)

static zp_tcp_t s_zp;
static uint8_t s_rx[1024] __attribute__((aligned(8)));
static uint8_t s_pool[1024] __attribute__((aligned(8)));

void app_main(void) {
    zp_ctx_t ctx = {
        .schema = &app_schema,
        ZP_BUFFER(rx, s_rx),
        ZP_BUFFER(pool, s_pool),
    };

    zp_tcp_t tcp = {
        .host = "192.168.1.100",
        .port = 8080,
        .reconnect_ms = 1500,
    };

    // Initialize ZeroPack context and start background worker task
    zp_tcp_start(&ctx, &tcp);

    // Emit events over WiFi / Ethernet
    Telemetry report = {
        .temperature = 25.4f,
        .uptime_ms = 1000,
    };
    zp_emit(&ctx, ACTION_TELEMETRY, &report);

    // Process incoming events with non-blocking zp_poll & FreeRTOS task notifications
    zp_msg_t msg;
    while (1) {
        while (zp_poll(&ctx, &msg)) {
            switch (msg.action) {
            case ACTION_PING:
                ESP_LOGI("APP", "Received ACTION_PING from server");
                break;
            default:
                break;
            }
        }

        // Wake tcp_task if it was waiting for buffer space
        if (tcp.tcp_task) {
            xTaskNotifyGive(tcp.tcp_task);
        }

        // Sleep with 0% CPU until tcp_task notifies us of new incoming bytes
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
}
```

---

## License

This project is licensed under the [MIT License](LICENSE).
