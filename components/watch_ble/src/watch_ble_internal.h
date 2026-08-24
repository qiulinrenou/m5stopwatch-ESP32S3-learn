#ifndef WATCH_BLE_INTERNAL_H
#define WATCH_BLE_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"

#include "watch_images.h"

#define WATCH_BLE_PROTOCOL_VERSION 1U
#define WATCH_BLE_STATUS_LENGTH 16U
#define WATCH_BLE_ATT_VALUE_MAX 236U
#define WATCH_BLE_DATA_PAYLOAD_MAX 224U
#define WATCH_BLE_INACTIVITY_US (5LL * 60LL * 1000000LL)

typedef enum {
    WATCH_BLE_STATE_OFF = 0,
    WATCH_BLE_STATE_ADVERTISING = 1,
    WATCH_BLE_STATE_CONNECTED = 2,
    WATCH_BLE_STATE_RECEIVING = 3,
    WATCH_BLE_STATE_VERIFYING = 4,
    WATCH_BLE_STATE_COMPLETE = 5,
    WATCH_BLE_STATE_ERROR = 6,
} watch_ble_transfer_state_t;

typedef enum {
    WATCH_BLE_ERROR_NONE = 0,
    WATCH_BLE_ERROR_VERSION = 1,
    WATCH_BLE_ERROR_FRAME = 2,
    WATCH_BLE_ERROR_PARAMETER = 3,
    WATCH_BLE_ERROR_STATE = 4,
    WATCH_BLE_ERROR_BUSY = 5,
    WATCH_BLE_ERROR_OFFSET = 6,
    WATCH_BLE_ERROR_SPACE = 7,
    WATCH_BLE_ERROR_FILE_IO = 8,
    WATCH_BLE_ERROR_LENGTH = 9,
    WATCH_BLE_ERROR_CRC = 10,
    WATCH_BLE_ERROR_FORMAT = 11,
    WATCH_BLE_ERROR_NOT_FOUND = 12,
} watch_ble_error_t;

typedef struct {
    uint8_t version;
    uint8_t state;
    uint16_t error;
    uint32_t session_id;
    uint32_t next_offset;
    uint32_t detail;
} watch_ble_status_t;

typedef enum {
    WATCH_BLE_WORK_CONTROL = 0,
    WATCH_BLE_WORK_DATA,
    WATCH_BLE_WORK_CONNECTED,
    WATCH_BLE_WORK_DISCONNECTED,
    WATCH_BLE_WORK_SHOW_PASSKEY,
    WATCH_BLE_WORK_HIDE_PASSKEY,
} watch_ble_work_type_t;

typedef struct {
    watch_ble_work_type_t type;
    uint16_t conn_handle;
    uint16_t length;
    uint32_t value;
    uint8_t bytes[WATCH_BLE_ATT_VALUE_MAX];
} watch_ble_work_item_t;

typedef void (*watch_ble_status_publish_t)(const watch_ble_status_t *status);
typedef void (*watch_ble_image_committed_t)(uint8_t slot);

typedef struct {
    watch_image_writer_t *writer;
    watch_image_metadata_t metadata;
    uint32_t session_id;
    uint32_t next_offset;
    uint32_t committed_session_id;
    uint32_t committed_size;
    int64_t last_activity_us;
    uint8_t committed_slot;
    bool active;
    bool has_committed_session;
    watch_ble_status_publish_t publish;
    watch_ble_image_committed_t committed;
} watch_ble_protocol_t;

/**
 * @brief 注册图片传输 GATT 服务。
 *
 * @return 0 成功，其他值为 NimBLE Host 错误码。
 */
int watch_ble_gatt_init(void);

/**
 * @brief 取得 Status 特征值句柄。
 *
 * @return 已注册的值句柄；注册前为 0。
 */
uint16_t watch_ble_gatt_status_handle(void);

/**
 * @brief 从 Host 回调无阻塞复制工作项到固定队列。
 *
 * @param item 完整工作项。
 * @return true 表示入队成功。
 */
bool watch_ble_internal_enqueue(const watch_ble_work_item_t *item);

/**
 * @brief 为 Status GATT Read 复制当前 16 字节线格式。
 *
 * @param[out] bytes 输出缓冲。
 */
void watch_ble_internal_copy_status(uint8_t bytes[WATCH_BLE_STATUS_LENGTH]);

/**
 * @brief 初始化协议状态机。
 *
 * @param protocol 协议上下文。
 * @param publish 状态发布函数。
 * @param committed 图片提交事件函数。
 */
void watch_ble_protocol_init(
    watch_ble_protocol_t *protocol,
    watch_ble_status_publish_t publish,
    watch_ble_image_committed_t committed
);

/**
 * @brief 处理连接完成事件并恢复或建立协议状态。
 *
 * @param protocol 协议上下文。
 */
void watch_ble_protocol_on_connected(watch_ble_protocol_t *protocol);

/**
 * @brief 处理 Control 特征完整值。
 *
 * @param protocol 协议上下文。
 * @param bytes 帧数据。
 * @param length 帧长度。
 */
void watch_ble_protocol_control(
    watch_ble_protocol_t *protocol,
    const uint8_t *bytes,
    size_t length
);

/**
 * @brief 处理 Data 特征完整值。
 *
 * @param protocol 协议上下文。
 * @param bytes 帧数据。
 * @param length 帧长度。
 */
void watch_ble_protocol_data(
    watch_ble_protocol_t *protocol,
    const uint8_t *bytes,
    size_t length
);

/**
 * @brief 检查并取消五分钟无数据的 RAM 会话。
 *
 * @param protocol 协议上下文。
 * @param now_us 当前单调时钟微秒值。
 */
void watch_ble_protocol_poll_timeout(
    watch_ble_protocol_t *protocol,
    int64_t now_us
);

#endif
