#ifndef WATCH_IMAGES_H
#define WATCH_IMAGES_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WATCH_IMAGE_WIDTH 466U
#define WATCH_IMAGE_HEIGHT 466U
#define WATCH_IMAGE_SLOT_COUNT 10U
#define WATCH_IMAGE_MAX_FILE_SIZE 307200U

typedef struct watch_image_writer watch_image_writer_t;

typedef struct {
    uint8_t slot;
    uint16_t width;
    uint16_t height;
    uint32_t total_size;
    uint32_t crc32;
} watch_image_metadata_t;

typedef struct {
    uint16_t occupied_mask;
    uint8_t latest_slot;
    uint32_t sizes[WATCH_IMAGE_SLOT_COUNT];
    uint32_t crc32[WATCH_IMAGE_SLOT_COUNT];
} watch_image_catalog_t;

typedef struct {
    esp_err_t result;
    uint32_t request_id;
    uint8_t slot;
    uint8_t *data;
    size_t size;
} watch_image_load_result_t;

typedef void (*watch_image_load_callback_t)(
    const watch_image_load_result_t *result,
    void *user_data
);

/**
 * @brief 挂载 images FATFS 分区并完成掉电恢复与目录重建。
 *
 * @return ESP_OK 表示存储和异步读取任务可用，其他错误表示新增图库功能应被禁用。
 */
esp_err_t watch_images_init(void);

/**
 * @brief 取得当前槽位占用、最近槽位、长度和 CRC 快照。
 *
 * @param[out] catalog 接收一致的目录快照。
 * @return ESP_OK 成功；未初始化或参数无效时返回相应错误。
 */
esp_err_t watch_images_get_catalog(watch_image_catalog_t *catalog);

/**
 * @brief 原子删除一个已占用图片槽位并更新持久目录。
 *
 * @param slot 待删除槽位 0..9。
 * @param[out] catalog 成功时接收删除后的目录快照，可为 NULL。
 * @return ESP_OK 成功；槽位不存在、写入事务活动或文件系统失败时返回对应错误。
 * @note 删除使用 `.del` 墓碑完成掉电恢复，活动图片写入期间不会执行。
 */
esp_err_t watch_images_delete(uint8_t slot, watch_image_catalog_t *catalog);

/**
 * @brief 开始一个不透明的图片写入事务。
 *
 * @param metadata 已经完成网络字段解析的图片元数据。
 * @param[out] writer 返回只允许单任务顺序使用的会话句柄。
 * @return ESP_OK 成功；空间不足返回 ESP_ERR_NO_MEM；参数或状态错误返回对应错误。
 */
esp_err_t watch_images_begin(
    const watch_image_metadata_t *metadata,
    watch_image_writer_t **writer
);

/**
 * @brief 按严格 offset 向当前 `.part` 文件追加数据。
 *
 * @param writer begin 返回的会话句柄。
 * @param offset 发送端声明的文件偏移，必须等于当前已写长度。
 * @param data 待写入数据。
 * @param length 数据长度。
 * @param[out] next_offset 实际落盘后的下一偏移。
 * @return ESP_OK 成功；任何越界、错序或文件错误均不推进偏移。
 */
esp_err_t watch_images_append(
    watch_image_writer_t *writer,
    uint32_t offset,
    const uint8_t *data,
    size_t length,
    uint32_t *next_offset
);

/**
 * @brief 校验并原子提交图片事务。
 *
 * @param writer 当前写入会话。
 * @param[out] catalog 成功时接收提交后的目录快照，可为 NULL。
 * @return ESP_OK 成功；校验或文件替换失败时旧槽位仍被保留。
 * @note 失败后调用者必须调用 watch_images_cancel() 释放会话。
 */
esp_err_t watch_images_commit(
    watch_image_writer_t *writer,
    watch_image_catalog_t *catalog
);

/**
 * @brief 取消事务、关闭句柄并删除临时文件。
 *
 * @param writer 当前写入会话；允许传入 NULL。
 */
void watch_images_cancel(watch_image_writer_t *writer);

/**
 * @brief 非阻塞请求把指定槽位读入一张 PSRAM JPEG 缓冲。
 *
 * @param slot 槽位编号 0..9。
 * @param request_id 由 UI 生成并用于丢弃过期结果的请求编号。
 * @param callback 文件任务完成后的回调。
 * @param user_data 回调透传上下文。
 * @return ESP_OK 已入队；队列满、槽位不存在或未初始化时返回错误。
 * @note 成功结果中的 data 所有权交给回调，回调必须最终调用 heap_caps_free()。
 */
esp_err_t watch_images_request_load(
    uint8_t slot,
    uint32_t request_id,
    watch_image_load_callback_t callback,
    void *user_data
);

#ifdef __cplusplus
}
#endif

#endif
