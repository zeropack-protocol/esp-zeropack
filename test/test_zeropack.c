#include <stdint.h>
#include <string.h>

#include "unity.h"
#include "unity_test_runner.h"
#include "zeropack.h"

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
ZP_GENERATE_SCHEMA(TEST_TYPES, TEST_ACTIONS)

TEST_CASE("Schema: tamaños, offsets y metadata correctos", "[zeropack]") {
    // Verificamos el tamaño total de la estructura en zp_schema
    TEST_ASSERT_EQUAL(sizeof(sensor_t), zp_schema.types[0].size);
    TEST_ASSERT_EQUAL_STRING("sensor_t", zp_schema.types[0].name);
    TEST_ASSERT_EQUAL(3, zp_schema.types[0].count);

    // Verificamos que los offsets de cada campo coincidan con offsetof del
    // compilador (ABI)
    TEST_ASSERT_EQUAL(
        offsetof(sensor_t, sensor_id), zp_schema.types[0].fields[0].offset
    );
    TEST_ASSERT_EQUAL(
        offsetof(sensor_t, temp_milli), zp_schema.types[0].fields[1].offset
    );
    TEST_ASSERT_EQUAL(
        offsetof(sensor_t, is_active), zp_schema.types[0].fields[2].offset
    );
    // Verificamos que las 3 acciones estén registradas
    TEST_ASSERT_EQUAL(3, zp_schema.action_count);
    TEST_ASSERT_EQUAL_STRING("ACTION_PING", zp_schema.actions[0].name);
    TEST_ASSERT_EQUAL_STRING("ACTION_UPDATE_DATA", zp_schema.actions[1].name);
    TEST_ASSERT_EQUAL_STRING("ACTION_SEND_BUFFER", zp_schema.actions[2].name);
}

static sensor_t s_sensor = {0};
static void on_sensor_update(const sensor_t* data) { s_sensor = *data; }

TEST_CASE("Dispatch: cast correcto y ejecución de callback", "[zeropack]") {
    zp_on_action(ACTION_UPDATE_DATA, on_sensor_update);

    sensor_t sensor_in = {
        .sensor_id = 42,
        .temp_milli = -1550,
        .is_active = true,
    };

    zp_header_t hdr = {
        .action = ACTION_UPDATE_DATA,
        .type_id = TYPE_ID_sensor_t,
        .count = 1,
    };

    bool ok =
        zp_schema.dispatch(&hdr, (const uint8_t*)&sensor_in, sizeof(sensor_t));

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT16(42, s_sensor.sensor_id);
    TEST_ASSERT_EQUAL_INT32(-1550, s_sensor.temp_milli);
    TEST_ASSERT_TRUE(s_sensor.is_active);
}

static uint16_t s_arr_count = 0;
static uint8_t s_buffer[16] = {0};

static void on_buffer_received(const uint8_t* data, uint16_t count) {
    s_arr_count = count;
    memcpy(s_buffer, data, count);
}

TEST_CASE("Dispatch: accion de tipo arreglo dinamico (slice)", "[zeropack]") {
    zp_on_action(ACTION_SEND_BUFFER, on_buffer_received);
    uint8_t data[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
    zp_header_t hdr = {
        .action = ACTION_SEND_BUFFER,
        .type_id = TYPE_ID_u8,
        .count = 5,
    };

    bool ok = zp_schema.dispatch(&hdr, data, sizeof(data));
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT16(5, s_arr_count);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(data, s_buffer, 5);
}

TEST_CASE(
    "Seguridad: descarte de paquetes incompletos o desconocidos", "[zeropack]"
) {
    zp_on_action(ACTION_UPDATE_DATA, on_sensor_update);
    zp_header_t valid_hdr = {.action = ACTION_UPDATE_DATA};

    // Payload incompleto: su longitud (2 bytes) es menor que sizeof(sensor_t)
    uint8_t incomplete_payload[2] = {0x01, 0x02};

    bool ok = zp_schema.dispatch(
        &valid_hdr, incomplete_payload, sizeof(incomplete_payload)
    );
    TEST_ASSERT_FALSE(ok);

    zp_header_t invalid_hdr = {.action = 99};
    ok = zp_schema.dispatch(
        &invalid_hdr, (const uint8_t*)&s_sensor, sizeof(sensor_t)
    );
    TEST_ASSERT_FALSE(ok);
}

TEST_CASE(
    "Pool: almacenamiento lineal, str/val, mutación y reset", "[zeropack]"
) {
    // Asegurar estado inicial limpio
    zp_pool_reset();
    TEST_ASSERT_EQUAL_UINT32(0, zp_pool_used());

    // Guardar un string con zp_pool_str (calcula strlen + 1)
    poolref_t ref_str = zp_pool_str("ZeroPack String");
    TEST_ASSERT_EQUAL_UINT16(0, ref_str);

    // Recuperar y verificar el string con zp_pool_get
    char* str = zp_pool_get(ref_str);
    TEST_ASSERT_EQUAL_STRING("ZeroPack String", str);
    size_t str_slot_size = ZP_SLOT_TOTAL_SIZE(strlen("ZeroPack String") + 1);
    TEST_ASSERT_EQUAL_UINT32(str_slot_size, zp_pool_used());

    // Guardar un struct por valor con zp_pool_val
    sensor_t s = {
        .sensor_id = 101,
        .temp_milli = 24500,
        .is_active = true,
    };
    poolref_t ref_struct = zp_pool_val(sensor_t, s);
    TEST_ASSERT_EQUAL_UINT16(str_slot_size, ref_struct);

    // Recuperar el struct y probar mutación in-place en el pool
    sensor_t* rec_s = zp_pool_get(ref_struct);
    TEST_ASSERT_EQUAL_UINT16(101, rec_s->sensor_id);
    TEST_ASSERT_EQUAL_INT32(24500, rec_s->temp_milli);

    rec_s->temp_milli = 30000;  // Mutamos in-place
    sensor_t* mutated_s = zp_pool_get(ref_struct);
    TEST_ASSERT_EQUAL_INT32(30000, mutated_s->temp_milli);

    // Probar zp_pool_reset() y reutilización desde el offset 0
    zp_pool_reset();
    TEST_ASSERT_EQUAL_UINT32(0, zp_pool_used());

    poolref_t ref_nuevo = zp_pool_str("Reinicio");
    TEST_ASSERT_EQUAL_UINT16(0, ref_nuevo);
    TEST_ASSERT_EQUAL_STRING("Reinicio", (char*)zp_pool_get(ref_nuevo));
}

TEST_CASE(
    "Pipeline RX: zp_rx_get_buffer y zp_rx_commit con reensamblado",
    "[zeropack]"
) {
    s_sensor.sensor_id = 0;
    zp_on_action(ACTION_UPDATE_DATA, on_sensor_update);

    // Probar recepción directa (Zero-Copy)
    size_t avail = 0;
    uint8_t* buf = zp_rx_get_buffer(&avail);
    TEST_ASSERT_NOT_NULL(buf);
    TEST_ASSERT_TRUE(avail >= sizeof(zp_header_t) + sizeof(sensor_t));

    zp_header_t hdr = {
        .action = ACTION_UPDATE_DATA,
        .type_id = TYPE_ID_sensor_t,
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
    zp_rx_commit(sizeof(hdr) + sizeof(sensor_in));

    // Verificamos que el paquete fue parseado y el callback ejecutado
    TEST_ASSERT_EQUAL_UINT16(777, s_sensor.sensor_id);
    TEST_ASSERT_EQUAL_INT32(2100, s_sensor.temp_milli);
    TEST_ASSERT_TRUE(s_sensor.is_active);

    // Probar paquete fragmentado en 2 chunks (reensamblado en buffer)
    s_sensor.sensor_id = 0;
    sensor_in.sensor_id = 888;
    sensor_in.temp_milli = -500;

    buf = zp_rx_get_buffer(&avail);
    TEST_ASSERT_NOT_NULL(buf);

    // Chunk 1: solo 2 bytes del header
    memcpy(buf, &hdr, 2);
    zp_rx_commit(2);
    TEST_ASSERT_EQUAL_UINT16(0, s_sensor.sensor_id);  // No debe disparar aún

    // Chunk 2: el resto del header (2 bytes) + payload completo
    buf = zp_rx_get_buffer(&avail);
    TEST_ASSERT_NOT_NULL(buf);
    memcpy(buf, ((const uint8_t*)&hdr) + 2, sizeof(hdr) - 2);
    memcpy(buf + (sizeof(hdr) - 2), &sensor_in, sizeof(sensor_in));
    zp_rx_commit((sizeof(hdr) - 2) + sizeof(sensor_in));

    // Debe completarse el paquete tras el segundo chunk
    TEST_ASSERT_EQUAL_UINT16(888, s_sensor.sensor_id);
    TEST_ASSERT_EQUAL_INT32(-500, s_sensor.temp_milli);

    // Verificamos que el buffer se haya compactado y liberado por completo
    size_t final_avail = 0;
    (void)zp_rx_get_buffer(&final_avail);
    TEST_ASSERT_EQUAL_UINT32(ZP_BUFFER_SIZE, final_avail);
}
