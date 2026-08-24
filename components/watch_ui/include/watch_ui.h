#ifndef WATCH_UI_H
#define WATCH_UI_H

#include <stdbool.h>                                 // 提供数据有效状态
#include <stddef.h>                                  // 提供 size_t 图片长度
#include <stdint.h>                                  // 提供固定宽度日期时间字段

#include "esp_lv_adapter_display.h"                  // 提供 LVGL 8/9 兼容的 lv_display_t 类型

#define WATCH_UI_IMAGE_WIDTH 466U
#define WATCH_UI_IMAGE_HEIGHT 466U
#define WATCH_UI_IMAGE_SLOT_COUNT 10U
#define WATCH_UI_IMAGE_MAX_SOURCE_SIZE (307200U + 18U)

typedef bool (*watch_ui_image_request_callback_t)(
    uint32_t request_id,
    uint8_t slot,
    void *user_data
);

typedef void (*watch_ui_image_release_callback_t)(void *buffer);

typedef struct {
    uint16_t occupied_mask;
    uint8_t latest_slot;
} watch_ui_image_catalog_t;

typedef struct {
    uint32_t request_id;
    uint8_t slot;
    bool success;
    uint8_t *data;
    size_t size;
    watch_ui_image_release_callback_t release;
} watch_ui_image_result_t;

typedef struct {
    bool time_valid;                                 // true 表示 RTC 日期时间已经通过校验
    uint16_t year;                                   // 完整年份，例如 2026
    uint8_t month;                                   // 月份，范围 1-12
    uint8_t day;                                     // 日期，范围 1-31
    uint8_t weekday;                                 // 星期，范围 0-6，0 表示星期日
    uint8_t hour;                                    // 小时，范围 0-23
    uint8_t minute;                                  // 分钟，范围 0-59
    uint8_t second;                                  // 秒，范围 0-59
    bool battery_valid;                              // true 表示电量百分比有效
    uint8_t battery_percent;                         // 经过板级滤波的电量，范围 0-100
} watch_ui_data_t;

/**
 * @brief 创建手表的初始主表盘界面。
 *
 * 本函数只负责创建 LVGL 对象，不负责初始化 LVGL adapter 或读取硬件。
 * 函数由 watch_lvgl_start() 在统一的 LVGL 锁内调用。
 *
 * @param display 已经由 watch_lvgl 注册完成的 LVGL Display。
 */
void ui_init(lv_display_t *display);

/**
 * @brief 在主表盘和 HTML 风格设置菜单之间切换。
 *
 * @param display 由 watch_lvgl 注册并传入的 LVGL Display。
 * @return 无返回值；Display 不匹配或页面未创建时忽略请求。
 * @note 调用者必须已经处于 watch_lvgl_run() 的 LVGL 互斥锁内，不能从 GPIO ISR 调用。
 */
void watch_ui_toggle_menu(lv_display_t *display);

/**
 * @brief 注册图库的非阻塞图片读取请求回调。
 *
 * @param callback UI 需要槽位 JPEG 时调用的回调。
 * @param user_data 回调透传上下文。
 * @note 本函数只保存函数指针，必须在 LVGL 锁内调用。
 */
void watch_ui_set_image_request_callback(
    watch_ui_image_request_callback_t callback,
    void *user_data
);

/**
 * @brief 更新图库可用槽位和最近槽位快照。
 *
 * @param display watch_lvgl 已注册的 Display。
 * @param catalog 不含文件系统细节的目录快照。
 * @note 必须在 watch_lvgl_run() 的 LVGL 锁内调用。
 */
void watch_ui_update_image_catalog(
    lv_display_t *display,
    const watch_ui_image_catalog_t *catalog
);

/**
 * @brief 请求异步加载一个已占用槽位。
 *
 * @param display watch_lvgl 已注册的 Display。
 * @param slot 目标槽位 0..9。
 * @param show_on_load 成功后是否自动全屏显示。
 * @return true 表示请求已入应用层队列。
 */
bool watch_ui_request_image(
    lv_display_t *display,
    uint8_t slot,
    bool show_on_load
);

/**
 * @brief 接管异步加载完成的 JPEG PSRAM 缓冲。
 *
 * @param display watch_lvgl 已注册的 Display。
 * @param result 含请求编号、槽位、数据和释放函数的结果。
 * @note 无论结果是否过期，data 的所有权都会在本函数内接管并最终释放。
 */
void watch_ui_accept_image(
    lv_display_t *display,
    const watch_ui_image_result_t *result
);

/**
 * @brief 使用 G2 语义打开最近图片，或从图片页返回主表盘。
 *
 * @param display watch_lvgl 已注册的 Display。
 * @note 配对页显示时忽略请求；加载期间保留旧图并忽略重复请求。
 */
void watch_ui_toggle_gallery(lv_display_t *display);

/**
 * @brief 在图库页锁定当前可删除槽位并阻止删除期间的页面或手势切换。
 *
 * @param display watch_lvgl 已注册的 Display。
 * @param[out] slot 成功时接收当前全屏显示的槽位。
 * @return true 表示当前状态允许删除且删除状态锁已建立。
 * @note 只能在 watch_lvgl_run() 的 LVGL 锁内调用。
 */
bool watch_ui_get_deletable_image_slot(lv_display_t *display, uint8_t *slot);

/**
 * @brief 取消一次未成功落盘的图片删除并恢复图库交互。
 *
 * @param display watch_lvgl 已注册的 Display。
 * @param slot 先前锁定但删除失败的槽位。
 * @note 只能在 watch_lvgl_run() 的 LVGL 锁内调用。
 */
void watch_ui_cancel_image_delete(lv_display_t *display, uint8_t slot);

/**
 * @brief 应用已成功提交的槽位删除，并选择下一张或返回主表盘。
 *
 * @param display watch_lvgl 已注册的 Display。
 * @param deleted_slot 已删除槽位。
 * @param catalog 删除后的图片目录。
 * @note 有剩余图片时保留旧缓冲直至下一张加载成功；最后一张删除时立即释放旧缓冲。
 */
void watch_ui_apply_image_deleted(
    lv_display_t *display,
    uint8_t deleted_slot,
    const watch_ui_image_catalog_t *catalog
);

/**
 * @brief 全屏显示首次配对的六位动态口令。
 *
 * @param display watch_lvgl 已注册的 Display。
 * @param passkey 100000..999999 的动态口令。
 */
void watch_ui_show_pairing_code(lv_display_t *display, uint32_t passkey);

/**
 * @brief 隐藏配对页并恢复进入配对前的页面。
 *
 * @param display watch_lvgl 已注册的 Display。
 */
void watch_ui_hide_pairing_code(lv_display_t *display);

/**
 * @brief 使用一份完整数据快照刷新主表盘。
 *
 * @param display watch_lvgl 已注册的 Display。
 * @param data 已经在 LVGL 锁外读取完成的数据快照。
 *
 * @note 本函数不自行加锁，必须由 watch_lvgl_start() 或 watch_lvgl_run() 调用。
 * @note 本函数只更新 LVGL 对象，不允许执行 I2C 或其他阻塞硬件访问。
 */
void watch_ui_update(
    lv_display_t *display,
    const watch_ui_data_t *data
);

#endif
