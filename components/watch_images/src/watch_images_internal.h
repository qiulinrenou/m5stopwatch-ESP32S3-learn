#ifndef WATCH_IMAGES_INTERNAL_H
#define WATCH_IMAGES_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"

typedef struct {
    uint16_t width;
    uint16_t height;
    uint32_t size;
    uint32_t crc32;
} watch_jpeg_info_t;

/**
 * @brief 用 ISO-HDLC 参数增量更新 CRC32 内部状态。
 *
 * @param state 尚未最终异或的 CRC 状态。
 * @param data 输入字节。
 * @param length 输入长度。
 * @return 更新后的内部状态。
 */
uint32_t watch_images_crc32_update(
    uint32_t state,
    const uint8_t *data,
    size_t length
);

/**
 * @brief 流式校验基线 JPEG，并返回尺寸、长度与 CRC32。
 *
 * @param path 待校验文件绝对路径。
 * @param[out] info 校验成功后的文件信息。
 * @return ESP_OK 表示 JPEG 含 SOF0、EOI 且结构满足限制。
 */
esp_err_t watch_jpeg_validate_file(
    const char *path,
    watch_jpeg_info_t *info
);

#endif
