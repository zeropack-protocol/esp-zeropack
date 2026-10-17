#include <stdint.h>
#include <string.h>

#include "unity.h"
#include "unity_test_runner.h"
#include "zeropack.h"
#include "zeropack/core.h"
#include "zeropack/protocol.h"
#include "zeropack/schema.h"
#include "zeropack/types.h"

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

static zp_ssize_t dummy_writev(const zp_iovec_t* iov, int iovcnt) {
    (void)iov;
    (void)iovcnt;
    return 0;
}

void setUp(void) {
    zp_init(
        &s_ctx,
        &(zp_config_t){
            .schema = &test_schema,
            .writev = dummy_writev,
            ZP_SLICE(rx, s_rx),
            ZP_SLICE(pool, s_pool),
        }
    );
}

TEST_CASE("Schema: tamaños, offsets y metadata correctos", "[zeropack]") {
    // Verificamos el tamaño total de la estructura en zp_schema
    TEST_ASSERT_EQUAL(sizeof(sensor_t), zp_type_size(sensor_t));
    TEST_ASSERT_EQUAL_STRING("sensor_t", zp_type_name(sensor_t));
    TEST_ASSERT_EQUAL_PTR(zp_type_meta(sensor_t), test_schema.types[0]);
    TEST_ASSERT_EQUAL(3, zp_type_meta(sensor_t)->count);

    // Verificamos que los offsets de cada campo coincidan con offsetof del
    // compilador (ABI)
    TEST_ASSERT_EQUAL(
        offsetof(sensor_t, sensor_id), zp_type_meta(sensor_t)->fields[0].offset
    );
    TEST_ASSERT_EQUAL(
        offsetof(sensor_t, temp_milli), zp_type_meta(sensor_t)->fields[1].offset
    );
    TEST_ASSERT_EQUAL(
        offsetof(sensor_t, is_active), zp_type_meta(sensor_t)->fields[2].offset
    );
    // Verificamos que las 3 acciones estén registradas
    TEST_ASSERT_EQUAL(3, test_schema.action_count);
    TEST_ASSERT_EQUAL_STRING("ACTION_PING", test_schema.actions[0].name);
    TEST_ASSERT_EQUAL_STRING("ACTION_UPDATE_DATA", test_schema.actions[1].name);
    TEST_ASSERT_EQUAL_STRING("ACTION_SEND_BUFFER", test_schema.actions[2].name);
}

static sensor_t s_sensor = {0};
static void on_sensor_update(zp_ctx_t* _, const sensor_t* data) {
    s_sensor = *data;
}

TEST_CASE("Dispatch: cast correcto y ejecución de callback", "[zeropack]") {
    zp_on_action(test_schema, ACTION_UPDATE_DATA, on_sensor_update);

    sensor_t sensor_in = {
        .sensor_id = 42,
        .temp_milli = -1550,
        .is_active = true,
    };

    zp_header_t hdr = {
        .action = ACTION_UPDATE_DATA,
        .type_id = zp_type_id(&test_schema, sensor_t),
        .count = 1,
    };

    bool ok = test_schema.dispatch(&s_ctx, &hdr, (const uint8_t*)&sensor_in);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT16(42, s_sensor.sensor_id);
    TEST_ASSERT_EQUAL_INT32(-1550, s_sensor.temp_milli);
    TEST_ASSERT_TRUE(s_sensor.is_active);
}

static uint16_t s_arr_count = 0;
static uint8_t s_buffer[16] = {0};

static void on_buffer_received(
    zp_ctx_t* _, const uint8_t* data, uint16_t count
) {
    s_arr_count = count;
    memcpy(s_buffer, data, count);
}

TEST_CASE("Dispatch: accion de tipo arreglo dinamico (slice)", "[zeropack]") {
    zp_on_action(test_schema, ACTION_SEND_BUFFER, on_buffer_received);
    uint8_t data[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
    zp_header_t hdr = {
        .action = ACTION_SEND_BUFFER,
        .type_id = TYPE_ID_u8,
        .count = 5,
    };

    bool ok = test_schema.dispatch(&s_ctx, &hdr, data);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT16(5, s_arr_count);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(data, s_buffer, 5);
}

TEST_CASE(
    "Seguridad: descarte de paquetes incompletos o desconocidos", "[zeropack]"
) {
    s_sensor.sensor_id = 0;
    zp_on_action(test_schema, ACTION_UPDATE_DATA, on_sensor_update);

    zp_header_t bad_hdr = {
        .action = ACTION_UPDATE_DATA, .type_id = TYPE_ID_u8, .count = 2
    };

    // Payload malformado: su longitud (2 bytes) es menor que sizeof(sensor_t)
    uint8_t bad_payload[2] = {0x01, 0x02};

    uint8_t* buf = zp_rx_buf(&s_ctx);
    memcpy(buf, &bad_hdr, sizeof(bad_hdr));
    memcpy(buf + sizeof(bad_hdr), bad_payload, sizeof(bad_payload));
    zp_rx_commit(&s_ctx, sizeof(bad_hdr) + sizeof(bad_payload));

    // El callback no debió ejecutarse y el paquete malformado fue descartado.
    TEST_ASSERT_EQUAL_UINT16(0, s_sensor.sensor_id);
    TEST_ASSERT_EQUAL_UINT32(0, s_sensor.sensor_id);

    // Acción desconocida en dispatch
    zp_header_t invalid_hdr = {.action = 99};
    bool ok =
        test_schema.dispatch(&s_ctx, &invalid_hdr, (const uint8_t*)&s_sensor);
    TEST_ASSERT_FALSE(ok);
}

TEST_CASE(
    "Pool: almacenamiento lineal, str/val, mutación y reset", "[zeropack]"
) {
    // Asegurar estado inicial limpio
    zp_pool_reset(&s_ctx);
    TEST_ASSERT_EQUAL_UINT32(0, s_ctx.pool_len);

    // Guardar un string con zp_pool_str (calcula strlen + 1)
    poolref_t ref_str = zp_pool_str(&s_ctx, "ZeroPack String");
    TEST_ASSERT_EQUAL_UINT16(0, ref_str);

    // Recuperar y verificar el string con zp_pool_get
    char* str = zp_pool_get(&s_ctx, ref_str);
    TEST_ASSERT_EQUAL_STRING("ZeroPack String", str);
    size_t str_slot_size = ZP_SLOT_TOTAL_SIZE(strlen("ZeroPack String") + 1);
    TEST_ASSERT_EQUAL_UINT32(str_slot_size, s_ctx.pool_len);

    // Guardar un struct por valor con zp_pool_val
    sensor_t s = {
        .sensor_id = 101,
        .temp_milli = 24500,
        .is_active = true,
    };
    poolref_t ref_struct = zp_pool_val(&s_ctx, sensor_t, s);
    TEST_ASSERT_EQUAL_UINT16(str_slot_size, ref_struct);

    // Recuperar el struct y probar mutación in-place en el pool
    sensor_t* rec_s = zp_pool_get(&s_ctx, ref_struct);
    TEST_ASSERT_EQUAL_UINT16(101, rec_s->sensor_id);
    TEST_ASSERT_EQUAL_INT32(24500, rec_s->temp_milli);

    rec_s->temp_milli = 30000;  // Mutamos in-place
    sensor_t* mutated_s = zp_pool_get(&s_ctx, ref_struct);
    TEST_ASSERT_EQUAL_INT32(30000, mutated_s->temp_milli);

    // Probar zp_pool_reset() y reutilización desde el offset 0
    zp_pool_reset(&s_ctx);
    TEST_ASSERT_EQUAL_UINT32(0, s_ctx.pool_len);

    poolref_t ref_nuevo = zp_pool_str(&s_ctx, "Reinicio");
    TEST_ASSERT_EQUAL_UINT16(0, ref_nuevo);
    TEST_ASSERT_EQUAL_STRING("Reinicio", (char*)zp_pool_get(&s_ctx, ref_nuevo));
}

TEST_CASE(
    "Pipeline RX: zp_rx_get_buffer y zp_rx_commit con reensamblado",
    "[zeropack]"
) {
    s_sensor.sensor_id = 0;
    zp_on_action(test_schema, ACTION_UPDATE_DATA, on_sensor_update);

    // Probar recepción directa (Zero-Copy)
    size_t avail = zp_rx_avail(&s_ctx);
    uint8_t* buf = zp_rx_buf(&s_ctx);
    TEST_ASSERT_NOT_NULL(buf);
    TEST_ASSERT_TRUE(avail >= sizeof(zp_header_t) + sizeof(sensor_t));

    zp_header_t hdr = {
        .action = ACTION_UPDATE_DATA,
        .type_id = zp_type_id(&test_schema, sensor_t),
        .count = 1,
    };
    sensor_t sensor_in = {
        .sensor_id = 777,
        .temp_milli = 2100,
        .is_active = true,
    };

    // Escribimos directo en el buffer de recepción prestado
    memcpy(buf, &hdr, sizeof(hdr));
    memcpy(buf + sizeof(hdr), &sensor_in, sizeof(sensor_in));

    // Notificamos los bytes recibidos
    zp_rx_commit(&s_ctx, sizeof(hdr) + sizeof(sensor_in));

    // Verificamos que el paquete fue parseado y el callback ejecutado
    TEST_ASSERT_EQUAL_UINT16(777, s_sensor.sensor_id);
    TEST_ASSERT_EQUAL_INT32(2100, s_sensor.temp_milli);
    TEST_ASSERT_TRUE(s_sensor.is_active);

    // Probar paquete fragmentado en 2 chunks (reensamblado en buffer)
    s_sensor.sensor_id = 0;
    sensor_in.sensor_id = 888;
    sensor_in.temp_milli = -500;

    buf = zp_rx_buf(&s_ctx);
    TEST_ASSERT_NOT_NULL(buf);

    // Chunk 1: solo 2 bytes del header
    memcpy(buf, &hdr, 2);
    zp_rx_commit(&s_ctx, 2);
    TEST_ASSERT_EQUAL_UINT16(0, s_sensor.sensor_id);  // No debe disparar aún

    // Chunk 2: el resto del header (2 bytes) + payload completo
    buf = zp_rx_buf(&s_ctx);
    TEST_ASSERT_NOT_NULL(buf);
    memcpy(buf, ((const uint8_t*)&hdr) + 2, sizeof(hdr) - 2);
    memcpy(buf + (sizeof(hdr) - 2), &sensor_in, sizeof(sensor_in));
    zp_rx_commit(&s_ctx, (sizeof(hdr) - 2) + sizeof(sensor_in));

    // Debe completarse el paquete tras el segundo chunk
    TEST_ASSERT_EQUAL_UINT16(888, s_sensor.sensor_id);
    TEST_ASSERT_EQUAL_INT32(-500, s_sensor.temp_milli);

    // Verificamos que el buffer se haya compactado y liberado por completo
    TEST_ASSERT_EQUAL_UINT32(s_ctx.rx_cap, zp_rx_avail(&s_ctx));
}

void app_main(void) {
    UNITY_BEGIN();
    unity_run_tests_by_tag("[zeropack]", false);
    UNITY_END();
}
