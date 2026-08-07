#include "watch_ui.h"

#include "lv_demos.h"

/**
 * @brief 创建当前工程的 LVGL 界面
 *
 * 当前阶段继续显示原来的 Widgets Demo，
 * 目的是验证模块化后显示和触摸功能没有变化。
 *
 * 注意：本函数不加锁，由调用者 main.c 负责加锁。
 */
void ui_init(lv_display_t *display)          // 接收 main.c 传入的 LVGL Display
{
    (void)display;                 // 当前暂时没有使用 display，避免编译器提示参数未使用

    lv_demo_widgets();             // 创建原来的 LVGL Demo；与 lv_demos.h 配套使用
}