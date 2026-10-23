#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "unity.h"
#include "unity_test_runner.h"

#define ZP_IMPLEMENTATION
#include "zeropack.h"
#include "zeropack/tcp.h"

#define SENSOR_FIELDS(S, F, A) \
    F(S, u16, sensor_id)       \
    F(S, s32, temp_milli)      \
    F(S, bool, is_active)

#define TEST_TYPES(X) X(sensor_t, SENSOR_FIELDS)

#define TEST_ACTIONS(V, T, A)       \
    V(ACTION_PING)                  \
    T(ACTION_UPDATE_DATA, sensor_t) \
    A(ACTION_SEND_BUFFER, u8)

ZP_TYPES(TEST_TYPES)
ZP_SCHEMA(test_schema, TEST_TYPES, TEST_ACTIONS)

static zp_ctx_t s_ctx;
static uint8_t s_rx[256] __attribute__((aligned(8)));
static uint8_t s_pool[256] __attribute__((aligned(8)));

static zp_ssize_t dummy_writev(
    zp_ctx_t* ctx, const zp_iovec_t* iov, int iovcnt
) {
    (void)ctx;
    (void)iov;
    (void)iovcnt;
    return 0;
}

void setUp(void) {
    s_ctx = (zp_ctx_t){
        .schema = &test_schema,
        .writev = dummy_writev,
        .buf = {.data = s_rx, .cap = sizeof(s_rx)},
        .pool = {.data = s_pool, .cap = sizeof(s_pool)},
    };
    zp_init(&s_ctx);
}

// Target ABI & Memory Smoke Tests =============================================

TEST_CASE("Schema: ABI, struct offsets, and field metadata", "[abi]") {
    TEST_ASSERT_EQUAL(sizeof(sensor_t), zp_metaof(sensor_t)->size);
    TEST_ASSERT_EQUAL_STRING("sensor_t", zp_metaof(sensor_t)->name);
    TEST_ASSERT_EQUAL_PTR(zp_metaof(sensor_t), test_schema.types[0]);
    TEST_ASSERT_EQUAL(3, zp_metaof(sensor_t)->count);

    TEST_ASSERT_EQUAL(
        offsetof(sensor_t, sensor_id), zp_metaof(sensor_t)->fields[0].offset
    );
    TEST_ASSERT_EQUAL(
        offsetof(sensor_t, temp_milli), zp_metaof(sensor_t)->fields[1].offset
    );
    TEST_ASSERT_EQUAL(
        offsetof(sensor_t, is_active), zp_metaof(sensor_t)->fields[2].offset
    );

    TEST_ASSERT_EQUAL(3, test_schema.action_count);
    TEST_ASSERT_EQUAL_STRING("ACTION_PING", test_schema.actions[0].name);
    TEST_ASSERT_EQUAL_STRING("ACTION_UPDATE_DATA", test_schema.actions[1].name);
    TEST_ASSERT_EQUAL_STRING("ACTION_SEND_BUFFER", test_schema.actions[2].name);
}

TEST_CASE("Pool: memory allocation and type-safe retrieval", "[pool]") {
    zp_pool_reset(&s_ctx);
    TEST_ASSERT_EQUAL_UINT32(0, s_ctx.pool.head);

    poolref_t ref_str = zp_pool_push_str(&s_ctx, "ZeroPack String");
    TEST_ASSERT_EQUAL_UINT16(0, ref_str);

    char* str = zp_pool_get_str(&s_ctx, ref_str);
    TEST_ASSERT_EQUAL_STRING("ZeroPack String", str);

    sensor_t s = {
        .sensor_id = 101,
        .temp_milli = 24500,
        .is_active = true,
    };
    poolref_t ref_struct = zp_pool_push(&s_ctx, sensor_t, &s, 1);
    sensor_t* rec_s = zp_pool_get(&s_ctx, sensor_t, ref_struct, NULL);
    TEST_ASSERT_NOT_NULL(rec_s);
    TEST_ASSERT_EQUAL_UINT16(101, rec_s->sensor_id);
    TEST_ASSERT_EQUAL_INT32(24500, rec_s->temp_milli);

    zp_pool_reset(&s_ctx);
    TEST_ASSERT_EQUAL_UINT32(0, s_ctx.pool.head);
}

// Transport & FreeRTOS Concurrency Tests ======================================

TEST_CASE("TCP: multi-instance isolation and offline handling", "[tcp]") {
    static zp_tcp_t s_tcp1;
    static zp_tcp_t s_tcp2;

    // Both instances have distinct stack buffers and TCBs in BSS
    TEST_ASSERT_NOT_EQUAL((void*)s_tcp1.stack, (void*)s_tcp2.stack);
    TEST_ASSERT_NOT_EQUAL((void*)&s_tcp1.tcb, (void*)&s_tcp2.tcb);

    // Offline writev returns -1 cleanly when socket is closed
    s_tcp1.sock = -1;
    zp_ctx_t ctx1 = {.transport = &s_tcp1};
    zp_iovec_t iov = {.buf = "abc", .len = 3};
    TEST_ASSERT_EQUAL(-1, zp_tcp_writev(&ctx1, &iov, 1));
}

#include <string.h>
#include <unistd.h>

#define WORKER_STACK_SIZE 4096
static StaticTask_t s_worker1_tcb;
static StackType_t s_worker1_stack[WORKER_STACK_SIZE];
static StaticTask_t s_worker2_tcb;
static StackType_t s_worker2_stack[WORKER_STACK_SIZE];

static SemaphoreHandle_t s_done_sem;
static StaticSemaphore_t s_done_sem_buf;
static SemaphoreHandle_t s_pool_lock;
static StaticSemaphore_t s_pool_lock_buf;

static void worker_pool_task1(void* pv) {
    zp_ctx_t* ctx = (zp_ctx_t*)pv;
    for (int i = 0; i < 50; i++) {
        xSemaphoreTake(s_pool_lock, portMAX_DELAY);
        zp_pool_push_str(ctx, "worker1_item");
        xSemaphoreGive(s_pool_lock);
    }
    xSemaphoreGive(s_done_sem);
    vTaskDelete(NULL);
}

static void worker_pool_task2(void* pv) {
    zp_ctx_t* ctx = (zp_ctx_t*)pv;
    for (int i = 0; i < 50; i++) {
        xSemaphoreTake(s_pool_lock, portMAX_DELAY);
        zp_pool_push_str(ctx, "worker2_item");
        xSemaphoreGive(s_pool_lock);
    }
    xSemaphoreGive(s_done_sem);
    vTaskDelete(NULL);
}

TEST_CASE(
    "FreeRTOS: concurrent pool mutations with transactional mutex", "[freertos]"
) {
    static zp_ctx_t s_test_ctx;
    static uint8_t s_test_rx[512] __attribute__((aligned(8)));
    static uint8_t s_test_pool[4096] __attribute__((aligned(8)));

    s_pool_lock = xSemaphoreCreateMutexStatic(&s_pool_lock_buf);

    s_test_ctx = (zp_ctx_t){
        .schema = &test_schema,
        .writev = dummy_writev,
        .buf = {.data = s_test_rx, .cap = sizeof(s_test_rx)},
        .pool = {.data = s_test_pool, .cap = sizeof(s_test_pool)},
    };
    TEST_ASSERT_NOT_NULL(zp_init(&s_test_ctx));

    s_done_sem = xSemaphoreCreateCountingStatic(2, 0, &s_done_sem_buf);

    xTaskCreateStatic(
        worker_pool_task1,
        "w1",
        WORKER_STACK_SIZE,
        &s_test_ctx,
        5,
        s_worker1_stack,
        &s_worker1_tcb
    );
    xTaskCreateStatic(
        worker_pool_task2,
        "w2",
        WORKER_STACK_SIZE,
        &s_test_ctx,
        5,
        s_worker2_stack,
        &s_worker2_tcb
    );

    TEST_ASSERT_TRUE(xSemaphoreTake(s_done_sem, pdMS_TO_TICKS(5000)));
    TEST_ASSERT_TRUE(xSemaphoreTake(s_done_sem, pdMS_TO_TICKS(5000)));

    TEST_ASSERT_TRUE(s_test_ctx.pool.head > 0);
}

static void worker_tx_task1(void* pv) {
    zp_ctx_t* ctx = (zp_ctx_t*)pv;
    for (int i = 0; i < 20; i++) {
        sensor_t s = {
            .sensor_id = (uint16_t)i,
            .temp_milli = 20000 + i,
            .is_active = true,
        };
        zp_send(ctx, ACTION_UPDATE_DATA, &s, 1);
    }
    xSemaphoreGive(s_done_sem);
    vTaskDelete(NULL);
}

static void worker_tx_task2(void* pv) {
    zp_ctx_t* ctx = (zp_ctx_t*)pv;
    for (int i = 0; i < 20; i++) {
        zp_send(ctx, ACTION_PING, NULL, 0);
    }
    xSemaphoreGive(s_done_sem);
    vTaskDelete(NULL);
}

TEST_CASE(
    "FreeRTOS: concurrent writev serialization with writev_lock", "[freertos]"
) {
    static zp_tcp_t s_test_tcp;
    static zp_ctx_t s_test_ctx;
    static uint8_t s_test_rx[512] __attribute__((aligned(8)));
    static uint8_t s_test_pool[512] __attribute__((aligned(8)));

    int fds[2];
    TEST_ASSERT_EQUAL(0, pipe(fds));

    s_test_tcp = (zp_tcp_t){
        .sock = fds[1],
    };
    s_test_tcp.writev_lock =
        xSemaphoreCreateMutexStatic(&s_test_tcp.writev_lock_buf);

    s_test_ctx = (zp_ctx_t){
        .schema = &test_schema,
        .transport = &s_test_tcp,
        .writev = zp_tcp_writev,
        .buf = {.data = s_test_rx, .cap = sizeof(s_test_rx)},
        .pool = {.data = s_test_pool, .cap = sizeof(s_test_pool)},
    };
    TEST_ASSERT_NOT_NULL(zp_init(&s_test_ctx));

    s_done_sem = xSemaphoreCreateCountingStatic(2, 0, &s_done_sem_buf);

    xTaskCreateStatic(
        worker_tx_task1,
        "tx1",
        WORKER_STACK_SIZE,
        &s_test_ctx,
        5,
        s_worker1_stack,
        &s_worker1_tcb
    );
    xTaskCreateStatic(
        worker_tx_task2,
        "tx2",
        WORKER_STACK_SIZE,
        &s_test_ctx,
        5,
        s_worker2_stack,
        &s_worker2_tcb
    );

    TEST_ASSERT_TRUE(xSemaphoreTake(s_done_sem, pdMS_TO_TICKS(5000)));
    TEST_ASSERT_TRUE(xSemaphoreTake(s_done_sem, pdMS_TO_TICKS(5000)));

    close(fds[1]);
    close(fds[0]);
}

TEST_CASE("RX: pull-based zp_recv decoding in FreeRTOS environment", "[rx]") {
    zp_ctx_t rx_ctx = {
        .schema = &test_schema,
        .writev = dummy_writev,
        .buf = {.data = s_rx, .cap = sizeof(s_rx)},
        .pool = {.data = s_pool, .cap = sizeof(s_pool)},
    };
    TEST_ASSERT_NOT_NULL(zp_init(&rx_ctx));

    zp_header_t ping_hdr = {
        .action = 0,
        .type_id = 0,
        .count = 0,
    };
    memcpy(rx_ctx.buf.data, &ping_hdr, sizeof(ping_hdr));
    rx_ctx.buf.head = sizeof(ping_hdr);

    zp_msg_t msg;
    TEST_ASSERT_TRUE(zp_recv(&rx_ctx, &msg));
    TEST_ASSERT_EQUAL_UINT8(0, msg.action);
    TEST_ASSERT_NULL(msg.payload);
    TEST_ASSERT_FALSE(zp_recv(&rx_ctx, &msg));
}

static void mock_tcp_worker(void* pv) {
    zp_ctx_t* ctx = (zp_ctx_t*)pv;
    zp_tcp_t* tcp = (zp_tcp_t*)ctx->transport;

    // Simulate network delay before bytes arrive
    vTaskDelay(pdMS_TO_TICKS(10));

    zp_header_t hdr = {
        .action = 0,
        .type_id = 0,
        .count = 0,
    };
    memcpy(ctx->buf.data + ctx->buf.head, &hdr, sizeof(hdr));
    zp_commit(ctx, sizeof(hdr));

    // Notify app task that bytes arrived
    if (tcp->app_task) {
        xTaskNotifyGive(tcp->app_task);
    }

    // Wait for app task to acknowledge / drain
    uint32_t val = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
    TEST_ASSERT_TRUE(val > 0);

    xSemaphoreGive(s_done_sem);
    vTaskDelete(NULL);
}

TEST_CASE(
    "FreeRTOS: bidirectional task notification (tcp_task <-> app_task)",
    "[freertos]"
) {
    static zp_tcp_t mock_tcp;
    static zp_ctx_t mock_ctx;
    static uint8_t mock_rx[256] __attribute__((aligned(8)));
    static uint8_t mock_pool[256] __attribute__((aligned(8)));

    mock_tcp = (zp_tcp_t){
        .app_task = xTaskGetCurrentTaskHandle(),
    };

    mock_ctx = (zp_ctx_t){
        .schema = &test_schema,
        .transport = &mock_tcp,
        .writev = dummy_writev,
        .buf = {.data = mock_rx, .cap = sizeof(mock_rx)},
        .pool = {.data = mock_pool, .cap = sizeof(mock_pool)},
    };
    TEST_ASSERT_NOT_NULL(zp_init(&mock_ctx));

    s_done_sem = xSemaphoreCreateCountingStatic(1, 0, &s_done_sem_buf);

    mock_tcp.tcp_task = xTaskCreateStatic(
        mock_tcp_worker,
        "mock_tcp",
        WORKER_STACK_SIZE,
        &mock_ctx,
        5,
        s_worker1_stack,
        &s_worker1_tcb
    );
    TEST_ASSERT_NOT_NULL(mock_tcp.tcp_task);

    // App task sleeps waiting for notification from mock_tcp
    uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
    TEST_ASSERT_TRUE(notified > 0);

    // Verify packet can now be polled
    zp_msg_t msg;
    TEST_ASSERT_TRUE(zp_recv(&mock_ctx, &msg));
    TEST_ASSERT_EQUAL_UINT8(0, msg.action);

    // Wake mock_tcp back up
    xTaskNotifyGive(mock_tcp.tcp_task);

    // Wait for worker to finish
    TEST_ASSERT_TRUE(xSemaphoreTake(s_done_sem, pdMS_TO_TICKS(1000)));
}

void app_main(void) {
    UNITY_BEGIN();
    unity_run_all_tests();
    UNITY_END();
    exit(0);
}
