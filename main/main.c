#include <assert.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"

#include "esp_lv_adapter.h"
#include "lvgl.h"
#include "watch_ui.h"

#include "watch_board.h"                                  // 使用统一屏幕和触摸配置
#include "touch_port.h"                                  // 使用 watch_board 的触摸初始化接口
#include "display_port.h"

#include "board_power.h"

static const char *TAG = "M5STOPWATCH";

/**
 * @brief CO5300 要求刷新区域的起点为偶数、终点为奇数。
 */
static void co5300_area_rounder_cb(lv_area_t *area, void *user_data)
{
    (void)user_data;
    area->x1 = (area->x1 >> 1) << 1;
    area->y1 = (area->y1 >> 1) << 1;
    area->x2 = ((area->x2 >> 1) << 1) + 1;
    area->y2 = ((area->y2 >> 1) << 1) + 1;
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

static void init_lvgl(esp_lcd_panel_io_handle_t panel_io,
                      esp_lcd_panel_handle_t panel_handle,
                      esp_lcd_touch_handle_t touch_handle)
{
    esp_lv_adapter_config_t adapter_config = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_config.stack_in_psram = true;
    ESP_ERROR_CHECK(esp_lv_adapter_init(&adapter_config));

    esp_lv_adapter_display_config_t display_config =
        ESP_LV_ADAPTER_DISPLAY_SPI_WITH_PSRAM_DEFAULT_CONFIG(
            panel_handle,
            panel_io,
            WATCH_LCD_H_RES,
            WATCH_LCD_V_RES,
            ESP_LV_ADAPTER_ROTATE_0);
    lv_display_t *display = esp_lv_adapter_register_display(&display_config);
    assert(display != NULL);

   ESP_ERROR_CHECK(esp_lv_adapter_lock(-1));                    // 获取 LVGL 互斥锁，-1 表示无限等待

lv_obj_t *default_screen = lv_disp_get_scr_act(display);     // 在锁内获取当前显示的活动屏幕
if (default_screen != NULL) {                                // 确认活动屏幕对象有效
    lv_obj_remove_style_all(default_screen);                 // 删除活动屏幕的原有样式
    lv_obj_set_style_bg_color(                               // 设置活动屏幕背景颜色
        default_screen,
        lv_color_black(),
        LV_STATE_DEFAULT
    );
    lv_obj_set_style_bg_opa(                                 // 设置活动屏幕背景完全不透明
        default_screen,
        LV_OPA_COVER,
        LV_STATE_DEFAULT
    );
    lv_obj_set_size(                                         // 设置活动屏幕覆盖整个显示区域
        default_screen,
        LV_PCT(100),
        LV_PCT(100)
    );
}

esp_lv_adapter_unlock();                                     // 完成 LVGL 对象操作后释放互斥锁

    ESP_ERROR_CHECK(esp_lv_adapter_set_area_rounder_cb(
        display, co5300_area_rounder_cb, NULL));

    esp_lv_adapter_touch_config_t touch_config =
        ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(display, touch_handle);
    lv_indev_t *input_device = esp_lv_adapter_register_touch(&touch_config);
    assert(input_device != NULL);

    ESP_ERROR_CHECK(esp_lv_adapter_start());
    ESP_LOGI(TAG, "LVGL 适配器启动成功");

    // 保留目标工程原有界面；PMIC 和 IOE1 状态只通过串口日志输出。
    if (esp_lv_adapter_lock(portMAX_DELAY) == ESP_OK) {
        ui_init(display);
        esp_lv_adapter_unlock();
    }
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

    esp_lcd_panel_io_handle_t panel_io = NULL;           // 接收显示模块创建的 Panel IO 句柄
    esp_lcd_panel_handle_t panel_handle = NULL;           // 接收显示模块创建的 CO5300 面板句柄

    
    ESP_ERROR_CHECK(                                     // 检查板级显示复位是否成功
    board_power_reset_display() );                       // 先由 M5IOE1 完成供电和硬件复位
                      
    ESP_ERROR_CHECK(                                     // 检查整个 CO5300 初始化过程
    display_port_init(                                  // 初始化 SPI、Panel IO 和 CO5300 面板
        &panel_io,                        // 输出给 LVGL adapter 的 Panel IO
        &panel_handle                        // 输出给 LVGL adapter 的面板句柄
    ) );

    esp_lcd_touch_handle_t touch_handle = init_touch();   // 显示完成后初始化 CST820
    init_lvgl(panel_io, panel_handle, touch_handle);      // 注册显示和触摸并启动 LVGL
}

