#pragma once

#include "zeropack/core.h"
#include "zeropack/types.h"

void esp_lock(zp_lock_target_t target, void* userdata);
void esp_unlock(zp_lock_target_t target, void* userdata);
zp_ssize_t esp_writev(const zp_iovec_t* iov, int iovcnt);
void esp_zp_start_task(zp_ctx_t* ctx);

#define ESP_ZP_CONFIG \
    .writev = esp_writev, .lock = esp_lock, .unlock = esp_unlock
