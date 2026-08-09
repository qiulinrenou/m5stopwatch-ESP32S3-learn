idf_component_register(                                  
    SRCS                                                 
        "src/touch_port.c"                  # CST820 触摸硬件初始化
        "src/display_port.c"                # CO5300 显示硬件初始化                               
    INCLUDE_DIRS                                         
        "include"                                        
    REQUIRES      

        esp_driver_i2c                     # 提供 i2c_master_bus_handle_t
        esp_driver_spi                      # 提供 SPI 总线初始化接口
        esp_driver_gpio                    # 提供 GPIO_NUM_NC 等 GPIO 定义

        esp_lcd                            # 提供 esp_lcd_new_panel_io_i2c()
        esp_lcd_touch                      # 提供 esp_lcd_touch_handle_t
        esp_lcd_touch_cst820               # 提供 CST820 驱动和配置宏
        esp_lcd_co5300                      # 提供 CO5300 驱动和 QSPI 配置
)