#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "unity.h"
#include "unity_test_runner.h"
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

ZP_DECLARE(TEST_TYPES, TEST_ACTIONS)
ZP_GENERATE_SCHEMA(test_schema, TEST_TYPES, TEST_ACTIONS)

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
        ZP_BUFFER(rx, s_rx),
        ZP_BUFFER(pool, s_pool),
    };
    zp_init(&s_ctx);
}

// Target ABI & Memory Smoke Tests =============================================

TEST_CASE("Schema: ABI, struct offsets, and field metadata", "[abi]") {
    TEST_ASSERT_EQUAL(sizeof(sensor_t), zp_type_size(sensor_t));
    TEST_ASSERT_EQUAL_STRING("sensor_t", zp_type_name(sensor_t));
    TEST_ASSERT_EQUAL_PTR(zp_type_meta(sensor_t), test_schema.types[0]);
    TEST_ASSERT_EQUAL(3, zp_type_meta(sensor_t)->count);

    TEST_ASSERT_EQUAL(
        offsetof(sensor_t, sensor_id), zp_type_meta(sensor_t)->fields[0].offset
    );
    TEST_ASSERT_EQUAL(
        offsetof(sensor_t, temp_milli), zp_type_meta(sensor_t)->fields[1].offset
    );
    TEST_ASSERT_EQUAL(
        offsetof(sensor_t, is_active), zp_type_meta(sensor_t)->fields[2].offset
    );

    TEST_ASSERT_EQUAL(3, test_schema.action_count);
    TEST_ASSERT_EQUAL_STRING("ACTION_PING", test_schema.actions[0].name);
    TEST_ASSERT_EQUAL_STRING("ACTION_UPDATE_DATA", test_schema.actions[1].name);
    TEST_ASSERT_EQUAL_STRING("ACTION_SEND_BUFFER", test_schema.actions[2].name);
}

TEST_CASE("Pool: memory allocation and type-safe retrieval", "[pool]") {
    zp_pool_reset(&s_ctx);
    TEST_ASSERT_EQUAL_UINT32(0, s_ctx.pool_len);

    poolref_t ref_str = zp_pool_push_str(&s_ctx, "ZeroPack String");
    TEST_ASSERT_EQUAL_UINT16(0, ref_str);

    char* str = zp_pool_get_str(&s_ctx, ref_str);
    TEST_ASSERT_EQUAL_STRING("ZeroPack String", str);

    sensor_t s = {
        .sensor_id = 101,
        .temp_milli = 24500,
        .is_active = true,
    };
    poolref_t ref_struct = zp_pool_push_val(&s_ctx, sensor_t, s);
    sensor_t* rec_s = zp_pool_get_typed(&s_ctx, sensor_t, ref_struct);
    TEST_ASSERT_NOT_NULL(rec_s);
    TEST_ASSERT_EQUAL_UINT16(101, rec_s->sensor_id);
    TEST_ASSERT_EQUAL_INT32(24500, rec_s->temp_milli);

    zp_pool_reset(&s_ctx);
    TEST_ASSERT_EQUAL_UINT32(0, s_ctx.pool_len);
}

// Transport & FreeRTOS Concurrency Tests ======================================

TEST_CASE("TCP: multi-instance isolation and offline handling", "[tcp]") {
    static zp_tcp_t s_tcp1;
    static zp_tcp_t s_tcp2;

    // Both instances have distinct stack buffers and TCBs in BSS
    TEST_ASSERT_NOT_EQUAL((void*)s_tcp1.stack, (void*)s_tcp2.stack);
    TEST_ASSERT_NOT_EQUAL((void*)&s_tcp1.tcb, (void*)&s_tcp2.tcb);
    TEST_ASSERT_NOT_EQUAL((void*)&s_tcp1.lock_bufs, (void*)&s_tcp2.lock_bufs);

    // Offline writev returns -1 cleanly when socket is closed
    s_tcp1.sock = -1;
    zp_ctx_t ctx1 = {.transport = &s_tcp1};
    zp_iovec_t iov = {.buf = "abc", .len = 3};
    TEST_ASSERT_EQUAL(-1, zp_tcp_writev(&ctx1, &iov, 1));
}

#define WORKER_STACK_SIZE 4096
static StaticTask_t s_worker1_tcb;
static StackType_t s_worker1_stack[WORKER_STACK_SIZE];
static StaticTask_t s_worker2_tcb;
static StackType_t s_worker2_stack[WORKER_STACK_SIZE];

static SemaphoreHandle_t s_done_sem;
static StaticSemaphore_t s_done_sem_buf;

static void worker_task1(void* pv) {
    zp_ctx_t* ctx = (zp_ctx_t*)pv;
    for (int i = 0; i < 50; i++) {
        zp_pool_push_str(ctx, "worker1_item");
    }
    xSemaphoreGive(s_done_sem);
    vTaskDelete(NULL);
}

static void worker_task2(void* pv) {
    zp_ctx_t* ctx = (zp_ctx_t*)pv;
    for (int i = 0; i < 50; i++) {
        zp_pool_push_str(ctx, "worker2_item");
    }
    xSemaphoreGive(s_done_sem);
    vTaskDelete(NULL);
}

TEST_CASE(
    "FreeRTOS: concurrent pool mutations with zp_tcp_lock", "[freertos]"
) {
    static zp_tcp_t s_test_tcp;
    static zp_ctx_t s_test_ctx;
    static uint8_t s_test_rx[512] __attribute__((aligned(8)));
    static uint8_t s_test_pool[4096] __attribute__((aligned(8)));

    s_test_tcp = (zp_tcp_t){0};
    s_test_tcp.locks[ZP_LOCK_TX] =
        xSemaphoreCreateMutexStatic(&s_test_tcp.lock_bufs[ZP_LOCK_TX]);
    s_test_tcp.locks[ZP_LOCK_POOL] =
        xSemaphoreCreateMutexStatic(&s_test_tcp.lock_bufs[ZP_LOCK_POOL]);

    s_test_ctx = (zp_ctx_t){
        .schema = &test_schema,
        .transport = &s_test_tcp,
        .lock = zp_tcp_lock,
        .unlock = zp_tcp_unlock,
        .writev = zp_tcp_writev,
        ZP_BUFFER(rx, s_test_rx),
        ZP_BUFFER(pool, s_test_pool),
    };
    TEST_ASSERT_NOT_NULL(zp_init(&s_test_ctx));

    s_done_sem = xSemaphoreCreateCountingStatic(2, 0, &s_done_sem_buf);

    xTaskCreateStatic(
        worker_task1,
        "w1",
        WORKER_STACK_SIZE,
        &s_test_ctx,
        5,
        s_worker1_stack,
        &s_worker1_tcb
    );
    xTaskCreateStatic(
        worker_task2,
        "w2",
        WORKER_STACK_SIZE,
        &s_test_ctx,
        5,
        s_worker2_stack,
        &s_worker2_tcb
    );

    TEST_ASSERT_TRUE(xSemaphoreTake(s_done_sem, pdMS_TO_TICKS(5000)));
    TEST_ASSERT_TRUE(xSemaphoreTake(s_done_sem, pdMS_TO_TICKS(5000)));

    TEST_ASSERT_TRUE(s_test_ctx.pool_len > 0);
}

void app_main(void) {
    UNITY_BEGIN();
    unity_run_all_tests();
    UNITY_END();
    exit(0);
}
