#ifndef WATCH_LVGL_H
#define WATCH_LVGL_H

#include <stdbool.h>                              // 提供 bool 类型
#include <stdint.h>                               // 提供 uint16_t 类型

#include "esp_err.h"                              // 提供 esp_err_t
#include "esp_lcd_panel_io.h"                     // 提供 Panel IO 句柄
#include "esp_lcd_panel_ops.h"                    // 提供 Panel 句柄
#include "esp_lcd_touch.h"                        // 提供 Touch 句柄
#include "esp_lv_adapter_display.h"                // 提供 LVGL 8/9 兼容的 lv_display_t

    typedef enum {
        WATCH_LVGL_ROTATION_0   = 0,    // 顺时针旋转 0 度
        WATCH_LVGL_ROTATION_90  =90,       // 顺时针旋转 90 度
        WATCH_LVGL_ROTATION_180 =180,      // 顺时针旋转 180 度
        WATCH_LVGL_ROTATION_270 =270,      // 顺时针旋转 270 度
    } watch_lvgl_rotation_t;

/**
 * @brief 初始界面创建回调类型。
 *
 * 回调由 watch_lvgl_start() 在 LVGL 锁内执行。
 */
    typedef void (*watch_lvgl_ui_init_cb_t)(lv_display_t *display); // 接收已注册的 lv_display

 /**
 * @brief 通用 LVGL 操作回调类型。
 *
 * 普通任务通过 watch_lvgl_run() 执行此类回调。
 */
    typedef void (*watch_lvgl_action_t)(
        lv_display_t *display,  
        void *user_data
    ); // 接收已注册的 lv_display 和用户数据

    typedef struct{
        esp_lcd_panel_io_handle_t panel_io;           // display_port 创建的 Panel IO
        esp_lcd_panel_handle_t panel;                 // display_port 创建的 Panel
        esp_lcd_touch_handle_t touch;                 // touch_port 创建的 Touch
        uint16_t horizontal_resolution;               // 屏幕横向分辨率
        uint16_t vertical_resolution;                 // 屏幕纵向分辨率
        uint16_t horizontal_alignment;                // 刷新区域横向对齐像素数
        uint16_t vertical_alignment;                  // 刷新区域纵向对齐像素数
        watch_lvgl_rotation_t rotation;               // LVGL 显示旋转方向
        bool use_psram;                               // 帧缓冲和任务栈是否使用 PSRAM
    } watch_lvgl_config_t;

/**
 * @brief 初始化并启动当前设备的 LVGL 运行时。
 *
 * 函数依次初始化 adapter、加锁、注册显示和触摸、创建初始界面、
 * 解锁，最后启动 LVGL 工作任务。
 *
 * @param config LVGL 显示、触摸和内存配置。
 * @param ui_init_cb 初始界面创建函数，在 LVGL 锁内调用。
 * @return ESP_OK 表示启动成功，否则返回具体错误码。
 *
 * @note 只能调用一次。
 * @note 调用前显示和触摸硬件必须初始化完成。
 * @note ui_init_cb 不得自行调用 watch_lvgl_run()。
 */

    esp_err_t watch_lvgl_start(
        const watch_lvgl_config_t *config,
        watch_lvgl_ui_init_cb_t ui_init_cb
    );

/**
 * @brief 在 LVGL adapter 锁内同步执行一次 UI 操作。
 *
 * @param action 要执行的 UI 操作。
 * @param user_data 传给 action 的数据，可以为 NULL。
 * @return ESP_OK 表示操作已执行，否则返回具体错误码。
 *
 * @note 禁止在硬件中断 ISR 中调用。
 * @note action 返回前，user_data 必须保持有效。
 * @note LVGL 事件和定时器回调已经处于 LVGL 上下文，不需要调用本函数。
 */
    esp_err_t watch_lvgl_run(
        watch_lvgl_action_t action,
        void *user_data
    );

#endif

