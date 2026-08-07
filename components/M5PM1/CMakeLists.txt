idf_component_register(
    SRCS
        "src/M5PM1.cpp"
    INCLUDE_DIRS
        "src"
    REQUIRES
        "driver"
        # IDF 6 将新 I2C 和 GPIO 驱动拆分为独立组件，必须显式声明。
        "esp_driver_i2c"
        "esp_driver_gpio"
)

# M5Unified 为可选依赖：组件存在时自动添加，否则静默跳过
# M5Unified is optional: added automatically if present, silently ignored otherwise.
idf_component_optional_requires(PRIVATE M5Unified)
