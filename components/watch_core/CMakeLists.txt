idf_component_register(                 # 将 watch_core 注册为业务协调组件
    SRCS
        "src/watch_core.c"              # 编译 RTC、电量和 UI 数据协调任务
    INCLUDE_DIRS
        "include"                       # 向 main 公开 watch_core.h
    REQUIRES
        watch_board                     # 提供 RX8130 日期时间类型
        watch_lvgl                      # 提供统一的 LVGL 线程安全入口
        watch_ui                        # 提供主表盘数据更新接口
        freertos                        # 提供周期数据任务
)
