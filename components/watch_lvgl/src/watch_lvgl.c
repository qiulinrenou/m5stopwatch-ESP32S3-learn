#include "watch_lvgl.h"

#include <string.h>                               // 提供 memset()

#include "esp_lv_adapter.h"                       // 提供 LVGL adapter 生命周期和锁
#include "esp_log.h"                              // 提供日志接口

    static const char *TAG = "WATCH_LVGL";           // 当前模块日志标签

    typedef struct {
        lv_display_t *display;                        // 保存唯一的 LVGL Display
        uint16_t horizontal_alignment;                // 刷新区域横向对齐像素数
        uint16_t vertical_alignment;                  // 刷新区域纵向对齐像素数
        bool started;                                 // LVGL 是否已启动
    } watch_lvgl_runtime_t;

    static watch_lvgl_runtime_t s_runtime = {0};      // 保存模块私有运行状态

/**
 * @brief 将刷新区域向外扩展到配置的像素边界。
 *
 * @param area LVGL 准备刷新的区域。
 * @param user_data 指向 watch_lvgl_runtime_t。
 *
 * @note 由 LVGL adapter 在其工作上下文中调用。
 */
    static void area_rounder_cb(
        lv_area_t *area,
        void *user_data
    )
    {
        watch_lvgl_runtime_t *runtime = user_data;          // 取得持久化运行配置

        if (area == NULL || runtime == NULL) {              // 防止使用无效回调参数
            return;                                         // 参数无效时不修改刷新区域
        }

        if(runtime->horizontal_alignment > 1) {                 // 屏幕要求横向区域对齐
            int32_t align = runtime->horizontal_alignment;                 // 取得对齐像素数
            area->x1 = (area->x1 / align) * align;                   // 起点向下对齐
            area->x2 = ((area->x2 + align) / align) * align - 1; // 终点向上对齐
        }

        if(runtime->vertical_alignment > 1) {                   // 屏幕要求纵向区域对齐
            int32_t align = runtime->vertical_alignment;                     // 取得对齐像素数
            area->y1 = (area->y1 / align) * align;                       // 起点向下对齐
            area->y2 = ((area->y2 + align) / align) * align - 1;     // 终点向上对齐
        }
    }

/**
 * @brief 检查 watch_lvgl_start() 的配置。
 *
 * @param config 待检查的启动配置。
 * @return ESP_OK 表示配置有效，否则返回 ESP_ERR_INVALID_ARG。
 */
    static esp_err_t validate_config(const watch_lvgl_config_t *config)
    {
         if (config == NULL ||                         // 配置地址必须有效
            config->panel_io == NULL ||               // SPI/QSPI 显示必须提供 Panel IO
            config->panel == NULL ||                  // 必须提供显示面板句柄
            config->touch == NULL ||                  // 当前手表必须提供触摸句柄
            config->horizontal_resolution == 0 ||     // 横向分辨率不能为零
            config->vertical_resolution == 0 ||       // 纵向分辨率不能为零
            config->horizontal_alignment == 0 ||      // 横向对齐值不能为零
            config->vertical_alignment == 0)        // 纵向对齐值不能为零
        {       
            return ESP_ERR_INVALID_ARG;
        }

        if ((config->horizontal_resolution %
         config->horizontal_alignment) != 0 ||    // 防止刷新区域越过屏幕右侧
        (config->vertical_resolution %
         config->vertical_alignment) != 0)        // 防止刷新区域越过屏幕底部
        {     
        return ESP_ERR_INVALID_ARG;
        }

    if (config->rotation != WATCH_LVGL_ROTATION_0 &&
        config->rotation != WATCH_LVGL_ROTATION_90 &&
        config->rotation != WATCH_LVGL_ROTATION_180 &&
        config->rotation != WATCH_LVGL_ROTATION_270) 
        {
        return ESP_ERR_INVALID_ARG;                // 拒绝 adapter 不支持的旋转角度
        }

    return ESP_OK;
    }
    
/**
 * @brief 初始化并启动当前设备的 LVGL 运行时。
 */
    esp_err_t watch_lvgl_start(
        const watch_lvgl_config_t *config,
        watch_lvgl_ui_init_cb_t ui_init_cb)
    {
        esp_err_t err = validate_config(config);        // 检查硬件句柄和显示参数
        if (err != ESP_OK || ui_init_cb == NULL) {      // 初始 UI 回调也必须有效
            return ESP_ERR_INVALID_ARG;
        }

        if (s_runtime.started ||
            esp_lv_adapter_is_initialized()) {          // 防止重复初始化 adapter
            return ESP_ERR_INVALID_STATE;
        }

        s_runtime.horizontal_alignment =
            config->horizontal_alignment;               // 保存回调长期使用的横向对齐值
        s_runtime.vertical_alignment =
            config->vertical_alignment;                 // 保存回调长期使用的纵向对齐值

        esp_lv_adapter_config_t adapter_config =
            ESP_LV_ADAPTER_DEFAULT_CONFIG();            // 取得 adapter 默认任务配置
        adapter_config.stack_in_psram =
            config->use_psram;                          // 根据配置选择任务栈内存

        err = esp_lv_adapter_init(&adapter_config);      // 初始化 LVGL 和 adapter 互斥锁
        if (err != ESP_OK) {
            memset(&s_runtime, 0, sizeof(s_runtime));    // 初始化失败时清空私有状态
            return err;
        }

        bool locked = false;                            // 记录失败清理时是否需要解锁

        err = esp_lv_adapter_lock(-1);                   // 注册 LVGL 对象前获取统一互斥锁
        if (err != ESP_OK) {
            goto fail;
        }
        locked = true;

        esp_lv_adapter_display_config_t display_config; // 保存 adapter 显示注册配置

        if (config->use_psram) {                        // 有 PSRAM 时使用双全屏缓冲
            display_config =
                ESP_LV_ADAPTER_DISPLAY_SPI_WITH_PSRAM_DEFAULT_CONFIG(
                    config->panel,
                    config->panel_io,
                    config->horizontal_resolution,
                    config->vertical_resolution,
                    (esp_lv_adapter_rotation_t)config->rotation
                );
        } else {                                        // 无 PSRAM 时使用较小的内部缓冲
            display_config =
                ESP_LV_ADAPTER_DISPLAY_SPI_WITHOUT_PSRAM_DEFAULT_CONFIG(
                    config->panel,
                    config->panel_io,
                    config->horizontal_resolution,
                    config->vertical_resolution,
                    (esp_lv_adapter_rotation_t)config->rotation
                );
        }

        s_runtime.display =
            esp_lv_adapter_register_display(            // 在锁内创建 LVGL Display
                &display_config
            );

        if (s_runtime.display == NULL) {                 // Display 注册失败没有具体错误码
            err = ESP_FAIL;
            goto fail;
        }

        err = esp_lv_adapter_set_area_rounder_cb(        // 注册通用刷新区域对齐回调
            s_runtime.display,
            area_rounder_cb,
            &s_runtime
        );
        if (err != ESP_OK) {
            goto fail;
        }

        esp_lv_adapter_touch_config_t touch_config =
            ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(
                s_runtime.display,
                config->touch
            );

        lv_indev_t *input_device =
            esp_lv_adapter_register_touch(               // 在锁内创建 LVGL 输入设备
                &touch_config
            );

        if (input_device == NULL) {                      // Touch 注册失败没有具体错误码
            err = ESP_FAIL;
            goto fail;
        }

        ui_init_cb(s_runtime.display);                   // 在锁内创建全部初始 UI 对象

        esp_lv_adapter_unlock();                         // 初始 UI 完整后释放互斥锁
        locked = false;

        err = esp_lv_adapter_start();                    // 解锁后启动 LVGL 工作任务
        if (err != ESP_OK) {
            goto fail;
        }

        s_runtime.started = true;                        // 所有启动步骤成功后更新状态
        ESP_LOGI(TAG, "LVGL runtime started");           // 记录运行时启动成功
        return ESP_OK;

    fail:
        if (locked) {                                    // 只在确实持锁时执行解锁
            esp_lv_adapter_unlock();
        }

        (void)esp_lv_adapter_deinit();                   // 回收已创建的 adapter 和 LVGL 资源
        memset(&s_runtime, 0, sizeof(s_runtime));        // 清空失效的 Display 和状态
        return err;
    }
   
/**
 * @brief 在 LVGL adapter 锁内同步执行一次 UI 操作。
 */
    esp_err_t watch_lvgl_run(
        watch_lvgl_action_t action,
        void *user_data
    )
    {
        if (action == NULL) {                            // 回调地址必须有效
            return ESP_ERR_INVALID_ARG;
        }

        if (!s_runtime.started ||                        
            s_runtime.display == NULL) {                  // adapter 未启动时不能操作 UI
            return ESP_ERR_INVALID_STATE;
        }

        esp_err_t err = esp_lv_adapter_lock(-1);          // 等待取得 adapter 的统一互斥锁
        if (err != ESP_OK) {
            return err;
        }

        action(s_runtime.display, user_data);             // 在锁内同步执行调用者的 UI 操作

        esp_lv_adapter_unlock();                          // 无论 action 内部如何 return 都会解锁

        return ESP_OK;
    }
