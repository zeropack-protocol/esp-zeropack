#include "zeropack/mutex.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

struct zp_mutex {
    SemaphoreHandle_t handle;
    StaticSemaphore_t buffer;
};

#define ZP_MUTEX_DEFINE(name)             \
    static struct zp_mutex __impl_##name; \
    zp_mutex_t* zp_mutex_##name = (zp_mutex_t*)&__impl_##name

ZP_MUTEX_DEFINE(tx);
ZP_MUTEX_DEFINE(pool);

void zp_mutex_lock(zp_mutex_t* m) {
    if (m) {
        if (!m->handle) {
            m->handle = xSemaphoreCreateMutexStatic(&m->buffer);
        }
        xSemaphoreTake(m->handle, portMAX_DELAY);
    }
}

void zp_mutex_unlock(zp_mutex_t* m) {
    if (m && m->handle) {
        xSemaphoreGive(m->handle);
    }
}
