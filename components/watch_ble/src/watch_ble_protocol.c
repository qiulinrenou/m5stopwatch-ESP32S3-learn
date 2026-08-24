#include "watch_ble_internal.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_timer.h"
#include "esp_log.h"

#define WATCH_BLE_OPCODE_START 1U
#define WATCH_BLE_OPCODE_END 2U
#define WATCH_BLE_OPCODE_CANCEL 3U
#define WATCH_BLE_OPCODE_QUERY 4U
#define WATCH_BLE_IMAGE_FORMAT_JPEG 1U
#define WATCH_BLE_CONTROL_SIMPLE_LENGTH 8U
#define WATCH_BLE_CONTROL_START_LENGTH 24U
#define WATCH_BLE_DATA_HEADER_LENGTH 12U
#define WATCH_BLE_PROGRESS_LOG_INTERVAL (16U * 1024U)

static const char *TAG = "watch_ble_protocol";

/**
 * @brief 从网络帧读取小端 16 位字段。
 *
 * @param source 输入地址。
 * @return 解码值。
 */
static uint16_t watch_ble_read_le16(const uint8_t *source)
{
    return (uint16_t)((uint16_t)source[0] | ((uint16_t)source[1] << 8U));
}

/**
 * @brief 从网络帧读取小端 32 位字段。
 *
 * @param source 输入地址。
 * @return 解码值。
 */
static uint32_t watch_ble_read_le32(const uint8_t *source)
{
    return (uint32_t)source[0] |
           ((uint32_t)source[1] << 8U) |
           ((uint32_t)source[2] << 16U) |
           ((uint32_t)source[3] << 24U);
}

/**
 * @brief 发布一份协议状态，保证所有保留字和版本一致。
 *
 * @param protocol 协议上下文。
 * @param state 传输状态。
 * @param error 错误码。
 * @param session_id 会话编号。
 * @param next_offset 下一偏移。
 * @param detail 状态附加值。
 */
static void publish(
    watch_ble_protocol_t *protocol,
    watch_ble_transfer_state_t state,
    watch_ble_error_t error,
    uint32_t session_id,
    uint32_t next_offset,
    uint32_t detail
)
{
    const watch_ble_status_t status = {
        .version = WATCH_BLE_PROTOCOL_VERSION,
        .state = (uint8_t)state,
        .error = (uint16_t)error,
        .session_id = session_id,
        .next_offset = next_offset,
        .detail = detail,
    };
    protocol->publish(&status);
}

/**
 * @brief 发布针对当前会话的 ERROR 状态。
 *
 * @param protocol 协议上下文。
 * @param error 错误码。
 * @param session_id 收到的会话编号，0 表示使用当前会话。
 * @param detail 错误附加值。
 */
static void publish_error(
    watch_ble_protocol_t *protocol,
    watch_ble_error_t error,
    uint32_t session_id,
    uint32_t detail
)
{
    const uint32_t effective_session = session_id != 0U
        ? session_id
        : protocol->session_id;
    const uint32_t next_offset = protocol->active ? protocol->next_offset : 0U;
    ESP_LOGW(
        TAG,
        "协议错误 session=%" PRIu32 ", error=%u, offset=%" PRIu32 ", detail=%" PRIu32,
        effective_session,
        (unsigned int)error,
        next_offset,
        detail
    );
    publish(
        protocol,
        WATCH_BLE_STATE_ERROR,
        error,
        effective_session,
        next_offset,
        detail
    );
}

/**
 * @brief 判断新 START 是否与当前活动会话完全相同。
 *
 * @param protocol 协议上下文。
 * @param session_id 新会话编号。
 * @param metadata 新元数据。
 * @return true 表示可作为幂等 START 应答。
 */
static bool session_matches(
    const watch_ble_protocol_t *protocol,
    uint32_t session_id,
    const watch_image_metadata_t *metadata
)
{
    return protocol->active && protocol->session_id == session_id &&
           protocol->metadata.slot == metadata->slot &&
           protocol->metadata.width == metadata->width &&
           protocol->metadata.height == metadata->height &&
           protocol->metadata.total_size == metadata->total_size &&
           protocol->metadata.crc32 == metadata->crc32;
}

/**
 * @brief 把存储层错误映射为线协议错误。
 *
 * @param result ESP-IDF 错误码。
 * @return 协议错误码。
 */
static watch_ble_error_t map_storage_error(esp_err_t result)
{
    if (result == ESP_ERR_NO_MEM) {
        return WATCH_BLE_ERROR_SPACE;
    }
    if (result == ESP_ERR_INVALID_SIZE) {
        return WATCH_BLE_ERROR_LENGTH;
    }
    if (result == ESP_ERR_INVALID_CRC) {
        return WATCH_BLE_ERROR_CRC;
    }
    if (result == ESP_ERR_INVALID_RESPONSE || result == ESP_ERR_NOT_SUPPORTED) {
        return WATCH_BLE_ERROR_FORMAT;
    }
    return WATCH_BLE_ERROR_FILE_IO;
}

/**
 * @brief 取消并清空当前协议会话。
 *
 * @param protocol 协议上下文。
 */
static void reset_session(watch_ble_protocol_t *protocol)
{
    if (protocol->writer != NULL) {
        watch_images_cancel(protocol->writer);
    }
    protocol->writer = NULL;
    protocol->metadata = (watch_image_metadata_t){0};
    protocol->session_id = 0U;
    protocol->next_offset = 0U;
    protocol->last_activity_us = 0;
    protocol->active = false;
}

/**
 * @brief 处理 START 控制帧。
 *
 * @param protocol 协议上下文。
 * @param bytes 完整 24 字节帧。
 */
static void handle_start(
    watch_ble_protocol_t *protocol,
    const uint8_t bytes[WATCH_BLE_CONTROL_START_LENGTH]
)
{
    const uint32_t session_id = watch_ble_read_le32(bytes + 4U);
    const watch_image_metadata_t metadata = {
        .slot = bytes[8],
        .width = watch_ble_read_le16(bytes + 10U),
        .height = watch_ble_read_le16(bytes + 12U),
        .total_size = watch_ble_read_le32(bytes + 16U),
        .crc32 = watch_ble_read_le32(bytes + 20U),
    };

    if (session_id == 0U || bytes[9] != WATCH_BLE_IMAGE_FORMAT_JPEG ||
        watch_ble_read_le16(bytes + 14U) != 0U || metadata.slot >= WATCH_IMAGE_SLOT_COUNT ||
        metadata.width != WATCH_IMAGE_WIDTH || metadata.height != WATCH_IMAGE_HEIGHT ||
        metadata.total_size == 0U || metadata.total_size > WATCH_IMAGE_MAX_FILE_SIZE) {
        publish_error(protocol, WATCH_BLE_ERROR_PARAMETER, session_id, 0U);
        return;
    }
    if (session_matches(protocol, session_id, &metadata)) {
        protocol->last_activity_us = esp_timer_get_time();
        publish(
            protocol,
            WATCH_BLE_STATE_RECEIVING,
            WATCH_BLE_ERROR_NONE,
            protocol->session_id,
            protocol->next_offset,
            protocol->metadata.total_size
        );
        return;
    }
    if (protocol->active) {
        publish_error(protocol, WATCH_BLE_ERROR_BUSY, session_id, protocol->session_id);
        return;
    }

    watch_image_writer_t *writer = NULL;
    const esp_err_t result = watch_images_begin(&metadata, &writer);
    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "START 存储初始化失败 session=%" PRIu32 ", result=%s (0x%x)",
            session_id,
            esp_err_to_name(result),
            (unsigned int)result
        );
        publish_error(protocol, map_storage_error(result), session_id, 0U);
        return;
    }
    protocol->has_committed_session = false;
    protocol->committed_session_id = 0U;
    protocol->committed_size = 0U;
    protocol->committed_slot = 0U;
    protocol->writer = writer;
    protocol->metadata = metadata;
    protocol->session_id = session_id;
    protocol->next_offset = 0U;
    protocol->last_activity_us = esp_timer_get_time();
    protocol->active = true;
    ESP_LOGI(
        TAG,
        "START session=%" PRIu32 ", slot=%u, size=%" PRIu32 ", crc=0x%08" PRIx32,
        session_id,
        (unsigned int)metadata.slot,
        metadata.total_size,
        metadata.crc32
    );
    publish(
        protocol,
        WATCH_BLE_STATE_RECEIVING,
        WATCH_BLE_ERROR_NONE,
        session_id,
        0U,
        metadata.total_size
    );
}

/**
 * @brief 处理 END 控制帧。
 *
 * @param protocol 协议上下文。
 * @param session_id 帧内会话编号。
 */
static void handle_end(watch_ble_protocol_t *protocol, uint32_t session_id)
{
    if (!protocol->active) {
        if (protocol->has_committed_session &&
            session_id == protocol->committed_session_id) {
            publish(
                protocol,
                WATCH_BLE_STATE_COMPLETE,
                WATCH_BLE_ERROR_NONE,
                protocol->committed_session_id,
                protocol->committed_size,
                protocol->committed_slot
            );
        } else {
            publish_error(protocol, WATCH_BLE_ERROR_STATE, session_id, 0U);
        }
        return;
    }
    if (session_id != protocol->session_id) {
        publish_error(protocol, WATCH_BLE_ERROR_STATE, session_id, protocol->session_id);
        return;
    }
    if (protocol->next_offset != protocol->metadata.total_size) {
        publish_error(
            protocol,
            WATCH_BLE_ERROR_LENGTH,
            session_id,
            protocol->metadata.total_size
        );
        return;
    }

    publish(
        protocol,
        WATCH_BLE_STATE_VERIFYING,
        WATCH_BLE_ERROR_NONE,
        session_id,
        protocol->next_offset,
        protocol->metadata.total_size
    );
    const uint8_t slot = protocol->metadata.slot;
    const uint32_t final_offset = protocol->next_offset;
    const esp_err_t result = watch_images_commit(protocol->writer, NULL);
    if (result != ESP_OK) {
        const watch_ble_error_t error = map_storage_error(result);
        reset_session(protocol);
        publish_error(protocol, error, session_id, 0U);
        return;
    }

    protocol->has_committed_session = true;
    protocol->committed_session_id = session_id;
    protocol->committed_size = final_offset;
    protocol->committed_slot = slot;
    protocol->writer = NULL;
    protocol->active = false;
    protocol->metadata = (watch_image_metadata_t){0};
    protocol->session_id = 0U;
    protocol->next_offset = 0U;
    protocol->last_activity_us = 0;
    ESP_LOGI(
        TAG,
        "COMPLETE session=%" PRIu32 ", slot=%u, size=%" PRIu32,
        session_id,
        (unsigned int)slot,
        final_offset
    );
    publish(
        protocol,
        WATCH_BLE_STATE_COMPLETE,
        WATCH_BLE_ERROR_NONE,
        session_id,
        final_offset,
        slot
    );
    protocol->committed(slot);
}

/**
 * @brief 处理 CANCEL 控制帧。
 *
 * @param protocol 协议上下文。
 * @param session_id 帧内会话编号。
 */
static void handle_cancel(watch_ble_protocol_t *protocol, uint32_t session_id)
{
    if (!protocol->active || session_id != protocol->session_id) {
        publish_error(protocol, WATCH_BLE_ERROR_STATE, session_id, protocol->session_id);
        return;
    }
    reset_session(protocol);
    publish(
        protocol,
        WATCH_BLE_STATE_CONNECTED,
        WATCH_BLE_ERROR_NONE,
        0U,
        0U,
        0U
    );
}

/**
 * @brief 处理 QUERY 控制帧。
 *
 * @param protocol 协议上下文。
 * @param session_id 帧内会话编号。
 */
static void handle_query(watch_ble_protocol_t *protocol, uint32_t session_id)
{
    if (!protocol->active || session_id != protocol->session_id) {
        publish_error(protocol, WATCH_BLE_ERROR_NOT_FOUND, session_id, 0U);
        return;
    }
    protocol->last_activity_us = esp_timer_get_time();
    publish(
        protocol,
        WATCH_BLE_STATE_RECEIVING,
        WATCH_BLE_ERROR_NONE,
        protocol->session_id,
        protocol->next_offset,
        protocol->metadata.total_size
    );
}

/**
 * @brief 初始化单会话协议上下文及状态发布回调。
 *
 * @param protocol 协议上下文。
 * @param publish_callback Status 发布函数。
 * @param committed_callback 图片提交完成函数。
 */
void watch_ble_protocol_init(
    watch_ble_protocol_t *protocol,
    watch_ble_status_publish_t publish_callback,
    watch_ble_image_committed_t committed_callback
)
{
    *protocol = (watch_ble_protocol_t){
        .publish = publish_callback,
        .committed = committed_callback,
    };
}

/**
 * @brief 连接后发布新连接状态或保留会话的续传状态。
 *
 * @param protocol 协议上下文。
 */
void watch_ble_protocol_on_connected(watch_ble_protocol_t *protocol)
{
    if (protocol->active) {
        publish(
            protocol,
            WATCH_BLE_STATE_RECEIVING,
            WATCH_BLE_ERROR_NONE,
            protocol->session_id,
            protocol->next_offset,
            protocol->metadata.total_size
        );
    } else {
        publish(
            protocol,
            WATCH_BLE_STATE_CONNECTED,
            WATCH_BLE_ERROR_NONE,
            0U,
            0U,
            0U
        );
    }
}

/**
 * @brief 校验并执行 START、END、CANCEL 或 QUERY 控制帧。
 *
 * @param protocol 协议上下文。
 * @param bytes 已从 GATT 队列复制的完整值。
 * @param length 值长度。
 */
void watch_ble_protocol_control(
    watch_ble_protocol_t *protocol,
    const uint8_t *bytes,
    size_t length
)
{
    if (protocol == NULL || bytes == NULL || length < WATCH_BLE_CONTROL_SIMPLE_LENGTH) {
        if (protocol != NULL) {
            publish_error(protocol, WATCH_BLE_ERROR_FRAME, 0U, (uint32_t)length);
        }
        return;
    }
    const uint32_t session_id = watch_ble_read_le32(bytes + 4U);
    if (bytes[0] != WATCH_BLE_PROTOCOL_VERSION) {
        publish_error(protocol, WATCH_BLE_ERROR_VERSION, session_id, bytes[0]);
        return;
    }
    if (session_id == 0U) {
        publish(
            protocol,
            WATCH_BLE_STATE_ERROR,
            WATCH_BLE_ERROR_PARAMETER,
            0U,
            protocol->active ? protocol->next_offset : 0U,
            0U
        );
        return;
    }

    const uint8_t opcode = bytes[1];
    const uint16_t payload_length = watch_ble_read_le16(bytes + 2U);
    if (opcode == WATCH_BLE_OPCODE_START) {
        if (length != WATCH_BLE_CONTROL_START_LENGTH || payload_length != 16U) {
            publish_error(protocol, WATCH_BLE_ERROR_FRAME, session_id, (uint32_t)length);
            return;
        }
        handle_start(protocol, bytes);
        return;
    }
    if (length != WATCH_BLE_CONTROL_SIMPLE_LENGTH || payload_length != 0U) {
        publish_error(protocol, WATCH_BLE_ERROR_FRAME, session_id, (uint32_t)length);
        return;
    }

    switch (opcode) {
        case WATCH_BLE_OPCODE_END:
            handle_end(protocol, session_id);
            break;
        case WATCH_BLE_OPCODE_CANCEL:
            handle_cancel(protocol, session_id);
            break;
        case WATCH_BLE_OPCODE_QUERY:
            handle_query(protocol, session_id);
            break;
        default:
            publish_error(protocol, WATCH_BLE_ERROR_FRAME, session_id, opcode);
            break;
    }
}

/**
 * @brief 校验会话、offset 和长度后把一个 Data 分片实际写入存储。
 *
 * @param protocol 协议上下文。
 * @param bytes 已从 GATT 队列复制的数据帧。
 * @param length 完整帧长度。
 */
void watch_ble_protocol_data(
    watch_ble_protocol_t *protocol,
    const uint8_t *bytes,
    size_t length
)
{
    if (protocol == NULL || bytes == NULL ||
        length < WATCH_BLE_DATA_HEADER_LENGTH + 1U) {
        if (protocol != NULL) {
            publish_error(protocol, WATCH_BLE_ERROR_FRAME, 0U, (uint32_t)length);
        }
        return;
    }
    const uint16_t payload_length = watch_ble_read_le16(bytes + 2U);
    const uint32_t session_id = watch_ble_read_le32(bytes + 4U);
    const uint32_t offset = watch_ble_read_le32(bytes + 8U);
    if (bytes[0] != WATCH_BLE_PROTOCOL_VERSION) {
        publish_error(protocol, WATCH_BLE_ERROR_VERSION, session_id, bytes[0]);
        return;
    }
    if (bytes[1] != 0U || payload_length == 0U ||
        payload_length > WATCH_BLE_DATA_PAYLOAD_MAX ||
        length != WATCH_BLE_DATA_HEADER_LENGTH + payload_length) {
        publish_error(protocol, WATCH_BLE_ERROR_FRAME, session_id, (uint32_t)length);
        return;
    }
    if (!protocol->active || session_id != protocol->session_id) {
        publish_error(protocol, WATCH_BLE_ERROR_STATE, session_id, protocol->session_id);
        return;
    }

    if (offset < protocol->next_offset) {
        if (payload_length <= protocol->next_offset - offset) {
            protocol->last_activity_us = esp_timer_get_time();
            publish(
                protocol,
                WATCH_BLE_STATE_RECEIVING,
                WATCH_BLE_ERROR_NONE,
                session_id,
                protocol->next_offset,
                protocol->metadata.total_size
            );
        } else {
            publish_error(protocol, WATCH_BLE_ERROR_OFFSET, session_id, protocol->next_offset);
        }
        return;
    }
    if (offset > protocol->next_offset) {
        publish_error(protocol, WATCH_BLE_ERROR_OFFSET, session_id, protocol->next_offset);
        return;
    }
    if (payload_length > protocol->metadata.total_size - protocol->next_offset) {
        publish_error(protocol, WATCH_BLE_ERROR_LENGTH, session_id, protocol->metadata.total_size);
        return;
    }

    const uint32_t previous_offset = protocol->next_offset;
    uint32_t next_offset = 0U;
    const esp_err_t result = watch_images_append(
        protocol->writer,
        offset,
        bytes + WATCH_BLE_DATA_HEADER_LENGTH,
        payload_length,
        &next_offset
    );
    if (result != ESP_OK) {
        publish_error(protocol, map_storage_error(result), session_id, protocol->next_offset);
        return;
    }
    protocol->next_offset = next_offset;
    protocol->last_activity_us = esp_timer_get_time();
    if (next_offset == protocol->metadata.total_size ||
        previous_offset / WATCH_BLE_PROGRESS_LOG_INTERVAL !=
            next_offset / WATCH_BLE_PROGRESS_LOG_INTERVAL) {
        ESP_LOGI(
            TAG,
            "进度 session=%" PRIu32 ", offset=%" PRIu32 "/%" PRIu32,
            session_id,
            next_offset,
            protocol->metadata.total_size
        );
    }
    publish(
        protocol,
        WATCH_BLE_STATE_RECEIVING,
        WATCH_BLE_ERROR_NONE,
        session_id,
        next_offset,
        protocol->metadata.total_size
    );
}

/**
 * @brief 取消五分钟没有有效活动的 RAM 写入会话。
 *
 * @param protocol 协议上下文。
 * @param now_us 当前单调时钟微秒值。
 */
void watch_ble_protocol_poll_timeout(
    watch_ble_protocol_t *protocol,
    int64_t now_us
)
{
    if (protocol == NULL || !protocol->active ||
        now_us - protocol->last_activity_us < WATCH_BLE_INACTIVITY_US) {
        return;
    }
    const uint32_t session_id = protocol->session_id;
    reset_session(protocol);
    publish_error(protocol, WATCH_BLE_ERROR_STATE, session_id, 0U);
}
