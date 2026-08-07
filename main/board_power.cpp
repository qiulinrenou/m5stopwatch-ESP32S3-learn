#include "board_power.h"

#include <algorithm>
#include <memory>
#include <mutex>

#include "M5IOE1.h"
#include "M5PM1.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr char TAG[] = "BOARD_POWER";

// M5StopWatch 主板上的共享 I2C 总线，PMIC、IOE1 和 CST820 均挂在这条总线上。
constexpr i2c_port_t I2C_PORT = I2C_NUM_0;
constexpr gpio_num_t I2C_SDA_PIN = GPIO_NUM_47;
constexpr gpio_num_t I2C_SCL_PIN = GPIO_NUM_48;
constexpr uint32_t I2C_SPEED_HZ = 100000;

constexpr uint8_t IOE_ADDR_PRIMARY = 0x4F;
constexpr uint8_t IOE_ADDR_SECONDARY = 0x6F;
constexpr uint32_t IOE_SPEED_HZ = 400000;

// M5IOE1 引脚与 M5StopWatch 板级功能的连接关系。
constexpr uint8_t IOE_MOTOR_EN = M5IOE1_PIN_9;
constexpr uint8_t IOE_L3B_EN = M5IOE1_PIN_8;
constexpr uint8_t IOE_SPEAKER_PA = M5IOE1_PIN_10;
constexpr uint8_t IOE_TOUCH_RST = M5IOE1_PIN_4;
constexpr uint8_t IOE_DISPLAY_RST = M5IOE1_PIN_5;
constexpr uint8_t IOE_MUX_CTRL = M5IOE1_PIN_1;
constexpr uint8_t IOE_AUDIO_EN = M5IOE1_PIN_3;
constexpr uint8_t MOTOR_PWM_CHANNEL = 0;
constexpr gpio_num_t SPEAKER_PA_PIN = GPIO_NUM_14;

// M5PM1 GPIO2 连接充电状态，GPIO3 拉低后启用硬件充电电流配置。
constexpr m5pm1_gpio_num_t PMIC_CHG_STAT = M5PM1_GPIO_NUM_2;
constexpr m5pm1_gpio_num_t PMIC_CHG_PROG = M5PM1_GPIO_NUM_3;

constexpr uint16_t BATTERY_EMPTY_MV = 3300;
constexpr uint16_t BATTERY_FULL_MV = 4200;
constexpr uint32_t POWER_SAMPLE_PERIOD_MS = 1000;
constexpr uint32_t POWER_LOG_PERIOD_MS = 10000;

i2c_master_bus_handle_t i2c_bus = nullptr;
std::unique_ptr<M5PM1> pmic;
std::unique_ptr<M5IOE1> ioe;
std::mutex state_mutex;
std::mutex vibration_mutex;
uint16_t filtered_battery_mv = 0;
uint8_t battery_percent = 0;
TaskHandle_t vibration_task_handle = nullptr;
bool vibration_enabled = false;
TickType_t vibration_end_tick = 0;
uint8_t vibration_strength = 0;

void vibration_control_task(void *);

uint8_t millivolts_to_percent(uint16_t millivolts)
{
    if (millivolts <= BATTERY_EMPTY_MV) {
        return 0;
    }
    if (millivolts >= BATTERY_FULL_MV) {
        return 100;
    }

    const uint32_t scaled = static_cast<uint32_t>(millivolts - BATTERY_EMPTY_MV) * 100U /
                            (BATTERY_FULL_MV - BATTERY_EMPTY_MV);
    return static_cast<uint8_t>(std::min<uint32_t>(scaled, 100U));
}

void update_battery_filter(uint16_t battery_mv)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    if (filtered_battery_mv == 0) {
        filtered_battery_mv = battery_mv;
    } else {
        // 采用源工程的 7:1 指数滤波，减少负载变化造成的电量显示抖动。
        filtered_battery_mv = static_cast<uint16_t>(
            (static_cast<uint32_t>(filtered_battery_mv) * 7U + battery_mv + 4U) / 8U);
    }
    battery_percent = millivolts_to_percent(filtered_battery_mv);
}

esp_err_t check_pmic_result(m5pm1_err_t result, const char *operation)
{
    if (result == M5PM1_OK) {
        return ESP_OK;
    }
    ESP_LOGE(TAG, "M5PM1 %s 失败，错误码=%d", operation, static_cast<int>(result));
    return ESP_FAIL;
}

esp_err_t check_ioe_result(m5ioe1_err_t result, const char *operation)
{
    if (result == M5IOE1_OK) {
        return ESP_OK;
    }
    ESP_LOGE(TAG, "M5IOE1 %s 失败，错误码=%d", operation, static_cast<int>(result));
    return ESP_FAIL;
}

esp_err_t ioe_pin_mode(uint8_t pin, uint8_t mode)
{
    m5ioe1_err_t result = M5IOE1_FAIL;
    ioe->pinModeWithRes(pin, mode, &result);
    return check_ioe_result(result, "配置 GPIO 模式");
}

esp_err_t ioe_write(uint8_t pin, uint8_t level)
{
    m5ioe1_err_t result = M5IOE1_FAIL;
    ioe->digitalWriteWithRes(pin, level, &result);
    return check_ioe_result(result, "写 GPIO");
}

esp_err_t init_i2c_bus()
{
    const i2c_master_bus_config_t config = {
        .i2c_port = I2C_PORT,
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {
            .enable_internal_pullup = true,
            .allow_pd = false,
        },
    };

    const esp_err_t result = i2c_new_master_bus(&config, &i2c_bus);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "创建共享 I2C 总线失败: %s", esp_err_to_name(result));
        return result;
    }

    ESP_LOGI(TAG, "共享 I2C 初始化完成: SDA=%d, SCL=%d, port=%d",
             static_cast<int>(I2C_SDA_PIN), static_cast<int>(I2C_SCL_PIN), static_cast<int>(I2C_PORT));
    return ESP_OK;
}

esp_err_t init_pmic()
{
    pmic = std::make_unique<M5PM1>();
    if (pmic->begin(i2c_bus, M5PM1_DEFAULT_ADDR, I2C_SPEED_HZ) != M5PM1_OK) {
        ESP_LOGE(TAG, "未找到 M5PM1，地址=0x%02X", M5PM1_DEFAULT_ADDR);
        pmic.reset();
        return ESP_ERR_NOT_FOUND;
    }

    // 与 UserDemo 保持一致：连续写两次关闭休眠，兼容芯片刚唤醒时的首次事务。
    if (check_pmic_result(pmic->setI2cSleepTime(0), "关闭 I2C 休眠(1)") != ESP_OK ||
        check_pmic_result(pmic->setI2cSleepTime(0), "关闭 I2C 休眠(2)") != ESP_OK ||
        check_pmic_result(pmic->btnSetConfig(M5PM1_BTN_TYPE_CLICK, M5PM1_BTN_CLICK_DELAY_1000MS),
                          "配置电源按键") != ESP_OK ||
        check_pmic_result(pmic->wdtSet(0), "关闭看门狗") != ESP_OK ||
        check_pmic_result(pmic->ldoSetPowerHold(true), "保持 LDO 供电") != ESP_OK ||
        check_pmic_result(pmic->setChargeEnable(true), "使能充电") != ESP_OK ||
        check_pmic_result(pmic->gpioSet(PMIC_CHG_PROG, M5PM1_GPIO_MODE_OUTPUT, 0,
                                        M5PM1_GPIO_PULL_NONE, M5PM1_GPIO_DRIVE_PUSHPULL),
                          "配置 CHG_PROG") != ESP_OK ||
        check_pmic_result(pmic->gpioSetFunc(PMIC_CHG_STAT, M5PM1_GPIO_FUNC_GPIO),
                          "配置 CHG_STAT 功能") != ESP_OK ||
        check_pmic_result(pmic->gpioSetMode(PMIC_CHG_STAT, M5PM1_GPIO_MODE_INPUT),
                          "配置 CHG_STAT 输入") != ESP_OK ||
        check_pmic_result(pmic->gpioSetPull(PMIC_CHG_STAT, M5PM1_GPIO_PULL_NONE),
                          "配置 CHG_STAT 上下拉") != ESP_OK ||
        check_pmic_result(pmic->setSingleResetDisable(true), "禁用单击复位") != ESP_OK) {
        pmic.reset();
        return ESP_FAIL;
    }

    uint16_t battery_mv = 0;
    if (pmic->readVbat(&battery_mv) == M5PM1_OK) {
        update_battery_filter(battery_mv);
    }
    ESP_LOGI(TAG, "M5PM1 初始化完成，I2C 地址=0x%02X", M5PM1_DEFAULT_ADDR);
    return ESP_OK;
}

esp_err_t configure_ioe_output(uint8_t pin, uint8_t initial_level)
{
    if (ioe_pin_mode(pin, OUTPUT) != ESP_OK) {
        return ESP_FAIL;
    }
    return ioe_write(pin, initial_level);
}

esp_err_t init_ioe()
{
    ioe = std::make_unique<M5IOE1>();
    uint8_t selected_address = IOE_ADDR_PRIMARY;
    m5ioe1_err_t result = ioe->begin(i2c_bus, IOE_ADDR_PRIMARY, IOE_SPEED_HZ, M5IOE1_INT_MODE_DISABLED);
    if (result != M5IOE1_OK) {
        selected_address = IOE_ADDR_SECONDARY;
        result = ioe->begin(i2c_bus, IOE_ADDR_SECONDARY, IOE_SPEED_HZ, M5IOE1_INT_MODE_DISABLED);
    }
    if (result != M5IOE1_OK) {
        ESP_LOGE(TAG, "未找到 M5IOE1，已探测地址 0x%02X 和 0x%02X",
                 IOE_ADDR_PRIMARY, IOE_ADDR_SECONDARY);
        ioe.reset();
        return ESP_ERR_NOT_FOUND;
    }

    if (check_ioe_result(ioe->setI2cSleepTime(0), "关闭 I2C 休眠(1)") != ESP_OK ||
        check_ioe_result(ioe->setI2cSleepTime(0), "关闭 I2C 休眠(2)") != ESP_OK) {
        ioe.reset();
        return ESP_FAIL;
    }

    // 先设置方向再写入源工程的安全默认电平：电机和扬声器关闭，其余电源与复位拉高。
    if (configure_ioe_output(IOE_MOTOR_EN, 0) != ESP_OK ||
        configure_ioe_output(IOE_L3B_EN, 1) != ESP_OK ||
        configure_ioe_output(IOE_SPEAKER_PA, 0) != ESP_OK ||
        configure_ioe_output(IOE_TOUCH_RST, 1) != ESP_OK ||
        configure_ioe_output(IOE_DISPLAY_RST, 1) != ESP_OK ||
        configure_ioe_output(IOE_MUX_CTRL, 0) != ESP_OK ||
        configure_ioe_output(IOE_AUDIO_EN, 1) != ESP_OK ||
        check_ioe_result(ioe->setPwmFrequency(5000), "配置电机 PWM 频率") != ESP_OK ||
        check_ioe_result(ioe->setPwmDuty(MOTOR_PWM_CHANNEL, 0, false, true),
                         "关闭电机 PWM") != ESP_OK) {
        ioe.reset();
        return ESP_FAIL;
    }

    gpio_config_t speaker_gpio_config = {
        .pin_bit_mask = 1ULL << SPEAKER_PA_PIN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&speaker_gpio_config), TAG, "配置扬声器 GPIO14 失败");
    ESP_RETURN_ON_ERROR(gpio_set_level(SPEAKER_PA_PIN, 0), TAG, "关闭扬声器 GPIO14 失败");

    // 源工程会反复确认 L3B_EN，避免扩展芯片刚切换高速 I2C 时首写未生效。
    int retry_count = 0;
    while (ioe->digitalRead(IOE_L3B_EN) != 1 && retry_count < 5) {
        vTaskDelay(pdMS_TO_TICKS(80));
        if (ioe_write(IOE_L3B_EN, 1) != ESP_OK) {
            ioe.reset();
            return ESP_FAIL;
        }
        ++retry_count;
    }
    if (ioe->digitalRead(IOE_L3B_EN) != 1) {
        ESP_LOGE(TAG, "L3B_EN 拉高失败");
        ioe.reset();
        return ESP_FAIL;
    }

    const BaseType_t task_created = xTaskCreate(vibration_control_task, "vibration", 4096,
                                                nullptr, 3, &vibration_task_handle);
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "创建振动控制任务失败");
        ioe.reset();
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "M5IOE1 初始化完成，I2C 地址=0x%02X，L3B_EN 重试=%d",
             selected_address, retry_count);
    return ESP_OK;
}

void power_monitor_task(void *)
{
    uint32_t elapsed_since_log_ms = POWER_LOG_PERIOD_MS;
    while (true) {
        if (pmic) {
            uint16_t battery_mv = 0;
            uint16_t vin_mv = 0;
            uint8_t charge_status = 1;

            const bool battery_ok = pmic->readVbat(&battery_mv) == M5PM1_OK;
            const bool vin_ok = pmic->readVin(&vin_mv) == M5PM1_OK;
            const bool charge_ok = pmic->gpioGetInput(PMIC_CHG_STAT, &charge_status) == M5PM1_OK;
            if (battery_ok) {
                update_battery_filter(battery_mv);
            }

            if (elapsed_since_log_ms >= POWER_LOG_PERIOD_MS) {
                uint16_t filtered_mv = 0;
                uint8_t percent = 0;
                {
                    std::lock_guard<std::mutex> lock(state_mutex);
                    filtered_mv = filtered_battery_mv;
                    percent = battery_percent;
                }
                if (battery_ok && vin_ok && charge_ok) {
                    const bool external_power = vin_mv > 4000;
                    const bool actively_charging = external_power && charge_status == 0;
                    ESP_LOGI(TAG,
                             "电源状态: 电池=%umV(%u%%), VIN=%umV, 外部电源=%s, 正在充电=%s",
                             filtered_mv, percent, vin_mv,
                             external_power ? "是" : "否", actively_charging ? "是" : "否");
                } else {
                    ESP_LOGW(TAG, "读取电源状态不完整: VBAT=%s, VIN=%s, CHG_STAT=%s",
                             battery_ok ? "正常" : "失败", vin_ok ? "正常" : "失败",
                             charge_ok ? "正常" : "失败");
                }
                elapsed_since_log_ms = 0;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(POWER_SAMPLE_PERIOD_MS));
        elapsed_since_log_ms += POWER_SAMPLE_PERIOD_MS;
    }
}

void vibration_control_task(void *)
{
    uint8_t current_strength = 0;
    while (true) {
        const TickType_t now = xTaskGetTickCount();
        TickType_t wait_ticks = portMAX_DELAY;
        uint8_t target_strength = 0;

        {
            std::lock_guard<std::mutex> lock(vibration_mutex);
            if (vibration_enabled && now < vibration_end_tick) {
                target_strength = vibration_strength;
                wait_ticks = vibration_end_tick - now;
            } else {
                vibration_enabled = false;
            }
        }

        if (target_strength != current_strength) {
            board_power_set_motor(target_strength);
            current_strength = target_strength;
        }

        // 新命令会通过任务通知立即唤醒；否则在截止时间自动醒来并关闭电机。
        ulTaskNotifyTake(pdTRUE, wait_ticks);
    }
}

}  // namespace

extern "C" esp_err_t board_power_init(void)
{
    if (i2c_bus != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(init_i2c_bus(), TAG, "共享 I2C 初始化失败");

    const esp_err_t pmic_result = init_pmic();
    if (pmic_result != ESP_OK) {
        // PMIC 不控制显示复位，记录错误后仍允许 IOE1 初始化，便于定位单芯片故障。
        ESP_LOGW(TAG, "M5PM1 不可用，电池与充电日志将被禁用");
    }

    ESP_RETURN_ON_ERROR(init_ioe(), TAG, "M5IOE1 初始化失败，不能安全启动屏幕和触摸");

    if (pmic) {
        const BaseType_t created = xTaskCreate(power_monitor_task, "power_monitor", 4096, nullptr, 2, nullptr);
        if (created != pdPASS) {
            ESP_LOGE(TAG, "创建电源监控任务失败");
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

extern "C" i2c_master_bus_handle_t board_power_get_i2c_bus(void)
{
    return i2c_bus;
}

extern "C" bool board_power_pmic_ready(void)
{
    return pmic != nullptr;
}

extern "C" bool board_power_ioe_ready(void)
{
    return ioe != nullptr;
}

extern "C" esp_err_t board_power_reset_display(void)
{
    if (!ioe) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(ioe_write(IOE_DISPLAY_RST, 0), TAG, "拉低屏幕复位失败");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(ioe_write(IOE_DISPLAY_RST, 1), TAG, "释放屏幕复位失败");
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_LOGI(TAG, "已通过 M5IOE1 IO5 复位 CO5300");
    return ESP_OK;
}

extern "C" esp_err_t board_power_reset_touch(void)
{
    if (!ioe) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(ioe_write(IOE_TOUCH_RST, 0), TAG, "拉低触摸复位失败");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(ioe_write(IOE_TOUCH_RST, 1), TAG, "释放触摸复位失败");
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_LOGI(TAG, "已通过 M5IOE1 IO4 复位 CST820");
    return ESP_OK;
}

extern "C" esp_err_t board_power_set_display_power(bool enabled)
{
    if (!ioe) {
        return ESP_ERR_INVALID_STATE;
    }
    return ioe_write(IOE_L3B_EN, enabled ? 1 : 0);
}

extern "C" esp_err_t board_power_set_audio_enabled(bool enabled)
{
    if (!ioe) {
        return ESP_ERR_INVALID_STATE;
    }
    return ioe_write(IOE_AUDIO_EN, enabled ? 1 : 0);
}

extern "C" esp_err_t board_power_set_mux(bool level)
{
    if (!ioe) {
        return ESP_ERR_INVALID_STATE;
    }
    return ioe_write(IOE_MUX_CTRL, level ? 1 : 0);
}

extern "C" esp_err_t board_power_set_motor(uint8_t strength)
{
    if (!ioe) {
        return ESP_ERR_INVALID_STATE;
    }
    strength = std::min<uint8_t>(strength, 100);
    // 源工程把非零强度映射到 25%-100%，避免占空比太低时电机无法起振。
    const uint8_t duty = strength == 0 ? 0 : static_cast<uint8_t>(25 + strength * 75U / 100U);
    return check_ioe_result(ioe->setPwmDuty(MOTOR_PWM_CHANNEL, duty, false, true), "设置电机 PWM");
}

extern "C" esp_err_t board_power_vibrate(uint16_t duration_ms, uint8_t strength)
{
    if (duration_ms == 0 || strength == 0) {
        return board_power_stop_vibration();
    }
    if (!ioe || vibration_task_handle == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    {
        std::lock_guard<std::mutex> lock(vibration_mutex);
        vibration_enabled = true;
        vibration_strength = std::min<uint8_t>(strength, 100);
        vibration_end_tick = xTaskGetTickCount() + pdMS_TO_TICKS(duration_ms);
    }
    xTaskNotifyGive(vibration_task_handle);
    return ESP_OK;
}

extern "C" esp_err_t board_power_stop_vibration(void)
{
    if (!ioe || vibration_task_handle == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    {
        std::lock_guard<std::mutex> lock(vibration_mutex);
        vibration_enabled = false;
        vibration_strength = 0;
    }
    // 先同步关闭 PWM，再通知控制任务更新其缓存，保证调用返回时电机已经停止。
    ESP_RETURN_ON_ERROR(board_power_set_motor(0), TAG, "停止振动失败");
    xTaskNotifyGive(vibration_task_handle);
    return ESP_OK;
}

extern "C" esp_err_t board_power_set_speaker_enabled(bool enabled)
{
    if (!ioe) {
        return ESP_ERR_INVALID_STATE;
    }
    if (enabled) {
        ESP_RETURN_ON_ERROR(ioe_write(IOE_SPEAKER_PA, 1), TAG, "打开 IOE1 扬声器使能失败");
        ESP_RETURN_ON_ERROR(gpio_set_level(SPEAKER_PA_PIN, 1), TAG, "打开 GPIO14 扬声器使能失败");
    } else {
        ESP_RETURN_ON_ERROR(ioe_write(IOE_SPEAKER_PA, 0), TAG, "关闭 IOE1 扬声器使能失败");
        ESP_RETURN_ON_ERROR(gpio_set_level(SPEAKER_PA_PIN, 0), TAG, "关闭 GPIO14 扬声器使能失败");
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    return ESP_OK;
}

extern "C" esp_err_t board_power_get_battery_mv(uint16_t *battery_mv)
{
    if (battery_mv == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!pmic) {
        return ESP_ERR_INVALID_STATE;
    }
    std::lock_guard<std::mutex> lock(state_mutex);
    *battery_mv = filtered_battery_mv;
    return ESP_OK;
}

extern "C" esp_err_t board_power_get_battery_percent(uint8_t *percent)
{
    if (percent == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!pmic) {
        return ESP_ERR_INVALID_STATE;
    }
    std::lock_guard<std::mutex> lock(state_mutex);
    *percent = battery_percent;
    return ESP_OK;
}

extern "C" esp_err_t board_power_get_vin_mv(uint16_t *vin_mv)
{
    if (vin_mv == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!pmic) {
        return ESP_ERR_INVALID_STATE;
    }
    return check_pmic_result(pmic->readVin(vin_mv), "读取 VIN");
}

extern "C" esp_err_t board_power_is_charging(bool strict, bool *charging)
{
    if (charging == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!pmic) {
        return ESP_ERR_INVALID_STATE;
    }

    uint16_t vin_mv = 0;
    uint8_t charge_status = 1;
    if (pmic->readVin(&vin_mv) != M5PM1_OK ||
        pmic->gpioGetInput(PMIC_CHG_STAT, &charge_status) != M5PM1_OK) {
        return ESP_FAIL;
    }
    const bool external_power = vin_mv > 4000;
    *charging = strict ? (external_power && charge_status == 0) : external_power;
    return ESP_OK;
}

extern "C" esp_err_t board_power_get_button_state(bool *pressed)
{
    if (pressed == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!pmic) {
        return ESP_ERR_INVALID_STATE;
    }
    return check_pmic_result(pmic->btnGetState(pressed), "读取电源按键");
}
