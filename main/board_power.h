#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化共享 I2C 总线、M5PM1 和 M5IOE1。
 *
 * M5PM1 初始化失败不会阻止 M5IOE1 继续初始化；M5IOE1 负责屏幕和触摸
 * 复位，因此 M5IOE1 初始化失败时本函数返回错误。
 */
esp_err_t board_power_init(void);

/** @brief 获取由板级模块创建的共享 I2C 主总线句柄。 */
i2c_master_bus_handle_t board_power_get_i2c_bus(void);

/** @brief 查询 M5PM1 是否已经正常初始化。 */
bool board_power_pmic_ready(void);

/** @brief 查询 M5IOE1 是否已经正常初始化。 */
bool board_power_ioe_ready(void);

/** @brief 通过 M5IOE1 IO5 对 CO5300/OLED 执行硬件复位。 */
esp_err_t board_power_reset_display(void);

/** @brief 通过 M5IOE1 IO4 对 CST820 执行硬件复位。 */
esp_err_t board_power_reset_touch(void);

/** @brief 控制 L3B 显示电源使能，正常运行时应保持开启。 */
esp_err_t board_power_set_display_power(bool enabled);

/** @brief 控制 M5IOE1 IO3 音频电源使能。 */
esp_err_t board_power_set_audio_enabled(bool enabled);

/** @brief 控制 CH442E MUX，level 为 false/true 时分别输出低/高电平。 */
esp_err_t board_power_set_mux(bool level);

/**
 * @brief 设置振动电机强度。
 * @param strength 强度百分比，范围 0-100；0 表示关闭。
 */
esp_err_t board_power_set_motor(uint8_t strength);

/**
 * @brief 按指定强度振动一段时间，时间到后自动停止。
 * @param duration_ms 振动持续时间，单位毫秒。
 * @param strength 强度百分比，范围 0-100。
 */
esp_err_t board_power_vibrate(uint16_t duration_ms, uint8_t strength);

/** @brief 立即停止振动电机。 */
esp_err_t board_power_stop_vibration(void);

/**
 * @brief 控制扬声器功放。
 *
 * 同时控制 M5IOE1 IO10 和 ESP32 GPIO14，避免只打开一级使能造成底噪。
 */
esp_err_t board_power_set_speaker_enabled(bool enabled);

/** @brief 读取滤波后的电池电压，单位毫伏。 */
esp_err_t board_power_get_battery_mv(uint16_t *battery_mv);

/** @brief 读取估算电量，范围 0-100。 */
esp_err_t board_power_get_battery_percent(uint8_t *percent);

/** @brief 读取 VIN 电压，单位毫伏。 */
esp_err_t board_power_get_vin_mv(uint16_t *vin_mv);

/**
 * @brief 查询充电状态。
 * @param strict 为 true 时只有 CHG_STAT 有效充电才返回 true；为 false 时只要检测到外部电源即返回 true。
 */
esp_err_t board_power_is_charging(bool strict, bool *charging);

/** @brief 读取 M5PM1 电源按键的当前状态。 */
esp_err_t board_power_get_button_state(bool *pressed);

#ifdef __cplusplus
}
#endif
