idf_component_register(                 # 将按键输入注册为独立 ESP-IDF 组件
    SRCS
        "src/watch_button.c"            # 编译 GPIO 配置、轮询和去抖实现
    INCLUDE_DIRS
        "include"                       # 向应用层公开按键配置和启动接口
    REQUIRES
        esp_driver_gpio                  # 提供 GPIO 输入、上拉和电平读取 API
        freertos                         # 提供按键轮询任务和延时 API
        log                              # 提供 esp_log.h 诊断日志接口
)
