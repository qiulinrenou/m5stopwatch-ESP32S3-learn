idf_component_register(            # 将 watch_ui 目录注册为 ESP-IDF 组件
    SRCS                           # 下面列出需要参与编译的源文件
        "src/watch_ui.c"           # 编译 watch_ui.c；和实际文件路径配套
    INCLUDE_DIRS                   # 下面列出其他组件可以访问的头文件目录
        "include"                  # 让 main.c 可以使用 #include "watch_ui.h"
    REQUIRES                       # 下面列出 watch_ui 的公开依赖
        lvgl                       # 提供 LVGL 控件、lv_demo_widgets() 等 API
        esp_lvgl_adapter           # 提供 esp_lv_adapter.h 和 lv_display_t 类型
)