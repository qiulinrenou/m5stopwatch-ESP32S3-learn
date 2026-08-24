#include "key.h"                                     // 引入按键模块对外接口

#include <stdbool.h>                                  // 提供按键状态布尔值
#include <stddef.h>                                   // 提供 size_t 类型
#include <stdint.h>                                   // 提供轮询周期使用的 uint32_t 类型

#include "driver/gpio.h"                             // 提供 GPIO 配置和读取 API
#include "esp_log.h"                                 // 提供按键诊断日志
#include "freertos/FreeRTOS.h"                       // 提供 FreeRTOS 基础类型和时间转换宏
#include "freertos/task.h"                           // 提供按键轮询任务 API

#include "watch_board.h"                             // 读取统一管理的 G1、G2 引脚和去抖参数

#define KEY_TASK_STACK_SIZE 3072                      // 为双按键轮询任务保留稳定栈空间
#define KEY_TASK_PRIORITY 3                           // 保持已验证按键任务优先级

typedef struct {
    gpio_num_t gpio_num;                               // 当前按键对应的 ESP32 GPIO 引脚
    const char *name;                                  // 用于串口诊断的按键名称
    key_click_cb_t on_click;                           // 完整单击后调用的应用层回调
    void *user_data;                                   // 交给 on_click 的应用层上下文
    key_click_cb_t on_long_press;                      // 持续按下达到阈值后调用的应用层回调
    void *long_press_user_data;                        // 交给 on_long_press 的应用层上下文
    bool raw_pressed;                                  // 最近一次原始低电平按下状态
    bool stable_pressed;                               // 去抖后确认的按下状态
    TickType_t raw_changed_at;                         // 原始电平最近一次变化的时刻
    TickType_t pressed_at;                             // 去抖确认按下的时刻
    bool long_press_fired;                             // 本次按压是否已经发布长按并抑制单击
} key_runtime_t;

static const char *TAG = "KEY";                      // 使用按键模块专属串口日志标签
static key_runtime_t s_keys[] = {                     // 集中管理本板两枚物理按键的运行状态
    { .gpio_num = WATCH_G1_BUTTON_PIN, .name = "G1" }, // G1 是蓝色 B 键，GPIO1 低电平按下
    { .gpio_num = WATCH_G2_BUTTON_PIN, .name = "G2" }, // G2 是黄色 A 键，GPIO2 低电平按下
};
static TaskHandle_t s_key_task_handle = NULL;         // 记录任务句柄以阻止重复初始化

/**
 * @brief 将毫秒转换为至少一个 FreeRTOS Tick 的延时值。
 *
 * @param period_ms 轮询或去抖使用的毫秒周期，必须大于零。
 * @return 至少为 1 的 Tick 数，避免任务发生忙循环。
 * @note 仅供 key.c 内部使用，不访问 GPIO 或 LVGL。
 */
static TickType_t period_to_ticks(uint32_t period_ms)
{
    const TickType_t ticks = pdMS_TO_TICKS(period_ms); // 调用 FreeRTOS 宏换算任务延时单位
    return ticks == 0 ? 1 : ticks;                     // 保证非常短的周期也至少让出一次调度
}

/**
 * @brief 配置一个物理按键为上拉输入并记录其初始状态。
 *
 * @param key 指向 G1 或 G2 的运行状态。
 * @return ESP_OK 表示 GPIO 已完成配置，否则返回具体 ESP-IDF 错误码。
 * @note 不调用 LVGL；GPIO 编号由 watch_board.h 统一定义。
 */
static esp_err_t key_configure(key_runtime_t *key)
{
    esp_err_t result = gpio_reset_pin(key->gpio_num);  // 清除按键 GPIO 可能遗留的复用配置
    if (result != ESP_OK) {
        return result;
    }

    result = gpio_set_direction(key->gpio_num, GPIO_MODE_INPUT); // 将按键 GPIO 配置为输入模式
    if (result != ESP_OK) {
        return result;
    }

    result = gpio_set_pull_mode(key->gpio_num, GPIO_PULLUP_ONLY); // 与低电平按下的实体按键电路配套
    if (result != ESP_OK) {
        return result;
    }

    key->raw_pressed = gpio_get_level(key->gpio_num) == 0; // 记录低电平按下的初始原始状态
    key->stable_pressed = key->raw_pressed;            // 初始稳定状态与第一次采样保持一致
    key->raw_changed_at = xTaskGetTickCount();          // 从初始化完成时刻开始计算后续去抖
    key->pressed_at = key->raw_changed_at;              // 若启动时已按住，不从未定义时刻计算长按
    key->long_press_fired = key->stable_pressed;        // 启动时按住只等待释放，避免误触发长按或单击
    return ESP_OK;
}

/**
 * @brief 对一个按键执行一次采样、去抖、长按和释放沿回调。
 *
 * @param key 指向需要处理的 G1 或 G2 运行状态。
 * @param now 本轮轮询的 FreeRTOS Tick 时刻。
 * @return 无返回值。
 * @note 长按一旦触发，本次释放沿不会再发布单击；本函数不调用 LVGL。
 */
static void key_poll(key_runtime_t *key, TickType_t now)
{
    const bool sampled_pressed = gpio_get_level(key->gpio_num) == 0; // 采样当前低电平按下状态

    if (sampled_pressed != key->raw_pressed) {         // 原始电平变化后重新开始去抖计时
        key->raw_pressed = sampled_pressed;
        key->raw_changed_at = now;
        ESP_LOGI(TAG, "%s 原始电平变化: %s", key->name,
                 key->raw_pressed ? "按下" : "释放");
    }

    if (key->raw_pressed != key->stable_pressed &&
        (now - key->raw_changed_at) >= period_to_ticks(WATCH_BUTTON_DEBOUNCE_MS)) {
        key->stable_pressed = key->raw_pressed;        // 电平稳定达到阈值后确认新的按键状态
        ESP_LOGI(TAG, "%s 去抖确认: %s", key->name,
                 key->stable_pressed ? "按下" : "释放");

        if (key->stable_pressed) {
            key->pressed_at = now;                     // 从去抖确认按下时开始计算长按时长
            key->long_press_fired = false;             // 新一轮按压允许触发一次长按
        } else {
            if (!key->long_press_fired && key->on_click != NULL) {
                key->on_click(key->user_data);         // 短按释放后才通知应用层
            }
            key->long_press_fired = false;             // 释放后复位，等待下一次按压
        }
    }

    if (key->stable_pressed && !key->long_press_fired &&
        key->on_long_press != NULL &&
        (now - key->pressed_at) >= period_to_ticks(WATCH_BUTTON_LONG_PRESS_MS)) {
        key->long_press_fired = true;                  // 先置位，保证一个按压周期只发布一次
        ESP_LOGI(TAG, "%s 长按确认: %u ms", key->name,
                 (unsigned int)WATCH_BUTTON_LONG_PRESS_MS);
        key->on_long_press(key->long_press_user_data); // 应用层负责把动作送入自己的任务队列
    }
}

/**
 * @brief 周期性轮询 G1、G2 并将确认后的单击或长按发布到各自回调。
 *
 * @param user_data 未使用；两枚按键状态保存在本文件的静态数组中。
 * @return 不返回；任务随系统持续运行。
 * @note 不调用 LVGL；所有 UI 行为必须由回调经 watch_lvgl_run() 执行。
 */
static void key_task(void *user_data)
{
    (void)user_data;                                   // 当前任务不需要额外上下文参数

    for (size_t index = 0; index < sizeof(s_keys) / sizeof(s_keys[0]); ++index) {
        ESP_LOGI(TAG, "%s 按键任务已启动: GPIO=%d, 原始电平=%d (0=按下)",
                 s_keys[index].name, s_keys[index].gpio_num,
                 s_keys[index].raw_pressed ? 0 : 1);
    }

    while (true) {
        const TickType_t now = xTaskGetTickCount();    // 同一轮使用相同时间基准处理两枚按键

        for (size_t index = 0; index < sizeof(s_keys) / sizeof(s_keys[0]); ++index) {
            key_poll(&s_keys[index], now);              // 分别执行 G1、G2 的采样和去抖
        }

        vTaskDelay(period_to_ticks(WATCH_BUTTON_POLL_MS)); // 按统一轮询周期让出 FreeRTOS 调度
    }
}

/**
 * @brief 配置 G1、G2 的上拉输入并创建统一单击与长按轮询任务。
 *
 * @param config 两枚按键的应用层回调配置，至少登记一个单击或长按回调。
 * @return ESP_OK 表示 GPIO 和任务均已成功创建，否则返回具体 ESP-IDF 错误码。
 * @note 本函数不访问 LVGL；G2 未登记回调时只完成硬件采样，不会影响任何页面或 UI。
 */
esp_err_t key_init(const key_config_t *config)
{
    if (config == NULL ||
        (config->g1_on_click == NULL && config->g2_on_click == NULL &&
         config->g2_on_long_press == NULL)) {           // 至少登记一个按键回调才创建轮询任务
        return ESP_ERR_INVALID_ARG;
    }

    if (s_key_task_handle != NULL) {                   // 两枚物理按键共用一个任务，只能初始化一次
        return ESP_ERR_INVALID_STATE;
    }

    s_keys[0].on_click = config->g1_on_click;          // 登记蓝色 G1 的菜单切换回调
    s_keys[0].user_data = config->g1_user_data;        // 登记蓝色 G1 回调上下文
    s_keys[1].on_click = config->g2_on_click;          // 登记黄色 G2 的预留回调，可为 NULL
    s_keys[1].user_data = config->g2_user_data;        // 保存黄色 G2 的预留回调上下文
    s_keys[1].on_long_press = config->g2_on_long_press; // 登记黄色 G2 长按回调，可为 NULL
    s_keys[1].long_press_user_data = config->g2_long_press_user_data; // 保存长按上下文

    for (size_t index = 0; index < sizeof(s_keys) / sizeof(s_keys[0]); ++index) {
        const esp_err_t result = key_configure(&s_keys[index]); // 逐个配置两枚按键的 GPIO 输入和上拉
        if (result != ESP_OK) {
            return result;
        }
    }

    const BaseType_t task_created = xTaskCreate(
        key_task,                                      // 由 watch_board 私有任务持续轮询两枚按键
        "key",                                         // 使用简短任务名便于 FreeRTOS 诊断
        KEY_TASK_STACK_SIZE,                            // 使用本模块定义的任务栈空间
        NULL,                                           // 按键状态已由静态数组持有，无需任务参数
        KEY_TASK_PRIORITY,                              // 使用已验证的按键任务优先级
        &s_key_task_handle                              // 保存句柄以防止重复创建任务
    );
    if (task_created != pdPASS) {
        s_key_task_handle = NULL;                       // 创建失败时允许调用者处理后重新初始化
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
