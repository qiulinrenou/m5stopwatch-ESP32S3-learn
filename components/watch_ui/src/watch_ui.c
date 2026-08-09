#include "watch_ui.h"

#include "lv_demos.h"

/**
 * @brief 创建当前工程的 LVGL 界面。
 *
 * @param display watch_lvgl 已经注册完成的 LVGL Display。
 *
 * @note 本函数不自行加锁。
 * @note 必须由 watch_lvgl_start() 或 watch_lvgl_run() 在锁内调用。
 */
void ui_init(lv_display_t *display)
{
    if (display == NULL) {                           // 防止使用无效 Display
        return;
    }

    lv_obj_t *default_screen =
        lv_disp_get_scr_act(display);                // 取得当前显示的活动屏幕

    if (default_screen != NULL) {
        lv_obj_remove_style_all(default_screen);     // 清除活动屏幕原有样式
        lv_obj_set_style_bg_color(
            default_screen,
            lv_color_black(),
            LV_STATE_DEFAULT
        );
        lv_obj_set_style_bg_opa(
            default_screen,
            LV_OPA_COVER,
            LV_STATE_DEFAULT
        );
        lv_obj_set_size(
            default_screen,
            LV_PCT(100),
            LV_PCT(100)
        );
    }

    lv_demo_widgets();                               // 创建原来的 LVGL Widgets Demo
}