idf_component_register(
    SRCS
        "src/watch_lvgl.c"           # 编译 LVGL 运行时实现
    INCLUDE_DIRS
        "include"                    # 向其他组件公开 watch_lvgl.h
    REQUIRES
        esp_lvgl_adapter             # 提供 adapter 生命周期、显示、输入和锁
        esp_lcd                      # 提供显示面板和 Panel IO 句柄
        esp_lcd_touch                # 提供触摸设备句柄
        lvgl                         # 提供 lv_display_t 和 lv_area_t
)
