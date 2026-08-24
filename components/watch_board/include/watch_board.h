#ifndef WATCH_BOARD_H
#define WATCH_BOARD_H

#include "driver/gpio.h"                             // 提供 GPIO_NUM_13 等 GPIO 定义

#define WATCH_LCD_H_RES       466                    // 手表屏幕横向分辨率，显示和触摸共同使用
#define WATCH_LCD_V_RES       466                    // 手表屏幕纵向分辨率，显示和触摸共同使用

#define WATCH_TOUCH_PIN_INT   GPIO_NUM_13            // CST820 触摸中断 GPIO
#define WATCH_TOUCH_I2C_HZ    (400 * 1000)           // CST820 使用的 I2C 通信频率
#define WATCH_G1_BUTTON_PIN   GPIO_NUM_1             // G1 蓝色按键，按下时接地
#define WATCH_G2_BUTTON_PIN   GPIO_NUM_2             // G2 黄色按键，按下时接地
#define WATCH_BUTTON_POLL_MS  10                     // 按键轮询周期
#define WATCH_BUTTON_DEBOUNCE_MS 40                  // 按键稳定去抖时间
#define WATCH_BUTTON_LONG_PRESS_MS 1500U             // 黄色 G2 长按删除触发时间

#endif
