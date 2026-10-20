#pragma once

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "portmacro.h"
#include "sdkconfig.h"
#include "zeropack.h"

typedef struct {
    const char* host;
    uint16_t port;
    uint32_t reconnect_ms;

    int sock;

    SemaphoreHandle_t writev_lock;
    StaticSemaphore_t writev_lock_buf;

    TaskHandle_t app_task;
    TaskHandle_t tcp_task;

    StaticTask_t tcb;
    StackType_t stack[CONFIG_ZP_STACK_SIZE];
} zp_tcp_t;

int zp_tcp_start(zp_ctx_t* ctx, zp_tcp_t* transport);
zp_ssize_t zp_tcp_writev(zp_ctx_t* ctx, const zp_iovec_t* iov, int iovcnt);
