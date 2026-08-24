#include "watch_ble.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "watch_ble_internal.h"

#define WATCH_BLE_WORK_QUEUE_LENGTH 8U
#define WATCH_BLE_WORK_TASK_STACK 6144U

static const char *TAG = "watch_ble";
static const ble_uuid128_t s_advertised_service_uuid = BLE_UUID128_INIT(
    0x53, 0x46, 0x42, 0x41, 0x44, 0x47, 0x45, 0x01,
    0x9C, 0x21, 0x4D, 0x5A, 0x6F, 0x3B, 0x7A, 0x1E
);

static QueueHandle_t s_work_queue;
static TaskHandle_t s_work_task_handle;
static watch_ble_config_t s_config;
static watch_ble_protocol_t s_protocol;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static watch_ble_status_t s_status = {
    .version = WATCH_BLE_PROTOCOL_VERSION,
    .state = WATCH_BLE_STATE_ADVERTISING,
};
static uint16_t s_connection_handle = BLE_HS_CONN_HANDLE_NONE;
static bool s_status_notify_enabled;
static uint8_t s_own_address_type;
static bool s_started;

/**
 * @brief 使用实际 GAP 回调启动可连接广播。
 */
static void start_advertising_with_callback(void);

/**
 * @brief 把 16 位状态字段按小端写入线格式。
 *
 * @param destination 输出地址。
 * @param value 待写值。
 */
static void watch_ble_write_le16(uint8_t *destination, uint16_t value)
{
    destination[0] = (uint8_t)(value & 0xFFU);
    destination[1] = (uint8_t)(value >> 8U);
}

/**
 * @brief 把 32 位状态字段按小端写入线格式。
 *
 * @param destination 输出地址。
 * @param value 待写值。
 */
static void watch_ble_write_le32(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)(value & 0xFFU);
    destination[1] = (uint8_t)((value >> 8U) & 0xFFU);
    destination[2] = (uint8_t)((value >> 16U) & 0xFFU);
    destination[3] = (uint8_t)(value >> 24U);
}

/**
 * @brief 把状态结构编码为固定 16 字节协议格式。
 *
 * @param status 输入状态。
 * @param[out] bytes 输出字节。
 */
static void encode_status(
    const watch_ble_status_t *status,
    uint8_t bytes[WATCH_BLE_STATUS_LENGTH]
)
{
    bytes[0] = status->version;
    bytes[1] = status->state;
    watch_ble_write_le16(bytes + 2U, status->error);
    watch_ble_write_le32(bytes + 4U, status->session_id);
    watch_ble_write_le32(bytes + 8U, status->next_offset);
    watch_ble_write_le32(bytes + 12U, status->detail);
}

/**
 * @brief 在短临界区内复制并编码当前 Status 快照。
 *
 * @param[out] bytes 接收固定 16 字节小端线格式。
 */
void watch_ble_internal_copy_status(uint8_t bytes[WATCH_BLE_STATUS_LENGTH])
{
    watch_ble_status_t snapshot;
    taskENTER_CRITICAL(&s_state_lock);
    snapshot = s_status;
    taskEXIT_CRITICAL(&s_state_lock);
    encode_status(&snapshot, bytes);
}

/**
 * @brief 从 NimBLE Host 回调无等待复制一个固定工作项。
 *
 * @param item 已完成长度检查的工作项。
 * @return true 表示已入队，false 表示队列未就绪或已满。
 */
bool watch_ble_internal_enqueue(const watch_ble_work_item_t *item)
{
    return item != NULL && s_work_queue != NULL &&
           xQueueSend(s_work_queue, item, 0U) == pdTRUE;
}

/**
 * @brief 更新 Status 快照，并在已订阅连接上发送通知。
 *
 * @param status 新状态。
 */
static void publish_status(const watch_ble_status_t *status)
{
    uint16_t connection_handle;
    bool should_notify;
    taskENTER_CRITICAL(&s_state_lock);
    s_status = *status;
    connection_handle = s_connection_handle;
    should_notify = s_status_notify_enabled &&
                    connection_handle != BLE_HS_CONN_HANDLE_NONE;
    taskEXIT_CRITICAL(&s_state_lock);

    if (!should_notify) {
        return;
    }
    uint8_t bytes[WATCH_BLE_STATUS_LENGTH];
    encode_status(status, bytes);
    struct os_mbuf *packet = ble_hs_mbuf_from_flat(bytes, sizeof(bytes));
    if (packet == NULL) {
        ESP_LOGW(TAG, "Status notify mbuf 不足");
        return;
    }
    const int result = ble_gatts_notify_custom(
        connection_handle,
        watch_ble_gatt_status_handle(),
        packet
    );
    if (result != 0) {
        ESP_LOGD(TAG, "Status notify 未发送: %d", result);
    }
}

/**
 * @brief 从协议工作任务发布图片提交事件。
 *
 * @param slot 已提交槽位。
 */
static void publish_image_committed(uint8_t slot)
{
    if (s_config.event_callback == NULL) {
        return;
    }
    const watch_ble_event_t event = {
        .type = WATCH_BLE_EVENT_IMAGE_COMMITTED,
        .data.slot = slot,
    };
    s_config.event_callback(&event, s_config.user_data);
}

/**
 * @brief 从 BLE 工作任务发布配对码显示或隐藏事件。
 *
 * @param show true 显示动态口令，false 隐藏。
 * @param passkey 六位口令，仅 show 时有效。
 */
static void publish_pairing_event(bool show, uint32_t passkey)
{
    if (s_config.event_callback == NULL) {
        return;
    }
    const watch_ble_event_t event = {
        .type = show
            ? WATCH_BLE_EVENT_SHOW_PAIRING_CODE
            : WATCH_BLE_EVENT_HIDE_PAIRING_CODE,
        .data.passkey = passkey,
    };
    s_config.event_callback(&event, s_config.user_data);
}

/**
 * @brief 检查队列中的 GATT 帧是否仍属于当前连接。
 *
 * @param connection_handle 帧携带的连接句柄。
 * @return true 表示可继续处理。
 */
static bool connection_is_current(uint16_t connection_handle)
{
    bool matches;
    taskENTER_CRITICAL(&s_state_lock);
    matches = connection_handle == s_connection_handle &&
              s_connection_handle != BLE_HS_CONN_HANDLE_NONE;
    taskEXIT_CRITICAL(&s_state_lock);
    return matches;
}

/**
 * @brief 串行执行控制帧、数据落盘、超时与应用事件。
 *
 * @param argument 未使用。
 */
static void ble_work_task(void *argument)
{
    (void)argument;
    watch_ble_work_item_t item;
    while (true) {
        if (xQueueReceive(s_work_queue, &item, pdMS_TO_TICKS(1000U)) == pdTRUE) {
            switch (item.type) {
                case WATCH_BLE_WORK_CONTROL:
                    if (connection_is_current(item.conn_handle)) {
                        watch_ble_protocol_control(&s_protocol, item.bytes, item.length);
                    }
                    break;
                case WATCH_BLE_WORK_DATA:
                    if (connection_is_current(item.conn_handle)) {
                        watch_ble_protocol_data(&s_protocol, item.bytes, item.length);
                    }
                    break;
                case WATCH_BLE_WORK_CONNECTED:
                    if (connection_is_current(item.conn_handle)) {
                        watch_ble_protocol_on_connected(&s_protocol);
                    }
                    break;
                case WATCH_BLE_WORK_DISCONNECTED:
                    break;
                case WATCH_BLE_WORK_SHOW_PASSKEY:
                    if (connection_is_current(item.conn_handle)) {
                        publish_pairing_event(true, item.value);
                    }
                    break;
                case WATCH_BLE_WORK_HIDE_PASSKEY:
                    if (item.conn_handle == BLE_HS_CONN_HANDLE_NONE ||
                        connection_is_current(item.conn_handle)) {
                        publish_pairing_event(false, 0U);
                    }
                    break;
                default:
                    break;
            }
        }
        watch_ble_protocol_poll_timeout(&s_protocol, esp_timer_get_time());
    }
}

/**
 * @brief 记录 NimBLE Host 重置原因。
 *
 * @param reason Host 重置码。
 */
static void host_on_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE Host reset: %d", reason);
}

/**
 * @brief 供广播与连接共用的 GAP 事件回调。
 *
 * @param event GAP 事件。
 * @param argument 未使用。
 * @return 0 表示事件已处理，重复配对按 NimBLE 约定返回特殊值。
 */
static int gap_event(struct ble_gap_event *event, void *argument)
{
    (void)argument;
    watch_ble_work_item_t item = {0};

    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status != 0) {
                start_advertising_with_callback();
                return 0;
            }
            taskENTER_CRITICAL(&s_state_lock);
            s_connection_handle = event->connect.conn_handle;
            s_status_notify_enabled = false;
            taskEXIT_CRITICAL(&s_state_lock);
            item.type = WATCH_BLE_WORK_CONNECTED;
            item.conn_handle = event->connect.conn_handle;
            watch_ble_internal_enqueue(&item);
            return 0;

        case BLE_GAP_EVENT_DISCONNECT:
            taskENTER_CRITICAL(&s_state_lock);
            s_connection_handle = BLE_HS_CONN_HANDLE_NONE;
            s_status_notify_enabled = false;
            taskEXIT_CRITICAL(&s_state_lock);
            if (s_work_queue != NULL) {
                xQueueReset(s_work_queue);
            }
            item.type = WATCH_BLE_WORK_DISCONNECTED;
            watch_ble_internal_enqueue(&item);
            item.type = WATCH_BLE_WORK_HIDE_PASSKEY;
            item.conn_handle = BLE_HS_CONN_HANDLE_NONE;
            watch_ble_internal_enqueue(&item);
            start_advertising_with_callback();
            return 0;

        case BLE_GAP_EVENT_ADV_COMPLETE:
            start_advertising_with_callback();
            return 0;

        case BLE_GAP_EVENT_SUBSCRIBE:
            if (event->subscribe.attr_handle == watch_ble_gatt_status_handle()) {
                taskENTER_CRITICAL(&s_state_lock);
                s_status_notify_enabled = event->subscribe.cur_notify != 0U;
                taskEXIT_CRITICAL(&s_state_lock);
            }
            return 0;

        case BLE_GAP_EVENT_MTU:
            ESP_LOGI(TAG, "ATT MTU=%u", (unsigned int)event->mtu.value);
            return 0;

        default:
            return 0;
    }
}

/**
 * @brief 用实际 GAP 回调启动广播，替换同步阶段的空回调占位。
 */
static void start_advertising_with_callback(void)
{
    struct ble_hs_adv_fields advertising = {0};
    advertising.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    advertising.uuids128 = (ble_uuid128_t *)&s_advertised_service_uuid;
    advertising.num_uuids128 = 1U;
    advertising.uuids128_is_complete = 1U;
    if (ble_gap_adv_set_fields(&advertising) != 0) {
        return;
    }
    struct ble_hs_adv_fields response = {0};
    response.name = (uint8_t *)WATCH_BLE_DEVICE_NAME;
    response.name_len = strlen(WATCH_BLE_DEVICE_NAME);
    response.name_is_complete = 1U;
    if (ble_gap_adv_rsp_set_fields(&response) != 0) {
        return;
    }
    const struct ble_gap_adv_params parameters = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };
    const int result = ble_gap_adv_start(
        s_own_address_type,
        NULL,
        BLE_HS_FOREVER,
        &parameters,
        gap_event,
        NULL
    );
    if (result != 0) {
        ESP_LOGE(TAG, "BLE 广播启动失败: %d", result);
    }
}

/**
 * @brief NimBLE Host 同步后的最终入口。
 */
static void host_on_sync_with_callback(void)
{
    int result = ble_hs_util_ensure_addr(0);
    if (result == 0) {
        result = ble_hs_id_infer_auto(0, &s_own_address_type);
    }
    if (result != 0) {
        ESP_LOGE(TAG, "NimBLE 地址初始化失败: %d", result);
        return;
    }
    start_advertising_with_callback();
}

/**
 * @brief 运行 NimBLE Host 主循环。
 *
 * @param argument 未使用。
 */
static void nimble_host_task(void *argument)
{
    (void)argument;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/**
 * @brief 初始化 NVS，并只在 NVS 页版本不可恢复时执行标准擦除重建。
 *
 * @return ESP_OK 表示 BLE bond 存储可用。
 */
static esp_err_t initialize_nvs(void)
{
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        result = nvs_flash_erase();
        if (result == ESP_OK) {
            result = nvs_flash_init();
        }
    }
    return result;
}

/**
 * @brief 初始化 NVS、NimBLE、固定队列、GATT 与 Host 任务并开始广播。
 *
 * @param config 非阻塞应用事件回调配置，可为 NULL。
 * @return ESP_OK 表示 BLE 生命周期已经启动；失败由 main 降级处理。
 */
esp_err_t watch_ble_start(const watch_ble_config_t *config)
{
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    s_config = config != NULL ? *config : (watch_ble_config_t){0};

    esp_err_t result = initialize_nvs();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "NVS 初始化失败: %s", esp_err_to_name(result));
        return result;
    }
    result = nimble_port_init();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE 初始化失败: %s", esp_err_to_name(result));
        return result;
    }

    s_work_queue = xQueueCreate(WATCH_BLE_WORK_QUEUE_LENGTH, sizeof(watch_ble_work_item_t));
    if (s_work_queue == NULL) {
        nimble_port_deinit();
        return ESP_ERR_NO_MEM;
    }
    watch_ble_protocol_init(&s_protocol, publish_status, publish_image_committed);
    if (xTaskCreatePinnedToCore(
            ble_work_task,
            "watch_ble_work",
            WATCH_BLE_WORK_TASK_STACK,
            NULL,
            4,
            &s_work_task_handle,
            0
        ) != pdPASS) {
        vQueueDelete(s_work_queue);
        s_work_queue = NULL;
        nimble_port_deinit();
        return ESP_ERR_NO_MEM;
    }

    ble_hs_cfg.reset_cb = host_on_reset;
    ble_hs_cfg.sync_cb = host_on_sync_with_callback;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    const int gatt_result = watch_ble_gatt_init();
    if (gatt_result != 0 || ble_svc_gap_device_name_set(WATCH_BLE_DEVICE_NAME) != 0) {
        vTaskDelete(s_work_task_handle);
        s_work_task_handle = NULL;
        vQueueDelete(s_work_queue);
        s_work_queue = NULL;
        nimble_port_deinit();
        ESP_LOGE(TAG, "BLE GATT 注册失败: %d", gatt_result);
        return ESP_FAIL;
    }

    nimble_port_freertos_init(nimble_host_task);
    s_started = true;
    ESP_LOGI(TAG, "%s BLE 图片服务已启动", WATCH_BLE_DEVICE_NAME);
    return ESP_OK;
}
