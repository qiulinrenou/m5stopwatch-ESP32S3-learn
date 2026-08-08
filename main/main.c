#include <assert.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"

#include "esp_lv_adapter.h"
#include "lvgl.h"
#include "watch_ui.h"

#include "watch_board.h"                                  // 使用统一屏幕和触摸配置
#include "touch_port.h"                                  // 使用 watch_board 的触摸初始化接口

#include "board_power.h"
#include "co5300_init_cmds.h"

static const char *TAG = "M5STOPWATCH";

// CO5300 QSPI 引脚：保留用户已经修改好的屏幕连接。
#define LCD_PIN_CS      GPIO_NUM_39
#define LCD_PIN_SCK     GPIO_NUM_40
#define LCD_PIN_D0      GPIO_NUM_41
#define LCD_PIN_D1      GPIO_NUM_42
#define LCD_PIN_D2      GPIO_NUM_46
#define LCD_PIN_D3      GPIO_NUM_45
#define LCD_PIN_TE      GPIO_NUM_38

// 触摸中断由 ESP32 直接接收；触摸复位由 M5IOE1 IO4 控制。

#define LCD_HOST            SPI2_HOST
#define LCD_PIXEL_CLOCK_HZ  (40 * 1000 * 1000)
#define LCD_X_GAP           6
#define LCD_Y_GAP           0

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

static void init_display(esp_lcd_panel_io_handle_t *panel_io_out,
                         esp_lcd_panel_handle_t *panel_out)
{
    // M5IOE1 先拉起 L3B_EN，再通过 IO5 给 CO5300 一个完整复位脉冲。
    ESP_ERROR_CHECK(board_power_reset_display());

    spi_bus_config_t bus_config = CO5300_PANEL_BUS_QSPI_CONFIG(
        LCD_PIN_SCK,
        LCD_PIN_D0,
        LCD_PIN_D1,
        LCD_PIN_D2,
        LCD_PIN_D3,
        WATCH_LCD_H_RES * 80 * sizeof(uint16_t));
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus_config, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = LCD_PIN_CS,
        .dc_gpio_num = -1,
        .spi_mode = 0,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .trans_queue_depth = 1,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags = {
            .quad_mode = true,
        },
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, panel_io_out));

    co5300_vendor_config_t vendor_config = {
        .init_cmds = lcd_init_cmds,
        .init_cmds_size = lcd_init_cmds_size,
        .flags = {
            .use_qspi_interface = 1,
        },
    };
    esp_lcd_panel_dev_config_t panel_config = {
        // 屏幕复位接在 M5IOE1 IO5，不占用 ESP32 GPIO15。
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_config,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_co5300(*panel_io_out, &panel_config, panel_out));

    // 外部已经通过 IOE1 复位，因此此调用不会再切换任何 ESP32 GPIO。
    ESP_ERROR_CHECK(esp_lcd_panel_reset(*panel_out));
    ESP_ERROR_CHECK(esp_lcd_panel_init(*panel_out));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(*panel_out, LCD_X_GAP, LCD_Y_GAP));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(*panel_out, true));
    ESP_ERROR_CHECK(esp_lcd_panel_co5300_set_brightness(*panel_out, 80));

    ESP_LOGI(TAG, "CO5300 QSPI 显示初始化完成 (%dx%d)，TE=%d",
                WATCH_LCD_H_RES, WATCH_LCD_V_RES, LCD_PIN_TE);
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

    lv_obj_t *default_screen = lv_disp_get_scr_act(display);
    if (default_screen != NULL) {
        lv_obj_remove_style_all(default_screen);
        lv_obj_set_style_bg_color(default_screen, lv_color_black(), LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(default_screen, LV_OPA_COVER, LV_STATE_DEFAULT);
        lv_obj_set_size(default_screen, LV_PCT(100), LV_PCT(100));
    }

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
    ESP_ERROR_CHECK(board_power_init());
    ESP_LOGI(TAG, "板级管理芯片状态: M5PM1=%s, M5IOE1=%s",
             board_power_pmic_ready() ? "正常" : "不可用",
             board_power_ioe_ready() ? "正常" : "不可用");

    esp_lcd_panel_io_handle_t panel_io = NULL;
    esp_lcd_panel_handle_t panel_handle = NULL;
    init_display(&panel_io, &panel_handle);
    esp_lcd_touch_handle_t touch_handle = init_touch();
    init_lvgl(panel_io, panel_handle, touch_handle);

    ESP_LOGI(TAG, "M5StopWatch 初始化完成，系统正常运行");
}
