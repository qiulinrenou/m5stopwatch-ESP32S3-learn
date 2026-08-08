#include "touch_port.h"                             // 引入本模块自己的公共接口
#include "watch_board.h"                           // 引入当前手表的硬件配置

#include "esp_lcd_panel_io.h"                      // 提供 esp_lcd_new_panel_io_i2c()
#include "esp_lcd_touch_cst820.h"                  // 使用已经存在的 CST820 驱动组件

/**
 * @brief 使用共享 I2C 创建当前手表连接方式下的 CST820。
 *
 * 这里调用的是已有的 esp_lcd_touch_cst820 驱动，
 * 本文件只负责传入当前手表的硬件参数。
 */
esp_err_t touch_port_init(i2c_master_bus_handle_t i2c_bus, 
    esp_lcd_touch_handle_t *touch_out )                     //  接收外部已经创建的共享 I2C和返回 CST820 设备句柄
{
    if (i2c_bus == NULL || touch_out == NULL) {           // 检查共享 I2C 和输出地址是否有效
        return ESP_ERR_INVALID_ARG;                       // 参数无效时直接返回错误
    }

    *touch_out = NULL;                                    // 先清空输出句柄，避免误用旧句柄

    esp_lcd_panel_io_i2c_config_t touch_io_config =       // 创建 CST820 的 I2C 配置
        ESP_LCD_TOUCH_IO_I2C_CST820_CONFIG();             // 使用已有 CST820 驱动提供的默认配置

    touch_io_config.scl_speed_hz =                        // 设置 CST820 的 I2C 时钟频率
        WATCH_TOUCH_I2C_HZ;                               // 使用统一板级配置中的 I2C 频率

    esp_lcd_panel_io_handle_t touch_io = NULL;            // 保存创建后的触摸 IO 句柄

    esp_err_t err = esp_lcd_new_panel_io_i2c(              // 在共享 I2C 上创建触摸 IO
        i2c_bus,                                          // 使用 board_power 创建的共享 I2C
        &touch_io_config,                                 // 传入 CST820 I2C 配置
        &touch_io                                         // 返回触摸 IO 句柄
    );

    if (err != ESP_OK) {                                  // 检查触摸 IO 是否创建成功
        return err;                                       // 失败时把具体错误返回给调用者
    }

    esp_lcd_touch_config_t touch_config = {               // 创建通用触摸设备配置
        .x_max = WATCH_LCD_H_RES,                         // 使用统一配置中的屏幕宽度
        .y_max = WATCH_LCD_V_RES,                         // 使用统一配置中的屏幕高度
        .rst_gpio_num = GPIO_NUM_NC,                      // 触摸复位由 M5IOE1 控制
        .int_gpio_num = WATCH_TOUCH_PIN_INT,              // 使用统一配置中的触摸中断 GPIO
        .levels = {
            .reset = 0,                                   // 保持原工程的复位有效电平
            .interrupt = 0,                               // 保持原工程的中断有效电平
        },
        .flags = {
            .swap_xy = 0,                                 // 不交换 X/Y 坐标
            .mirror_x = 0,                                // 不镜像 X 坐标
            .mirror_y = 0,                                // 不镜像 Y 坐标
        },
    };

    return esp_lcd_touch_new_i2c_cst820(                  // 调用已有 CST820 驱动创建设备
        touch_io,                                         // 传入刚刚创建的触摸 IO
        &touch_config,                                    // 传入触摸坐标和 GPIO 配置
        touch_out                                         // 输出 CST820 触摸句柄
    );
}