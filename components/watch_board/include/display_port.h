#ifndef DISPLAY_PORT_H
#define DISPLAY_PORT_H

#include "esp_err.h"                                  // 提供 esp_err_t 错误码类型
#include "esp_lcd_panel_io.h"                         // 提供 Panel IO 句柄类型
#include "esp_lcd_panel_ops.h"                        // 提供显示面板句柄类型

/**
 * @brief 初始化已经完成板级复位的 CO5300 QSPI 显示面板。
 *
 * 本函数负责初始化 SPI 总线、创建 Panel IO、创建 CO5300 面板，
 * 并完成面板初始化、显示偏移、开屏和亮度设置。
 *
 * @param panel_io_out 输出 Panel IO 句柄，后续交给 LVGL adapter。
 * @param panel_out 输出 CO5300 面板句柄，后续交给 LVGL adapter。
 * @return ESP_OK 表示初始化成功，否则返回具体 ESP-IDF 错误码。
 *
 * @note 调用前必须先执行 board_power_reset_display()。
 * @note 本函数不调用 LVGL API，因此不需要获取 LVGL 锁。
 * @note 本函数只应在系统启动阶段调用一次。
 */
esp_err_t display_port_init(
    esp_lcd_panel_io_handle_t *panel_io_out,          // 输出显示通信 IO 句柄
    esp_lcd_panel_handle_t *panel_out                 // 输出 CO5300 面板句柄
);

#endif