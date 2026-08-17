#include "watch_button.h"                            // 引入本模块的配置和启动接口

#include <stdbool.h>                                  // 提供按下状态布尔值
#include <string.h>                                   // 提供 memset() 清理失败状态

#include "esp_log.h"                                 // 提供 GPIO 诊断日志
#include "freertos/FreeRTOS.h"                       // 提供 FreeRTOS 基础类型和时间转换宏
#include "freertos/task.h"                           // 提供按键轮询任务 API

#define WATCH_BUTTON_TASK_STACK_SIZE 3072             // 为独立 GPIO 轮询任务保留稳定栈空间
#define WATCH_BUTTON_TASK_PRIORITY 3                  // 与原 G1 任务保持相同优先级

typedef struct {
    watch_button_config_t config;                     // 保存应用层配置副本，避免调用者栈变量失效
    TaskHandle_t task_handle;                         // 保存任务句柄以阻止重复启动
} watch_button_runtime_t;

static const char *TAG = "WATCH_BUTTON";             // 模块专属串口日志标签
static watch_button_runtime_t s_runtime = {0};        // 本项目当前只需要一个 G1 按键实例

/**
 * @brief 将毫秒转换为至少一个 FreeRTOS Tick 的延时值。
 *
 * @param period_ms 调用者配置的毫秒周期，必须大于零。
 * @return 至少为 1 的 Tick 数，避免任务忙循环。
 * @note 仅在 watch_button 模块内部使用，不访问 GPIO 或 LVGL。
 */
static TickType_t period_to_ticks(uint32_t period_ms)
{
    const TickType_t ticks = pdMS_TO_TICKS(period_ms);
    return ticks == 0 ? 1 : ticks;
}

/**
 * @brief 轮询 GPIO，确认低电平按下和释放后发布一次单击回调。
 *
 * @param user_data 指向模块私有 watch_button_runtime_t 的稳定地址。
 * @return 不返回；任务随系统持续运行。
 * @note 不调用 LVGL；on_click 回调由应用层负责切换到正确的 LVGL 锁。
 */
static void watch_button_task(void *user_data)
{
    watch_button_runtime_t *runtime = user_data;
    const gpio_num_t gpio_num = runtime->config.gpio_num;
    bool raw_pressed = gpio_get_level(gpio_num) == 0;
    bool stable_pressed = raw_pressed;
    TickType_t raw_changed_at = xTaskGetTickCount();

    ESP_LOGI(TAG, "G1 按键任务已启动: GPIO=%d, 原始电平=%d (0=按下)",
             gpio_num, raw_pressed ? 0 : 1);

    while (true) {
        const bool sampled_pressed = gpio_get_level(gpio_num) == 0;
        const TickType_t now = xTaskGetTickCount();

        if (sampled_pressed != raw_pressed) {
            raw_pressed = sampled_pressed;
            raw_changed_at = now;
            ESP_LOGI(TAG, "G1 原始电平变化: %s", raw_pressed ? "按下" : "释放");
        }

        if (raw_pressed != stable_pressed &&
            (now - raw_changed_at) >= period_to_ticks(runtime->config.debounce_period_ms)) {
            stable_pressed = raw_pressed;
            ESP_LOGI(TAG, "G1 去抖确认: %s", stable_pressed ? "按下" : "释放");

            if (!stable_pressed) {
                runtime->config.on_click(runtime->config.user_data);
            }
        }

        vTaskDelay(period_to_ticks(runtime->config.poll_period_ms));
    }
}

/**
 * @brief 配置 GPIO 输入并创建模块私有的按键轮询任务。
 *
 * @param config GPIO、轮询、去抖和单击回调配置，函数会复制该配置。
 * @return ESP_OK 表示配置和任务均已成功创建，否则返回具体 ESP-IDF 错误码。
 * @note 本函数不访问 LVGL；单击回调在任务上下文中运行，应用层必须自行处理 UI 锁。
 */
esp_err_t watch_button_start(const watch_button_config_t *config)
{
    if (config == NULL || config->gpio_num == GPIO_NUM_NC ||
        config->poll_period_ms == 0 || config->debounce_period_ms == 0 ||
        config->on_click == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_runtime.task_handle != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result = gpio_reset_pin(config->gpio_num);
    if (result != ESP_OK) {
        return result;
    }

    result = gpio_set_direction(config->gpio_num, GPIO_MODE_INPUT);
    if (result != ESP_OK) {
        return result;
    }

    result = gpio_set_pull_mode(config->gpio_num, GPIO_PULLUP_ONLY);
    if (result != ESP_OK) {
        return result;
    }

    s_runtime.config = *config;                       // 复制配置，调用者可在函数返回后释放局部变量
    const BaseType_t created = xTaskCreate(
        watch_button_task,
        "watch_button",
        WATCH_BUTTON_TASK_STACK_SIZE,
        &s_runtime,
        WATCH_BUTTON_TASK_PRIORITY,
        &s_runtime.task_handle
    );
    if (created != pdPASS) {
        memset(&s_runtime, 0, sizeof(s_runtime));
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
