#pragma once

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "portmacro.h"
#include "sdkconfig.h"
#include "zeropack.h"

typedef struct {
    zp_ctx_t ctx;

    const char* host;
    uint16_t port;
    uint32_t reconnect_ms;

    int sock;
    StaticSemaphore_t lock_bufs[2];
    SemaphoreHandle_t locks[2];

    StaticTask_t tcb;
    StackType_t stack[CONFIG_ZP_STACK_SIZE];
} zp_tcp_t;

zp_ssize_t zp_tcp_writev(zp_ctx_t* ctx, const zp_iovec_t* iov, int iovcnt);
void zp_tcp_start(zp_tcp_t* transport);

void zp_tcp_lock(zp_ctx_t* ctx, zp_lock_target_t target);
void zp_tcp_unlock(zp_ctx_t* ctx, zp_lock_target_t target);

#define ZP_TCP_CONFIG \
    .writev = zp_tcp_writev, .lock = zp_tcp_lock, .unlock = zp_tcp_unlock
