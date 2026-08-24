#include "watch_ble_internal.h"

#include <stdint.h>
#include <string.h>

#include "host/ble_att.h"
#include "host/ble_hs.h"
#include "os/os_mbuf.h"

typedef enum {
    WATCH_BLE_CHARACTERISTIC_CONTROL = 0,
    WATCH_BLE_CHARACTERISTIC_DATA,
    WATCH_BLE_CHARACTERISTIC_STATUS,
} watch_ble_characteristic_t;

static const ble_uuid128_t s_service_uuid = BLE_UUID128_INIT(
    0x53, 0x46, 0x42, 0x41, 0x44, 0x47, 0x45, 0x01,
    0x9C, 0x21, 0x4D, 0x5A, 0x6F, 0x3B, 0x7A, 0x1E
);
static const ble_uuid128_t s_control_uuid = BLE_UUID128_INIT(
    0x53, 0x46, 0x42, 0x41, 0x44, 0x47, 0x45, 0x02,
    0x9C, 0x21, 0x4D, 0x5A, 0x6F, 0x3B, 0x7A, 0x1E
);
static const ble_uuid128_t s_data_uuid = BLE_UUID128_INIT(
    0x53, 0x46, 0x42, 0x41, 0x44, 0x47, 0x45, 0x03,
    0x9C, 0x21, 0x4D, 0x5A, 0x6F, 0x3B, 0x7A, 0x1E
);
static const ble_uuid128_t s_status_uuid = BLE_UUID128_INIT(
    0x53, 0x46, 0x42, 0x41, 0x44, 0x47, 0x45, 0x04,
    0x9C, 0x21, 0x4D, 0x5A, 0x6F, 0x3B, 0x7A, 0x1E
);

static uint16_t s_status_value_handle;

/**
 * @brief 把 Control/Data mbuf 拷贝到固定工作项并立即返回。
 *
 * @param connection_handle 当前连接句柄。
 * @param context NimBLE GATT 访问上下文。
 * @param type 工作项类型。
 * @return 0 成功，否则为 ATT 错误码。
 */
static int enqueue_write(
    uint16_t connection_handle,
    struct ble_gatt_access_ctxt *context,
    watch_ble_work_type_t type
)
{
    const uint16_t length = OS_MBUF_PKTLEN(context->om);
    if (length == 0U || length > WATCH_BLE_ATT_VALUE_MAX) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    watch_ble_work_item_t item = {
        .type = type,
        .conn_handle = connection_handle,
        .length = length,
    };
    uint16_t copied = 0U;
    const int result = ble_hs_mbuf_to_flat(
        context->om,
        item.bytes,
        sizeof(item.bytes),
        &copied
    );
    if (result != 0 || copied != length) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    return watch_ble_internal_enqueue(&item) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

/**
 * @brief 处理三项图片 GATT 特征访问。
 *
 * @param connection_handle 当前连接句柄。
 * @param attribute_handle 当前属性句柄。
 * @param context NimBLE 访问上下文。
 * @param argument 注册时传入的特征类型。
 * @return 0 成功，否则为 ATT 错误码。
 */
static int gatt_access(
    uint16_t connection_handle,
    uint16_t attribute_handle,
    struct ble_gatt_access_ctxt *context,
    void *argument
)
{
    (void)attribute_handle;
    const watch_ble_characteristic_t characteristic =
        (watch_ble_characteristic_t)(uintptr_t)argument;

    if (characteristic == WATCH_BLE_CHARACTERISTIC_STATUS) {
        if (context->op != BLE_GATT_ACCESS_OP_READ_CHR) {
            return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
        }
        uint8_t status[WATCH_BLE_STATUS_LENGTH];
        watch_ble_internal_copy_status(status);
        return os_mbuf_append(context->om, status, sizeof(status)) == 0
            ? 0
            : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (context->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_READ_NOT_PERMITTED;
    }
    return enqueue_write(
        connection_handle,
        context,
        characteristic == WATCH_BLE_CHARACTERISTIC_CONTROL
            ? WATCH_BLE_WORK_CONTROL
            : WATCH_BLE_WORK_DATA
    );
}

static const struct ble_gatt_svc_def s_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &s_control_uuid.u,
                .access_cb = gatt_access,
                .arg = (void *)(uintptr_t)WATCH_BLE_CHARACTERISTIC_CONTROL,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = &s_data_uuid.u,
                .access_cb = gatt_access,
                .arg = (void *)(uintptr_t)WATCH_BLE_CHARACTERISTIC_DATA,
                .flags = BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &s_status_uuid.u,
                .access_cb = gatt_access,
                .arg = (void *)(uintptr_t)WATCH_BLE_CHARACTERISTIC_STATUS,
                .val_handle = &s_status_value_handle,
                .flags = BLE_GATT_CHR_F_READ |
                         BLE_GATT_CHR_F_NOTIFY,
            },
            {0},
        },
    },
    {0},
};

/**
 * @brief 计算并注册图片传输 Service 与三项受保护特征。
 *
 * @return 0 成功，其他值为 NimBLE Host 错误码。
 */
int watch_ble_gatt_init(void)
{
    int result = ble_gatts_count_cfg(s_services);
    if (result == 0) {
        result = ble_gatts_add_svcs(s_services);
    }
    return result;
}

/**
 * @brief 返回 NimBLE 注册后写入的 Status 特征值句柄。
 *
 * @return 注册前为 0，注册后为有效 ATT 句柄。
 */
uint16_t watch_ble_gatt_status_handle(void)
{
    return s_status_value_handle;
}
