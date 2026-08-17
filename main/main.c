#include <assert.h>
#include <stdio.h>

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"

#include "watch_button.h"                           // 使用独立按键模块管理 GPIO 轮询和去抖
#include "watch_lvgl.h"                              // 使用统一LVGL
#include "watch_ui.h"
#include "watch_core.h"                              // 使用真实表盘数据协调任务
#include "watch_board.h"                                  // 使用统一屏幕和触摸配置
#include "touch_port.h"                                  // 使用 watch_board 的触摸初始化接口
#include "display_port.h"
#include "watch_rtc.h"                               // 使用 RX8130 实时时钟接口

#include "board_power.h"

static const char *TAG = "M5STOPWATCH";

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
 * @brief 将 watch_button 已确认的 G1 单击事件转交给 LVGL 锁。
 *
 * @param user_data 未使用，保持 watch_button_click_cb_t 回调签名。
 * @return 无返回值。
 * @note GPIO 轮询和去抖由 watch_button 管理；本函数只通过 watch_lvgl_run() 修改 UI。
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

    const watch_button_config_t g1_button_config = {
        .gpio_num = WATCH_G1_BUTTON_PIN,             // 使用厂商 UserDemo 定义的 B 键 GPIO1
        .poll_period_ms = WATCH_BUTTON_POLL_MS,       // 保持原 10 ms 轮询周期
        .debounce_period_ms = WATCH_BUTTON_DEBOUNCE_MS, // 保持原 40 ms 稳定去抖
        .on_click = g1_button_click_action,           // 单击后才转交给 LVGL 菜单切换路径
        .user_data = NULL,                            // 当前菜单切换不需要额外应用层上下文
    };
    ESP_ERROR_CHECK(watch_button_start(&g1_button_config));

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

