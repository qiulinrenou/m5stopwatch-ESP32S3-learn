#ifndef WATCH_UI_H                 
#define WATCH_UI_H                 

#include "lvgl.h"                  // 提供 lv_display_t 类型；和下面函数参数配套使用
#include "esp_lv_adapter.h"        // 提供 lv_display_t；总头还包含 adapter 初始化、锁、输入等接口



/**
 * @brief 初始化当前手表界面
 *
 * 这个函数只负责创建 LVGL 界面对象，不负责初始化 LVGL adapter。
 * 调用前必须由外部获取 esp_lv_adapter 锁。
 *
 * @param display 已经由 esp_lv_adapter_register_display() 注册的显示器
 */
void ui_init(lv_display_t *display);          // 接收 main.c 中注册成功的 LVGL Display

#endif