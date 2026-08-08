#ifndef TOUCH_PORT_H
#define TOUCH_PORT_H

#include "driver/i2c_master.h"                            // 提供 i2c_master_bus_handle_t
#include "esp_err.h"                                      // 提供 esp_err_t 和 ESP_OK
#include "esp_lcd_touch.h"                                // 提供 esp_lcd_touch_handle_t

/**
 * @brief 使用共享 I2C 创建 CST820 触摸设备。
 *
 * 本函数不创建 I2C 总线。
 * 本函数不执行触摸复位。
 * 本函数不调用 LVGL。
 *
 * 共享 I2C 和触摸复位仍由 main.c 中的 board_power 管理。
 *
 * @param i2c_bus 已经初始化完成的共享 I2C 总线
 * @param touch_out 输出 CST820 触摸设备句柄
 *
 * @return ESP_OK 表示创建成功
 */
esp_err_t touch_port_init(                                // 对外公开 CST820 创建接口
    i2c_master_bus_handle_t i2c_bus,                      // 接收 board_power 提供的共享 I2C
    esp_lcd_touch_handle_t *touch_out                     // 返回创建成功的触摸设备句柄
);

#endif