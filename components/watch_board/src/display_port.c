#include "display_port.h"                            // 引入本模块公开的显示初始化接口
#include "watch_board.h"                             // 使用统一的屏幕分辨率配置
#include "co5300_init_cmds.h"                        // 使用当前 CO5300 初始化命令表

#include "driver/gpio.h"                             // 提供 GPIO_NUM_NC 和 GPIO 引脚定义
#include "driver/spi_master.h"                       // 提供 SPI 总线初始化接口
#include "esp_lcd_co5300.h"                          // 提供 CO5300 驱动和 QSPI 配置宏
#include "esp_lcd_panel_vendor.h"                    // 提供面板设备配置结构
#include "esp_log.h"                                 // 提供显示初始化日志接口

static const char *TAG = "M5STOPWATCH";              // 保持原 main.c 的日志标签

#define LCD_PIN_CS      GPIO_NUM_39                  // CO5300 QSPI 片选引脚
#define LCD_PIN_SCK     GPIO_NUM_40                  // CO5300 QSPI 时钟引脚
#define LCD_PIN_D0      GPIO_NUM_41                  // CO5300 QSPI 数据引脚 D0
#define LCD_PIN_D1      GPIO_NUM_42                  // CO5300 QSPI 数据引脚 D1
#define LCD_PIN_D2      GPIO_NUM_46                  // CO5300 QSPI 数据引脚 D2
#define LCD_PIN_D3      GPIO_NUM_45                  // CO5300 QSPI 数据引脚 D3
#define LCD_PIN_TE      GPIO_NUM_38                  // CO5300 TE 引脚，当前只用于日志

#define LCD_HOST            SPI2_HOST                // CO5300 使用 SPI2 控制器
#define LCD_PIXEL_CLOCK_HZ  (40 * 1000 * 1000)       // 保持原来的 40 MHz 通信时钟
#define LCD_X_GAP           6                        // CO5300 横向显示偏移
#define LCD_Y_GAP           0                        // CO5300 纵向显示偏移

/**
 * @brief 初始化已经完成板级复位的 CO5300 QSPI 显示面板。
 *
 * @param panel_io_out 输出 Panel IO 句柄，供 LVGL adapter 注册显示。
 * @param panel_out 输出显示面板句柄，供 LVGL adapter 注册显示。
 * @return ESP_OK 表示初始化成功，否则返回具体 ESP-IDF 错误码。
 *
 * @note 调用前必须先执行 board_power_reset_display()。
 * @note 本函数不调用 LVGL API，不需要获取 LVGL 锁。
 * @note 本函数只应在 app_main() 的启动流程中调用一次。
 */
esp_err_t display_port_init(
    esp_lcd_panel_io_handle_t *panel_io_out,
    esp_lcd_panel_handle_t *panel_out)
{
    if (panel_io_out == NULL || panel_out == NULL) { // 检查两个输出参数是否有效
        return ESP_ERR_INVALID_ARG;                  // 参数无效时返回标准错误码
    }

    *panel_io_out = NULL;                            // 清空 Panel IO 输出，避免误用旧句柄
    *panel_out = NULL;                               // 清空面板输出，保持失败状态明确

    spi_bus_config_t bus_config =                    // 创建 CO5300 QSPI 总线配置
        CO5300_PANEL_BUS_QSPI_CONFIG(
            LCD_PIN_SCK,                            // 设置 QSPI 时钟引脚
            LCD_PIN_D0,                             // 设置 QSPI 数据引脚 D0
            LCD_PIN_D1,                             // 设置 QSPI 数据引脚 D1
            LCD_PIN_D2,                             // 设置 QSPI 数据引脚 D2
            LCD_PIN_D3,                             // 设置 QSPI 数据引脚 D3
            WATCH_LCD_H_RES * 80 * sizeof(uint16_t) // 保持原来的最大 DMA 传输大小
        );

    esp_err_t err = spi_bus_initialize(             // 初始化 CO5300 使用的 SPI2 总线
        LCD_HOST,                                   // 指定 SPI2 控制器
        &bus_config,                                // 传入 QSPI 总线配置
        SPI_DMA_CH_AUTO                             // 由 ESP-IDF 自动选择 DMA 通道
    );

    if (err != ESP_OK) {                            // 检查 SPI 总线初始化结果
        return err;                                 // 返回具体的 SPI 初始化错误
    }

    esp_lcd_panel_io_spi_config_t io_config = {     // 配置 CO5300 Panel IO
        .cs_gpio_num = LCD_PIN_CS,                  // 设置屏幕片选引脚
        .dc_gpio_num = -1,                          // QSPI 模式不使用独立 DC 引脚
        .spi_mode = 0,                              // 保持原来的 SPI 模式 0
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,              // 设置 40 MHz 通信时钟
        .trans_queue_depth = 1,                     // 保持原来的事务队列深度
        .lcd_cmd_bits = 32,                         // CO5300 QSPI 命令宽度为 32 位
        .lcd_param_bits = 8,                        // CO5300 参数宽度为 8 位
        .flags = {
            .quad_mode = true,                      // 启用 QSPI 四线数据模式
        },
    };

    err = esp_lcd_new_panel_io_spi(                 // 在 SPI2 总线上创建 Panel IO
        (esp_lcd_spi_bus_handle_t)LCD_HOST,         // 将 SPI2 主机转换为 LCD SPI 总线句柄
        &io_config,                                 // 传入 CO5300 Panel IO 配置
        panel_io_out                                // 输出创建后的 Panel IO 句柄
    );

    if (err != ESP_OK) {                            // 检查 Panel IO 是否创建成功
        return err;                                 // 返回具体的 Panel IO 创建错误
    }

    co5300_vendor_config_t vendor_config = {        // 创建 CO5300 厂商专用配置
        .init_cmds = lcd_init_cmds,                 // 使用移动到本模块的初始化命令表
        .init_cmds_size = lcd_init_cmds_size,       // 传入初始化命令数量
        .flags = {
            .use_qspi_interface = 1,                // 告知 CO5300 驱动使用 QSPI 接口
        },
    };

    esp_lcd_panel_dev_config_t panel_config = {     // 创建 CO5300 通用面板配置
        .reset_gpio_num = GPIO_NUM_NC,              // 屏幕复位由 M5IOE1 控制
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB, // 保持原来的 RGB 颜色顺序
        .bits_per_pixel = 16,                       // 使用 RGB565 像素格式
        .vendor_config = &vendor_config,            // 传入 CO5300 厂商配置
    };

    err = esp_lcd_new_panel_co5300(                 // 创建 CO5300 面板驱动对象
        *panel_io_out,                          // 使用刚创建的 Panel IO
        &panel_config,                    // 传入面板配置
        panel_out                            // 输出 CO5300 面板句柄
    );

    if (err != ESP_OK) {                            // 检查面板对象是否创建成功
        return err;                                 // 返回具体的面板创建错误
    }

    err = esp_lcd_panel_reset(*panel_out);    // 执行驱动层复位流程
    if (err != ESP_OK) {                            // 检查驱动层复位结果
        return err;                                 // 失败时返回具体错误码
    }

    err = esp_lcd_panel_init(*panel_out);     // 发送 CO5300 初始化命令表
    if (err != ESP_OK) {                            // 检查面板初始化结果
        return err;                                 // 失败时返回具体错误码
    }

    err = esp_lcd_panel_set_gap(                    // 设置 CO5300 显示区域偏移
        *panel_out,                                 // 使用已经初始化的面板句柄
        LCD_X_GAP,                                  // 设置横向偏移
        LCD_Y_GAP                                   // 设置纵向偏移
    );

    if (err != ESP_OK) {                            // 检查显示偏移设置结果
        return err;                                 // 失败时返回具体错误码
    }

    err = esp_lcd_panel_disp_on_off(                // 打开 CO5300 显示输出
        *panel_out,                                 // 使用当前面板句柄
        true                                        // true 表示开启显示
    );

    if (err != ESP_OK) {                            // 检查开屏操作结果
        return err;                                 // 失败时返回具体错误码
    }

    err = esp_lcd_panel_co5300_set_brightness(      // 设置 CO5300 内部显示亮度
        *panel_out,                           // 使用当前面板句柄
        80                               // 保持原工程的 80% 亮度
    );

    if (err != ESP_OK) {                            // 检查亮度设置结果
        return err;                                 // 失败时返回具体错误码
    }

    ESP_LOGI(                                       // 输出与原程序一致的初始化日志
        TAG,
        "CO5300 QSPI 显示初始化完成 (%dx%d)，TE=%d",
        WATCH_LCD_H_RES,
        WATCH_LCD_V_RES,
        LCD_PIN_TE
    );

    return ESP_OK;
}