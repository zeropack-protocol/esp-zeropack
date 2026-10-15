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

- **FreeRTOS Integration:** Static mutexes (`xSemaphoreCreateMutexStatic`) for ZeroPack TX and pool synchronization.
- **TCP Socket Worker:** Background FreeRTOS task handling TCP connection, reconnection, and packet dispatching.
- **Submodule Core:** Directly pulls the canonical, freestanding [ZeroPack C11 core](https://github.com/zeropack-protocol/zeropack).
- **Kconfig Configurable:** Configure server IP, port, reconnect timeouts, and buffer sizes directly via `idf.py menuconfig`.

---

## Installation

Add this repository as a submodule into your ESP-IDF project's `components/` directory:

```bash
git submodule add --recursive https://github.com/zeropack-protocol/esp-zeropack.git components/esp-zeropack
```

---

## Configuration

Run `idf.py menuconfig` and navigate to:

```text
Component config --->
    ZeroPack Configuration --->
        (192.168.1.100) Server IP address
        (8080)          Server TCP port
        (1024)          Receive buffer size (bytes)
        (4096)          RX task stack size (bytes)
        (1024)          Static memory pool size (bytes)
        (1500)          Reconnection retry delay (ms)
```

---

## Usage

In your application code (e.g., `main/app_main.c`):

```c
#include "zeropack.h"
#include "esp_log.h"

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
ZP_GENERATE_SCHEMA(APP_TYPES, APP_ACTIONS)

// 2. Action callback
static void on_ping(void) {
    ESP_LOGI("APP", "Received ACTION_PING from server");
}

void app_main(void) {
    // Register action callbacks
    zp_on_action(ACTION_PING, on_ping);

    // Starts the background TCP RX worker and connects to backend
    zp_start();

    // Emit events over WiFi
    Telemetry report = {
        .temperature = 25.4f,
        .uptime_ms = 1000,
    };
    zp_emit(ACTION_TELEMETRY, &report);
}
```

---

## License

This project is licensed under the [MIT License](LICENSE).
