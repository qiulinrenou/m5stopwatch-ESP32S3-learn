#ifndef WATCH_BOARD_H
#define WATCH_BOARD_H

#include "driver/gpio.h"                             // 提供 GPIO_NUM_13 等 GPIO 定义

#define WATCH_LCD_H_RES       466                    // 手表屏幕横向分辨率，显示和触摸共同使用
#define WATCH_LCD_V_RES       466                    // 手表屏幕纵向分辨率，显示和触摸共同使用

#define WATCH_TOUCH_PIN_INT   GPIO_NUM_13            // CST820 触摸中断 GPIO
#define WATCH_TOUCH_I2C_HZ    (400 * 1000)           // CST820 使用的 I2C 通信频率

#endif