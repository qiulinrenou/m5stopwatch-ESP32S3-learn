#include "watch_rtc.h"

#include <stdbool.h>                                 // 提供 bool 类型
#include <stddef.h>                                  // 提供 size_t 类型

#include "esp_log.h"                                 // 提供 RTC 自检日志
#include "freertos/FreeRTOS.h"                      // 提供 FreeRTOS 时间换算宏
#include "freertos/task.h"                          // 提供 vTaskDelay()

#define WATCH_RTC_I2C_ADDRESS       0x32             // RX8130 的 7 位 I2C 地址
#define WATCH_RTC_I2C_SPEED_HZ      100000           // 与板级共享总线的基础速率一致
#define WATCH_RTC_I2C_TIMEOUT_MS    100              // RTC 单次事务超时时间
#define WATCH_RTC_SELF_TEST_MS      1100             // 连续读取自检的等待时间

#define RX8130_REG_SECOND           0x10             // 秒寄存器起始地址
#define RX8130_REG_FLAG             0x1D             // 中断和掉电标志寄存器
#define RX8130_REG_CONTROL_0        0x1E             // 时钟停止和中断控制寄存器
#define RX8130_REG_CONTROL_1        0x1F             // 后备电池充电配置寄存器

#define RX8130_FLAG_VLF             (1U << 1)         // 电压下降/振荡停止标志
#define RX8130_CONTROL_0_STOP       (1U << 6)         // 修改日期时间前暂停时钟
#define RX8130_BACKUP_CHARGE_MASK   0x30             // 原厂配置使用的后备电池充电位

#define RX8130_TIME_REGISTER_COUNT  7                // 秒到年份共 7 个连续寄存器

static const char *TAG = "WATCH_RTC";                // 当前模块的日志标签
static i2c_master_dev_handle_t s_rtc_device = NULL;  // 保存唯一的 RX8130 设备句柄

/**
 * @brief 读取一个 RX8130 寄存器。
 */
static esp_err_t read_register(uint8_t address, uint8_t *value)
{
    if (s_rtc_device == NULL || value == NULL) {     // 防止在初始化前访问设备
        return ESP_ERR_INVALID_STATE;
    }

    return i2c_master_transmit_receive(
        s_rtc_device,
        &address,
        sizeof(address),
        value,
        sizeof(*value),
        WATCH_RTC_I2C_TIMEOUT_MS
    );
}

/**
 * @brief 从指定地址开始连续读取 RX8130 寄存器。
 */
static esp_err_t read_registers(
    uint8_t start_address,
    uint8_t *data,
    size_t length)
{
    if (s_rtc_device == NULL || data == NULL || length == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    return i2c_master_transmit_receive(
        s_rtc_device,
        &start_address,
        sizeof(start_address),
        data,
        length,
        WATCH_RTC_I2C_TIMEOUT_MS
    );
}

/**
 * @brief 写入一个 RX8130 寄存器。
 */
static esp_err_t write_register(uint8_t address, uint8_t value)
{
    if (s_rtc_device == NULL) {                      // RTC 未初始化时禁止写寄存器
        return ESP_ERR_INVALID_STATE;
    }

    const uint8_t command[2] = {address, value};     // RX8130 写事务为地址后跟数据

    return i2c_master_transmit(
        s_rtc_device,
        command,
        sizeof(command),
        WATCH_RTC_I2C_TIMEOUT_MS
    );
}

/**
 * @brief 检查一个字节是否为合法的 BCD 编码。
 */
static bool is_valid_bcd(uint8_t value)
{
    return ((value & 0x0FU) <= 9U) && (((value >> 4) & 0x0FU) <= 9U);
}

/**
 * @brief 将合法的 BCD 编码转换为十进制。
 */
static uint8_t bcd_to_decimal(uint8_t value)
{
    return (uint8_t)(((value >> 4) * 10U) + (value & 0x0FU));
}

/**
 * @brief 将合法的十进制日期时间字段转换为 BCD。
 */
static uint8_t decimal_to_bcd(uint8_t value)
{
    return (uint8_t)(((value / 10U) << 4) | (value % 10U));
}

/**
 * @brief 判断指定年份是否为闰年。
 */
static bool is_leap_year(uint16_t year)
{
    return ((year % 4U) == 0U && (year % 100U) != 0U) ||
           ((year % 400U) == 0U);
}

/**
 * @brief 返回指定月份允许的最大日期。
 */
static uint8_t days_in_month(uint16_t year, uint8_t month)
{
    static const uint8_t days[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
    };

    if (month == 0 || month > 12) {                  // 非法月份没有有效日期
        return 0;
    }

    if (month == 2 && is_leap_year(year)) {          // 闰年二月有 29 天
        return 29;
    }

    return days[month - 1U];
}

/**
 * @brief 根据公历日期计算星期，返回 0-6，其中 0 表示星期日。
 */
static uint8_t calculate_weekday(
    uint16_t year,
    uint8_t month,
    uint8_t day)
{
    static const uint8_t month_offsets[] = {
        0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4,
    };

    uint16_t adjusted_year = year;
    if (month < 3U) {
        --adjusted_year;
    }

    return (uint8_t)(
        (adjusted_year +
         adjusted_year / 4U -
         adjusted_year / 100U +
         adjusted_year / 400U +
         month_offsets[month - 1U] +
         day) % 7U
    );
}

/**
 * @brief 将日期时间整体移动指定分钟数，并同步调整日期和星期。
 */
static void shift_datetime_minutes(
    watch_rtc_datetime_t *datetime,
    int16_t offset_minutes)
{
    int32_t shifted_minutes =
        (int32_t)datetime->hour * 60 +
        datetime->minute +
        offset_minutes;

    while (shifted_minutes < 0) {
        shifted_minutes += 24 * 60;
        datetime->weekday =
            (uint8_t)((datetime->weekday + 6U) % 7U);

        if (datetime->day > 1U) {
            --datetime->day;
        } else {
            if (datetime->month > 1U) {
                --datetime->month;
            } else {
                datetime->month = 12U;
                --datetime->year;
            }
            datetime->day =
                days_in_month(datetime->year, datetime->month);
        }
    }

    while (shifted_minutes >= 24 * 60) {
        shifted_minutes -= 24 * 60;
        datetime->weekday =
            (uint8_t)((datetime->weekday + 1U) % 7U);

        if (datetime->day <
            days_in_month(datetime->year, datetime->month)) {
            ++datetime->day;
        } else {
            datetime->day = 1U;
            if (datetime->month < 12U) {
                ++datetime->month;
            } else {
                datetime->month = 1U;
                ++datetime->year;
            }
        }
    }

    datetime->hour = (uint8_t)(shifted_minutes / 60);
    datetime->minute = (uint8_t)(shifted_minutes % 60);
}

/**
 * @brief 解析 C 编译器提供的 __DATE__ 和 __TIME__ 本地时间。
 */
static esp_err_t get_build_datetime(
    watch_rtc_datetime_t *datetime)
{
    static const char *const month_names[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    };
    const char *const build_date = __DATE__;          // 格式固定为 "Mmm dd yyyy"
    const char *const build_time = __TIME__;          // 格式固定为 "hh:mm:ss"
    uint8_t month = 0;

    for (uint8_t index = 0; index < 12U; ++index) {
        if (build_date[0] == month_names[index][0] &&
            build_date[1] == month_names[index][1] &&
            build_date[2] == month_names[index][2]) {
            month = (uint8_t)(index + 1U);
            break;
        }
    }
    if (month == 0U) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint8_t day_tens =
        (build_date[4] == ' ') ? 0U : (uint8_t)(build_date[4] - '0');
    watch_rtc_datetime_t parsed = {
        .year = (uint16_t)(
            (build_date[7] - '0') * 1000U +
            (build_date[8] - '0') * 100U +
            (build_date[9] - '0') * 10U +
            (build_date[10] - '0')
        ),
        .month = month,
        .day = (uint8_t)(day_tens * 10U + (build_date[5] - '0')),
        .weekday = 0,
        .hour = (uint8_t)((build_time[0] - '0') * 10U +
                          (build_time[1] - '0')),
        .minute = (uint8_t)((build_time[3] - '0') * 10U +
                            (build_time[4] - '0')),
        .second = (uint8_t)((build_time[6] - '0') * 10U +
                            (build_time[7] - '0')),
    };

    if (parsed.year < 2000U || parsed.year > 2099U ||
        parsed.day < 1U ||
        parsed.day > days_in_month(parsed.year, parsed.month) ||
        parsed.hour > 23U ||
        parsed.minute > 59U ||
        parsed.second > 59U) {
        return ESP_ERR_INVALID_STATE;
    }

    parsed.weekday =
        calculate_weekday(parsed.year, parsed.month, parsed.day);
    *datetime = parsed;
    return ESP_OK;
}

/**
 * @brief 逐字段比较两份日期时间，避免读取结构体填充字节。
 */
static bool datetime_is_equal(
    const watch_rtc_datetime_t *left,
    const watch_rtc_datetime_t *right)
{
    return left->year == right->year &&
           left->month == right->month &&
           left->day == right->day &&
           left->weekday == right->weekday &&
           left->hour == right->hour &&
           left->minute == right->minute &&
           left->second == right->second;
}

/**
 * @brief 解码并校验 RX8130 的七个日期时间寄存器。
 */
static esp_err_t decode_datetime(
    const uint8_t registers[RX8130_TIME_REGISTER_COUNT],
    watch_rtc_datetime_t *datetime)
{
    const uint8_t second_bcd = registers[0] & 0x7FU;
    const uint8_t minute_bcd = registers[1] & 0x7FU;
    const uint8_t hour_bcd = registers[2] & 0x3FU;
    const uint8_t weekday_bcd = registers[3] & 0x07U;
    const uint8_t day_bcd = registers[4] & 0x3FU;
    const uint8_t month_bcd = registers[5] & 0x1FU;
    const uint8_t year_bcd = registers[6];

    if (!is_valid_bcd(second_bcd) ||                 // 拒绝损坏的 BCD 字段
        !is_valid_bcd(minute_bcd) ||
        !is_valid_bcd(hour_bcd) ||
        !is_valid_bcd(weekday_bcd) ||
        !is_valid_bcd(day_bcd) ||
        !is_valid_bcd(month_bcd) ||
        !is_valid_bcd(year_bcd)) {
        return ESP_ERR_INVALID_STATE;
    }

    watch_rtc_datetime_t decoded = {
        .year = (uint16_t)(2000U + bcd_to_decimal(year_bcd)),
        .month = bcd_to_decimal(month_bcd),
        .day = bcd_to_decimal(day_bcd),
        .weekday = bcd_to_decimal(weekday_bcd),
        .hour = bcd_to_decimal(hour_bcd),
        .minute = bcd_to_decimal(minute_bcd),
        .second = bcd_to_decimal(second_bcd),
    };

    const uint8_t maximum_day =
        days_in_month(decoded.year, decoded.month);  // 按年份和月份校验日期上限

    if (decoded.second > 59 ||
        decoded.minute > 59 ||
        decoded.hour > 23 ||
        decoded.weekday > 6 ||
        decoded.month < 1 ||
        decoded.month > 12 ||
        decoded.day < 1 ||
        decoded.day > maximum_day) {
        return ESP_ERR_INVALID_STATE;
    }

    *datetime = decoded;                             // 所有字段校验通过后再写入输出
    return ESP_OK;
}

/**
 * @brief 直接读取并校验日期时间寄存器，不检查 VLF。
 *
 * 该函数只供初始化自检使用，使初始化流程能够在 VLF 置位时判断寄存器
 * 内容是否合法、时钟是否仍在运行。普通业务读取仍由 watch_rtc_get_datetime()
 * 检查 VLF，避免未经自检就使用不可信时间。
 */
static esp_err_t read_datetime_for_self_test(
    watch_rtc_datetime_t *datetime)
{
    uint8_t registers[RX8130_TIME_REGISTER_COUNT] = {0};
    esp_err_t err = read_registers(
        RX8130_REG_SECOND,
        registers,
        sizeof(registers)
    );
    if (err != ESP_OK) {
        return err;
    }

    return decode_datetime(registers, datetime);     // 仍严格校验 BCD 和日期范围
}

/**
 * @brief 将一份已校验的 UTC 日期时间写入 RX8130。
 */
static esp_err_t write_datetime(
    const watch_rtc_datetime_t *datetime)
{
    uint8_t control_0 = 0;
    esp_err_t err = read_register(RX8130_REG_CONTROL_0, &control_0);
    if (err != ESP_OK) {
        return err;
    }

    err = write_register(
        RX8130_REG_CONTROL_0,
        (uint8_t)(control_0 | RX8130_CONTROL_0_STOP)
    );                                               // 写日期前暂停 RTC 计数
    if (err != ESP_OK) {
        return err;
    }

    const uint8_t command[RX8130_TIME_REGISTER_COUNT + 1U] = {
        RX8130_REG_SECOND,
        decimal_to_bcd(datetime->second),
        decimal_to_bcd(datetime->minute),
        decimal_to_bcd(datetime->hour),
        decimal_to_bcd(datetime->weekday),
        decimal_to_bcd(datetime->day),
        decimal_to_bcd(datetime->month),
        decimal_to_bcd((uint8_t)(datetime->year % 100U)),
    };

    err = i2c_master_transmit(
        s_rtc_device,
        command,
        sizeof(command),
        WATCH_RTC_I2C_TIMEOUT_MS
    );

    const esp_err_t restart_err = write_register(
        RX8130_REG_CONTROL_0,
        (uint8_t)(control_0 & (uint8_t)~RX8130_CONTROL_0_STOP)
    );                                               // 无论写入是否成功都尝试恢复计时

    if (err != ESP_OK) {
        return err;
    }
    return restart_err;
}

esp_err_t watch_rtc_get_datetime(watch_rtc_datetime_t *datetime)
{
    if (datetime == NULL) {                          // 输出地址必须有效
        return ESP_ERR_INVALID_ARG;
    }
    if (s_rtc_device == NULL) {                      // RTC 设备尚未初始化
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t flag = 0;
    esp_err_t err = read_register(RX8130_REG_FLAG, &flag);
    if (err != ESP_OK) {                             // I2C 失败时保留具体错误码
        return err;
    }
    if ((flag & RX8130_FLAG_VLF) != 0U) {            // VLF 表示时间可能因掉电而失效
        return ESP_ERR_INVALID_STATE;
    }

    return read_datetime_for_self_test(datetime);    // VLF 通过后读取并校验日期时间
}

esp_err_t watch_rtc_init(i2c_master_bus_handle_t i2c_bus)
{
    if (i2c_bus == NULL) {                           // RX8130 必须复用已创建的共享总线
        return ESP_ERR_INVALID_ARG;
    }
    if (s_rtc_device != NULL) {                      // 防止重复向总线添加同一个设备
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = i2c_master_probe(
        i2c_bus,
        WATCH_RTC_I2C_ADDRESS,
        WATCH_RTC_I2C_TIMEOUT_MS
    );
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RX8130 探测失败，地址=0x%02X: %s",
                 WATCH_RTC_I2C_ADDRESS, esp_err_to_name(err));
        return err;
    }

    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = WATCH_RTC_I2C_ADDRESS,
        .scl_speed_hz = WATCH_RTC_I2C_SPEED_HZ,
        .scl_wait_us = 0,
        .flags = {
            .disable_ack_check = false,
        },
    };

    err = i2c_master_bus_add_device(
        i2c_bus,
        &device_config,
        &s_rtc_device
    );
    if (err != ESP_OK) {
        s_rtc_device = NULL;
        ESP_LOGE(TAG, "添加 RX8130 设备失败: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t control_1 = 0;
    err = read_register(RX8130_REG_CONTROL_1, &control_1);
    if (err == ESP_OK) {                             // 保留原有控制位，只使能后备电池充电
        err = write_register(
            RX8130_REG_CONTROL_1,
            (uint8_t)(control_1 | RX8130_BACKUP_CHARGE_MASK)
        );
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "配置 RX8130 后备电池失败: %s", esp_err_to_name(err));
        return err;
    }

    watch_rtc_datetime_t first = {0};
    watch_rtc_datetime_t second = {0};

    err = read_datetime_for_self_test(&first);       // 即使 VLF 置位，也先验证寄存器内容
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RX8130 日期时间寄存器无效，需要重新校时");
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(WATCH_RTC_SELF_TEST_MS));  // 等待超过一秒确认晶振正在计时

    err = read_datetime_for_self_test(&second);      // 第二次读取确认晶振和秒计数正常
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RX8130 第二次时间读取失败: %s", esp_err_to_name(err));
        return err;
    }

    if (datetime_is_equal(&first, &second)) {         // 所有字段完全不变表示时钟没有前进
        ESP_LOGW(TAG, "RX8130 时间未前进，当前时间不可信");
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t flag = 0;
    err = read_register(RX8130_REG_FLAG, &flag);     // 自检通过后再处理历史掉电标志
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "读取 RX8130 标志寄存器失败: %s", esp_err_to_name(err));
        return err;
    }

    if ((flag & RX8130_FLAG_VLF) != 0U) {
        err = write_register(
            RX8130_REG_FLAG,
            (uint8_t)(flag & (uint8_t)~RX8130_FLAG_VLF)
        );                                           // 只清除 VLF，保留其他已置位的中断标志
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "清除 RX8130 VLF 失败: %s", esp_err_to_name(err));
            return err;
        }

        uint8_t verified_flag = 0;
        err = read_register(RX8130_REG_FLAG, &verified_flag);
        if (err != ESP_OK ||
            (verified_flag & RX8130_FLAG_VLF) != 0U) {
            ESP_LOGW(TAG, "RX8130 VLF 清除后校验失败");
            return (err == ESP_OK) ? ESP_ERR_INVALID_STATE : err;
        }

        ESP_LOGW(TAG, "RX8130 日期合法且时钟正在运行，已清除旧 VLF；请核对实际时间");
    }

    ESP_LOGI(TAG, "RX8130 有效时间: %04u-%02u-%02u %02u:%02u:%02u",
             (unsigned int)second.year,
             (unsigned int)second.month,
             (unsigned int)second.day,
             (unsigned int)second.hour,
             (unsigned int)second.minute,
             (unsigned int)second.second);
    return ESP_OK;
}

esp_err_t watch_rtc_restore_from_build_time(
    int16_t local_timezone_offset_minutes)
{
    if (s_rtc_device == NULL ||
        local_timezone_offset_minutes < (-12 * 60) ||
        local_timezone_offset_minutes > (14 * 60)) {
        return ESP_ERR_INVALID_ARG;
    }

    watch_rtc_datetime_t build_datetime = {0};
    esp_err_t err = get_build_datetime(&build_datetime);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "无法解析固件编译时间");
        return err;
    }

    shift_datetime_minutes(
        &build_datetime,
        (int16_t)-local_timezone_offset_minutes
    );                                               // 本地编译时间转换为 RX8130 使用的 UTC

    err = write_datetime(&build_datetime);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "写入 RX8130 编译时间失败: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t flag = 0;
    err = read_register(RX8130_REG_FLAG, &flag);
    if (err == ESP_OK) {
        err = write_register(
            RX8130_REG_FLAG,
            (uint8_t)(flag & (uint8_t)~RX8130_FLAG_VLF)
        );                                           // 时间写入成功后才清除 VLF
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "恢复后清除 RX8130 VLF 失败: %s", esp_err_to_name(err));
        return err;
    }

    watch_rtc_datetime_t first = {0};
    watch_rtc_datetime_t second = {0};
    err = watch_rtc_get_datetime(&first);
    if (err != ESP_OK) {
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(WATCH_RTC_SELF_TEST_MS));
    err = watch_rtc_get_datetime(&second);
    if (err != ESP_OK || datetime_is_equal(&first, &second)) {
        ESP_LOGE(TAG, "RX8130 写入编译时间后未正常走时");
        return (err == ESP_OK) ? ESP_ERR_INVALID_STATE : err;
    }

    ESP_LOGW(TAG,
             "RX8130 已使用固件编译时间恢复为 UTC %04u-%02u-%02u %02u:%02u:%02u",
             (unsigned int)second.year,
             (unsigned int)second.month,
             (unsigned int)second.day,
             (unsigned int)second.hour,
             (unsigned int)second.minute,
             (unsigned int)second.second);
    ESP_LOGW(TAG, "编译时间是近似初值，烧录后请与手机时间核对");
    return ESP_OK;
}
