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
)
