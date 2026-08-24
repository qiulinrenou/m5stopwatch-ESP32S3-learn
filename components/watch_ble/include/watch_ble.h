#ifndef WATCH_BLE_H
#define WATCH_BLE_H

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WATCH_BLE_DEVICE_NAME "SF-BADGE"
#define WATCH_BLE_SERVICE_UUID "1E7A3B6F-5A4D-219C-0145-474441424653"
#define WATCH_BLE_CONTROL_UUID "1E7A3B6F-5A4D-219C-0245-474441424653"
#define WATCH_BLE_DATA_UUID "1E7A3B6F-5A4D-219C-0345-474441424653"
#define WATCH_BLE_STATUS_UUID "1E7A3B6F-5A4D-219C-0445-474441424653"

typedef enum {
    WATCH_BLE_EVENT_SHOW_PAIRING_CODE = 0,
    WATCH_BLE_EVENT_HIDE_PAIRING_CODE,
    WATCH_BLE_EVENT_IMAGE_COMMITTED,
} watch_ble_event_type_t;

typedef struct {
    watch_ble_event_type_t type;
    union {
        uint32_t passkey;
        uint8_t slot;
    } data;
} watch_ble_event_t;

typedef void (*watch_ble_event_callback_t)(
    const watch_ble_event_t *event,
    void *user_data
);

typedef struct {
    watch_ble_event_callback_t event_callback;
    void *user_data;
} watch_ble_config_t;

/**
 * @brief 启动 ESP32-S3 BLE-only NimBLE 外设与图片协议工作任务。
 *
 * @param config 非阻塞应用事件回调配置，可为 NULL。
 * @return ESP_OK 表示广播已经交由 NimBLE 启动；失败不影响原手表功能。
 * @note 回调由 BLE 工作任务调用，不在 NimBLE Host 回调或 LVGL 线程中执行。
 */
esp_err_t watch_ble_start(const watch_ble_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
