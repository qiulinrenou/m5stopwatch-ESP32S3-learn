idf_component_register(                                  
    SRCS                                                 
        "src/touch_port.c"                               
    INCLUDE_DIRS                                         
        "include"                                        
    REQUIRES                                             
        esp_driver_i2c                     # 提供 i2c_master_bus_handle_t
        esp_driver_gpio                    # 提供 GPIO_NUM_NC 等 GPIO 定义
        esp_lcd                            # 提供 esp_lcd_new_panel_io_i2c()
        esp_lcd_touch                      # 提供 esp_lcd_touch_handle_t
        esp_lcd_touch_cst820               # 提供 CST820 驱动和配置宏
)