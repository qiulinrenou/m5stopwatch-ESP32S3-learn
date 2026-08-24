#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "key.h"                                    // 使用本地 key 模块管理 G1 GPIO 轮询和去抖
#include "watch_lvgl.h"                              // 使用统一LVGL
#include "watch_ui.h"
#include "watch_core.h"                              // 使用真实表盘数据协调任务
#include "watch_board.h"                                  // 使用统一屏幕和触摸配置
#include "touch_port.h"                                  // 使用 watch_board 的触摸初始化接口
#include "display_port.h"
#include "watch_rtc.h"                               // 使用 RX8130 实时时钟接口
#include "watch_images.h"                            // 使用独立图片存储和异步读取模块
#include "watch_ble.h"                               // 使用独立 NimBLE 图片协议模块

#include "board_power.h"

static const char *TAG = "M5STOPWATCH";

#define APP_EVENT_QUEUE_LENGTH 8U
#define APP_EVENT_TASK_STACK_SIZE 4096U

typedef enum {
    APP_EVENT_IMAGE_LOADED = 0,
    APP_EVENT_BLE,
    APP_EVENT_DELETE_CURRENT_IMAGE,
} app_event_type_t;

typedef struct {
    app_event_type_t type;
    union {
        watch_ui_image_result_t image;
        watch_ble_event_t ble;
    } data;
} app_event_t;

typedef struct {
    watch_ui_image_catalog_t catalog;
    uint8_t committed_slot;
    bool request_committed_slot;
} image_ui_update_t;

typedef struct {
    bool available;
    uint8_t slot;
} image_delete_request_t;

typedef struct {
    watch_ui_image_catalog_t catalog;
    uint8_t deleted_slot;
} image_delete_ui_update_t;

static QueueHandle_t s_app_event_queue;

/**
 * @brief 把存储任务的异步 JPEG 结果交给 LVGL UI 接管。
 *
 * @param display watch_lvgl 注册的 Display。
 * @param user_data 指向本次同步有效的 watch_ui_image_result_t。
 */
static void accept_image_action(lv_display_t *display, void *user_data)
{
    watch_ui_accept_image(display, (const watch_ui_image_result_t *)user_data);
}

/**
 * @brief 接收存储任务结果并无阻塞转交应用事件队列。
 *
 * @param result 异步读取结果；成功时 data 所有权转交应用事件队列。
 * @param user_data 未使用。
 */
static void image_load_finished(
    const watch_image_load_result_t *result,
    void *user_data
)
{
    (void)user_data;
    if (result == NULL) {
        return;
    }
    const app_event_t event = {
        .type = APP_EVENT_IMAGE_LOADED,
        .data.image = {
            .request_id = result->request_id,
            .slot = result->slot,
            .success = result->result == ESP_OK,
            .data = result->data,
            .size = result->size,
            .release = heap_caps_free,
        },
    };
    if (s_app_event_queue == NULL ||
        xQueueSend(s_app_event_queue, &event, 0U) != pdTRUE) {
        ESP_LOGW(TAG, "图片结果事件队列已满，丢弃请求 %lu",
                 (unsigned long)result->request_id);
        if (result->data != NULL) {
            heap_caps_free(result->data);
        }
    }
}

/**
 * @brief 响应 UI 的非阻塞槽位读取请求。
 *
 * @param request_id UI 生成的防过期编号。
 * @param slot 目标槽位。
 * @param user_data 未使用。
 * @return true 表示请求成功进入存储任务队列。
 */
static bool request_image_from_ui(
    uint32_t request_id,
    uint8_t slot,
    void *user_data
)
{
    (void)user_data;
    return watch_images_request_load(
        slot,
        request_id,
        image_load_finished,
        NULL
    ) == ESP_OK;
}

/**
 * @brief 在 LVGL 锁内注册图库回调并同步存储目录。
 *
 * @param display watch_lvgl 注册的 Display。
 * @param user_data 指向 watch_ui_image_catalog_t。
 */
static void configure_image_ui_action(lv_display_t *display, void *user_data)
{
    watch_ui_set_image_request_callback(request_image_from_ui, NULL);
    watch_ui_update_image_catalog(
        display,
        (const watch_ui_image_catalog_t *)user_data
    );
}

/**
 * @brief 在 LVGL 锁内更新图片目录，并可自动打开刚提交的槽位。
 *
 * @param display watch_lvgl 注册的 Display。
 * @param user_data 指向 image_ui_update_t。
 */
static void update_image_ui_action(lv_display_t *display, void *user_data)
{
    const image_ui_update_t *update = (const image_ui_update_t *)user_data;
    watch_ui_update_image_catalog(display, &update->catalog);
    if (update->request_committed_slot) {
        watch_ui_request_image(display, update->committed_slot, true);
    }
}

/**
 * @brief 在 LVGL 锁内检查并锁定当前可删除的图库槽位。
 *
 * @param display watch_lvgl 注册的 Display。
 * @param user_data 指向 image_delete_request_t，接收检查结果和槽位。
 */
static void begin_image_delete_action(lv_display_t *display, void *user_data)
{
    image_delete_request_t *request = (image_delete_request_t *)user_data;
    request->available = watch_ui_get_deletable_image_slot(display, &request->slot);
}

/**
 * @brief 在 LVGL 锁内取消一次未成功提交的图片删除。
 *
 * @param display watch_lvgl 注册的 Display。
 * @param user_data 指向删除失败的 uint8_t 槽位。
 */
static void cancel_image_delete_action(lv_display_t *display, void *user_data)
{
    watch_ui_cancel_image_delete(display, *(const uint8_t *)user_data);
}

/**
 * @brief 在 LVGL 锁内应用已经成功提交的图片删除结果。
 *
 * @param display watch_lvgl 注册的 Display。
 * @param user_data 指向 image_delete_ui_update_t。
 */
static void apply_image_delete_action(lv_display_t *display, void *user_data)
{
    const image_delete_ui_update_t *update = (const image_delete_ui_update_t *)user_data;
    watch_ui_apply_image_deleted(display, update->deleted_slot, &update->catalog);
}

/**
 * @brief 在应用事件任务中完成当前图库槽位的存储删除和 UI 同步。
 *
 * @note LVGL 状态检查与更新分别通过 watch_lvgl_run() 执行，文件系统删除位于锁外。
 */
static void process_current_image_delete(void)
{
    image_delete_request_t request = {
        .available = false,
        .slot = 0xFFU,
    };
    const esp_err_t begin_result = watch_lvgl_run(begin_image_delete_action, &request);
    if (begin_result != ESP_OK || !request.available) {
        ESP_LOGI(TAG, "G2 长按删除已忽略：当前不是可删除的已加载图库图片");
        return;
    }

    watch_image_catalog_t stored_catalog;
    const esp_err_t delete_result = watch_images_delete(request.slot, &stored_catalog);
    if (delete_result != ESP_OK) {
        const esp_err_t cancel_result = watch_lvgl_run(
            cancel_image_delete_action,
            &request.slot
        );
        ESP_LOGW(TAG, "删除图片槽位 %u 失败: %s，图库解锁=%s",
                 (unsigned int)request.slot,
                 esp_err_to_name(delete_result),
                 esp_err_to_name(cancel_result));
        return;
    }

    const image_delete_ui_update_t update = {
        .catalog = {
            .occupied_mask = stored_catalog.occupied_mask,
            .latest_slot = stored_catalog.latest_slot,
        },
        .deleted_slot = request.slot,
    };
    const esp_err_t ui_result = watch_lvgl_run(apply_image_delete_action, (void *)&update);
    if (ui_result != ESP_OK) {
        ESP_LOGW(TAG, "槽位 %u 已删除，但图库同步失败: %s",
                 (unsigned int)request.slot, esp_err_to_name(ui_result));
    }
}

/**
 * @brief 在 LVGL 锁内显示 BLE 动态口令。
 *
 * @param display watch_lvgl 注册的 Display。
 * @param user_data 指向 uint32_t 六位口令。
 */
static void show_pairing_action(lv_display_t *display, void *user_data)
{
    watch_ui_show_pairing_code(display, *(const uint32_t *)user_data);
}

/**
 * @brief 在 LVGL 锁内隐藏 BLE 动态口令。
 *
 * @param display watch_lvgl 注册的 Display。
 * @param user_data 未使用。
 */
static void hide_pairing_action(lv_display_t *display, void *user_data)
{
    (void)user_data;
    watch_ui_hide_pairing_code(display);
}

/**
 * @brief 在独立应用任务中处理一项 BLE UI 事件。
 *
 * @param event 已从固定队列复制出的 BLE 事件。
 * @note 文件系统访问和 LVGL 同步均不在 BLE 工作任务中执行。
 */
static void process_ble_app_event(const watch_ble_event_t *event)
{
    if (event->type == WATCH_BLE_EVENT_SHOW_PAIRING_CODE) {
        uint32_t passkey = event->data.passkey;
        watch_lvgl_run(show_pairing_action, &passkey);
        return;
    }
    if (event->type == WATCH_BLE_EVENT_HIDE_PAIRING_CODE) {
        watch_lvgl_run(hide_pairing_action, NULL);
        return;
    }
    if (event->type == WATCH_BLE_EVENT_IMAGE_COMMITTED) {
        watch_image_catalog_t stored_catalog;
        if (watch_images_get_catalog(&stored_catalog) != ESP_OK) {
            return;
        }
        const image_ui_update_t update = {
            .catalog = {
                .occupied_mask = stored_catalog.occupied_mask,
                .latest_slot = stored_catalog.latest_slot,
            },
            .committed_slot = event->data.slot,
            .request_committed_slot = true,
        };
        watch_lvgl_run(update_image_ui_action, (void *)&update);
    }
}

/**
 * @brief 串行把存储与 BLE 应用事件交给 LVGL 线程安全入口。
 *
 * @param argument 未使用。
 */
static void app_event_task(void *argument)
{
    (void)argument;
    app_event_t event;
    while (true) {
        if (xQueueReceive(s_app_event_queue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (event.type == APP_EVENT_IMAGE_LOADED) {
            const esp_err_t result = watch_lvgl_run(
                accept_image_action,
                &event.data.image
            );
            if (result != ESP_OK && event.data.image.data != NULL) {
                heap_caps_free(event.data.image.data);
            }
        } else if (event.type == APP_EVENT_BLE) {
            process_ble_app_event(&event.data.ble);
        } else if (event.type == APP_EVENT_DELETE_CURRENT_IMAGE) {
            process_current_image_delete();
        }
    }
}

/**
 * @brief 创建新增功能共用的非阻塞应用事件桥。
 *
 * @return ESP_OK 表示队列和 Core 0 工作任务均已创建。
 */
static esp_err_t start_app_event_bridge(void)
{
    s_app_event_queue = xQueueCreate(APP_EVENT_QUEUE_LENGTH, sizeof(app_event_t));
    if (s_app_event_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCore(
            app_event_task,
            "watch_app_events",
            APP_EVENT_TASK_STACK_SIZE,
            NULL,
            3,
            NULL,
            0
        ) != pdPASS) {
        vQueueDelete(s_app_event_queue);
        s_app_event_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/**
 * @brief 接收 BLE 工作任务发布的配对与图片完成事件。
 *
 * @param event BLE 应用事件。
 * @param user_data 未使用。
 * @note 本回调只复制固定大小事件且不阻塞；文件系统和 LVGL 均由应用事件任务处理。
 */
static void ble_event_callback(
    const watch_ble_event_t *event,
    void *user_data
)
{
    (void)user_data;
    if (event == NULL) {
        return;
    }
    const app_event_t app_event = {
        .type = APP_EVENT_BLE,
        .data.ble = *event,
    };
    if (s_app_event_queue == NULL ||
        xQueueSend(s_app_event_queue, &app_event, 0U) != pdTRUE) {
        ESP_LOGW(TAG, "BLE 应用事件队列已满，事件=%u", (unsigned int)event->type);
    }
}

/**
 * @brief 将 G1 单击动作转发到 watch_ui 的 LVGL 页面切换函数。
 *
 * @param display watch_lvgl 注册的 LVGL Display。
 * @param user_data 未使用，保持 watch_lvgl_action_t 签名。
 * @return 无返回值；函数运行时已经持有 watch_lvgl 的 LVGL 互斥锁。
 */
static void toggle_menu_action(lv_display_t *display, void *user_data)
{
    (void)user_data;
    ESP_LOGI(TAG, "G1 菜单切换请求已进入 LVGL 锁");
    watch_ui_toggle_menu(display);
}

/**
 * @brief 将 key 已确认的 G1 单击事件转交给 LVGL 锁。
 *
 * @param user_data 未使用，保持 key_click_cb_t 回调签名。
 * @return 无返回值。
 * @note GPIO 轮询和去抖由 key 管理；本函数只通过 watch_lvgl_run() 修改 UI。
 */
static void g1_button_click_action(void *user_data)
{
    (void)user_data;

    const esp_err_t result = watch_lvgl_run(toggle_menu_action, NULL);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "G1 菜单切换失败: %s", esp_err_to_name(result));
    } else {
        ESP_LOGI(TAG, "G1 菜单切换请求执行完成");
    }
}

/**
 * @brief 将 G2 单击动作转发到图库切换函数。
 *
 * @param display watch_lvgl 注册的 LVGL Display。
 * @param user_data 未使用。
 */
static void toggle_gallery_action(lv_display_t *display, void *user_data)
{
    (void)user_data;
    watch_ui_toggle_gallery(display);
}

/**
 * @brief 将 key 已确认的 G2 单击事件交给 LVGL 锁。
 *
 * @param user_data 未使用。
 */
static void g2_button_click_action(void *user_data)
{
    (void)user_data;
    const esp_err_t result = watch_lvgl_run(toggle_gallery_action, NULL);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "G2 图库切换失败: %s", esp_err_to_name(result));
    }
}

/**
 * @brief 将 key 已确认的 G2 长按转换为应用层图片删除事件。
 *
 * @param user_data 未使用。
 * @note 本回调运行在按键任务中，只执行无等待队列投递，不访问 LVGL 或文件系统。
 */
static void g2_button_long_press_action(void *user_data)
{
    (void)user_data;
    const app_event_t event = {
        .type = APP_EVENT_DELETE_CURRENT_IMAGE,
    };
    if (s_app_event_queue == NULL ||
        xQueueSend(s_app_event_queue, &event, 0U) != pdTRUE) {
        ESP_LOGW(TAG, "G2 长按删除事件队列已满，已忽略本次请求");
    }
}

static esp_lcd_touch_handle_t init_touch(void)
{
    i2c_master_bus_handle_t i2c_bus =                                                             // 获取共享 I2C 总线
    board_power_get_i2c_bus();                                                                   // board_power_init() 配套使用
    assert(i2c_bus != NULL);                                                                    //检查共享 I2C 是否有效

    // CST820、M5PM1 和 M5IOE1 必须共用这一条 I2C 总线，不能重复安装驱动。
    ESP_ERROR_CHECK(                                                                            // 保持原来的触摸复位时序
        board_power_reset_touch()                                                               // 由 M5IOE1 控制 CST820 复位
    );

    esp_lcd_touch_handle_t touch_handle = NULL;
    ESP_ERROR_CHECK(touch_port_init(i2c_bus, &touch_handle));
    ESP_LOGI(TAG, "CST820 触摸初始化完成，中断 GPIO=%d", WATCH_TOUCH_PIN_INT);
    return touch_handle;
}

/**
 * @brief 按板级、RTC、显示触摸、LVGL、图片、BLE、按键和 core 顺序装配应用。
 */
void app_main(void)
{
    ESP_LOGI(TAG, "开始初始化 M5StopWatch: M5PM1 + M5IOE1 + CO5300 + CST820 + LVGL8");

    // 电源与 IO 扩展必须先于显示和触摸初始化，否则屏幕供电及复位状态不确定。
    ESP_ERROR_CHECK(
        board_power_init());
    ESP_LOGI(TAG, "板级管理芯片状态: M5PM1=%s, M5IOE1=%s",
             board_power_pmic_ready() ? "正常" : "不可用",
             board_power_ioe_ready() ? "正常" : "不可用");

    i2c_master_bus_handle_t i2c_bus =                 // 获取板级模块已经创建的共享 I2C 总线
        board_power_get_i2c_bus();
    assert(i2c_bus != NULL);                          // 后续触摸和 RTC 都依赖这条总线

    esp_err_t rtc_init_result =
        watch_rtc_init(i2c_bus);                      // 探测 RX8130 并检查时间是否可信
    if (rtc_init_result == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "RX8130 没有合法时间，尝试使用本次固件编译时间恢复");
        rtc_init_result = watch_rtc_restore_from_build_time(
            8 * 60                                    // 当前编译电脑使用中国/香港标准时间 UTC+8
        );
    }
    if (rtc_init_result != ESP_OK) {                  // RTC 无效不能阻止手表显示和其他功能启动
        ESP_LOGW(TAG, "RX8130 当前不可用，表盘将显示大号 RTC 提示: %s",
                 esp_err_to_name(rtc_init_result));
    }

    esp_lcd_panel_io_handle_t panel_io = NULL;           // 接收显示模块创建的 Panel IO 句柄
    esp_lcd_panel_handle_t panel_handle = NULL;           // 接收显示模块创建的 CO5300 面板句柄

    ESP_ERROR_CHECK(                                     // 检查板级显示复位是否成功
    board_power_reset_display() );                       // 先由 M5IOE1 完成供电和硬件复位

    ESP_ERROR_CHECK(                                     // 检查整个 CO5300 初始化过程
    display_port_init(                                  // 初始化 SPI、Panel IO 和 CO5300 面板
        &panel_io,                        // 输出给 LVGL adapter 的 Panel IO
        &panel_handle                        // 输出给 LVGL adapter 的面板句柄
    ) );

    esp_lcd_touch_handle_t touch_handle =
        init_touch();                                   // 创建 CST820 硬件触摸句柄

    watch_lvgl_config_t lvgl_config = {
        .panel_io = panel_io,                           // 传入 display_port 创建的 Panel IO
        .panel = panel_handle,                          // 传入 display_port 创建的 CO5300 面板
        .touch = touch_handle,                          // 传入 touch_port 创建的 CST820
        .horizontal_resolution = WATCH_LCD_H_RES,       // 设置 466 像素横向分辨率
        .vertical_resolution = WATCH_LCD_V_RES,         // 设置 466 像素纵向分辨率
        .horizontal_alignment = 2,                      // CO5300 横向刷新区域按 2 像素对齐
        .vertical_alignment = 2,                        // CO5300 纵向刷新区域按 2 像素对齐
        .rotation = WATCH_LVGL_ROTATION_0,               // 保持当前不旋转配置
        .use_psram = true,                              // 使用当前设备的 PSRAM 双缓冲
    };

    ESP_ERROR_CHECK(
        watch_lvgl_start(
            &lvgl_config,                               // 传入硬件和显示配置
            ui_init                                     // 在运行时内部的锁内创建 UI
        )
    );

    const esp_err_t event_bridge_result = start_app_event_bridge();
    if (event_bridge_result != ESP_OK) {
        ESP_LOGW(TAG, "图片存储与 BLE 图片功能已禁用，事件桥创建失败: %s",
                 esp_err_to_name(event_bridge_result));
    } else {
        const esp_err_t images_result = watch_images_init();
        if (images_result == ESP_OK) {
            watch_image_catalog_t stored_catalog;
            if (watch_images_get_catalog(&stored_catalog) == ESP_OK) {
                const watch_ui_image_catalog_t ui_catalog = {
                    .occupied_mask = stored_catalog.occupied_mask,
                    .latest_slot = stored_catalog.latest_slot,
                };
                const esp_err_t ui_result = watch_lvgl_run(
                    configure_image_ui_action,
                    (void *)&ui_catalog
                );
                if (ui_result != ESP_OK) {
                    ESP_LOGW(TAG, "图库 UI 装配失败: %s", esp_err_to_name(ui_result));
                }
            }

            const watch_ble_config_t ble_config = {
                .event_callback = ble_event_callback,
                .user_data = NULL,
            };
            const esp_err_t ble_result = watch_ble_start(&ble_config);
            if (ble_result != ESP_OK) {
                ESP_LOGW(TAG, "BLE 图片功能已禁用: %s", esp_err_to_name(ble_result));
            }
        } else {
            ESP_LOGW(TAG, "图片存储与 BLE 图片功能已禁用: %s",
                     esp_err_to_name(images_result));
        }
    }

    const key_config_t key_config = {
        .g1_on_click = g1_button_click_action,         // 仅 G1 保持已验证的主表盘/菜单切换行为
        .g1_user_data = NULL,                           // G1 菜单切换不需要额外应用层上下文
        .g2_on_click = g2_button_click_action,          // G2 打开最近图片或从图库返回主表盘
        .g2_user_data = NULL,                           // G2 图库切换不需要额外上下文
        .g2_on_long_press = g2_button_long_press_action, // G2 长按删除当前全屏图库图片
        .g2_long_press_user_data = NULL,                // G2 长按删除不需要额外上下文
    };
    ESP_ERROR_CHECK(key_init(&key_config));             // 由 watch_board 的 key 模块统一启动 G1、G2

    const watch_core_config_t core_config = {
        .rtc_reader = watch_rtc_get_datetime,         // 注入 RX8130 读取回调，避免 core 依赖硬件细节
        .battery_reader = board_power_get_battery_percent, // 注入线程安全的滤波电量读取接口
        .update_period_ms = 1000,                     // 每秒更新时间、电量和冒号闪烁状态
        .timezone_offset_minutes = 8 * 60,            // RX8130 保存 UTC，香港/中国标准时间为 UTC+8
    };

    ESP_ERROR_CHECK(
        watch_core_start(&core_config)                // LVGL 完全启动后再发布真实数据
    );
}

