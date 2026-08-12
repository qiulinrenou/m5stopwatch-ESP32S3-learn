idf_component_register(                 # 将 watch_ui 注册为独立的 ESP-IDF 组件
    SRCS
        "src/watch_ui.c"                # 编译主表盘的 LVGL 界面实现
    INCLUDE_DIRS
        "include"                       # 向 main 公开 watch_ui.h
    REQUIRES
        lvgl                            # 提供 LVGL 控件、字体、样式和定时器
        esp_lvgl_adapter                # 提供兼容 LVGL 8/9 的 Display 类型
)
