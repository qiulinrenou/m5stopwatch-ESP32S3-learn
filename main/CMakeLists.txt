idf_component_register(
    SRCS
        "main.c"
        "board_power.cpp"
    INCLUDE_DIRS
        "."
    PRIV_REQUIRES
        M5PM1
        M5IOE1
        # main.c 直接包含 esp_lcd_touch_cst820.h，必须声明直接依赖。
        esp_lcd_touch_cst820
        watch_ui            # 新增：main 依赖 watch_ui 组件
        watch_board         # main.c 使用 touch_port.h 和 touch_port_init()
        watch_lvgl                 # main 使用统一 LVGL 运行时
        watch_core                 # main 组装 RTC、电量和主表盘数据任务
        watch_images               # main 只装配图片存储公开接口
        watch_ble                  # main 只接收 BLE 应用事件
        heap                       # 异步 JPEG 缓冲使用 heap_caps_free() 交给 UI
        freertos                   # 固定队列桥接 BLE/存储回调与 LVGL
)
