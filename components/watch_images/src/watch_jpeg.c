#include "watch_images_internal.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "watch_images.h"

/**
 * @brief 判断 JPEG marker 是否属于任一种 SOF。
 *
 * @param marker marker 低字节。
 * @return true 表示 SOF marker。
 */
static bool is_sof_marker(uint8_t marker)
{
    return marker == 0xC0U || marker == 0xC1U || marker == 0xC2U ||
           marker == 0xC3U || marker == 0xC5U || marker == 0xC6U ||
           marker == 0xC7U || marker == 0xC9U || marker == 0xCAU ||
           marker == 0xCBU || marker == 0xCDU || marker == 0xCEU ||
           marker == 0xCFU;
}

/**
 * @brief 从文件读取一个大端 16 位字段。
 *
 * @param file 已打开的 JPEG 文件。
 * @param[out] value 接收字段值。
 * @return ESP_OK 成功，否则为格式错误。
 */
static esp_err_t read_be16(FILE *file, uint16_t *value)
{
    const int high = fgetc(file);
    const int low = fgetc(file);
    if (high == EOF || low == EOF) {
        return ESP_ERR_INVALID_SIZE;
    }
    *value = (uint16_t)(((uint16_t)high << 8U) | (uint16_t)low);
    return ESP_OK;
}

/**
 * @brief 跳过 JPEG 段中不需要解析的负载。
 *
 * @param file 已打开文件。
 * @param length 要跳过的字节数。
 * @return ESP_OK 成功，否则为格式错误。
 */
static esp_err_t skip_bytes(FILE *file, uint32_t length)
{
    if (length > (uint32_t)LONG_MAX || fseek(file, (long)length, SEEK_CUR) != 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

/**
 * @brief 用 ISO-HDLC 多项式增量更新尚未最终异或的 CRC32 状态。
 *
 * @param state 当前 CRC 内部状态。
 * @param data 输入字节。
 * @param length 输入长度。
 * @return 更新后的内部状态。
 */
uint32_t watch_images_crc32_update(
    uint32_t state,
    const uint8_t *data,
    size_t length
)
{
    for (size_t index = 0; index < length; ++index) {
        state ^= data[index];
        for (uint8_t bit = 0; bit < 8U; ++bit) {
            const uint32_t mask = 0U - (state & 1U);
            state = (state >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return state;
}

/**
 * @brief 计算文件长度、CRC 并确认结尾为 JPEG EOI。
 *
 * @param file 已打开文件。
 * @param[out] info 接收长度与 CRC。
 * @return ESP_OK 成功，否则表示读取或边界错误。
 */
static esp_err_t inspect_file_bytes(FILE *file, watch_jpeg_info_t *info)
{
    uint8_t buffer[1024];
    uint8_t previous = 0U;
    uint8_t last = 0U;
    uint32_t size = 0U;
    uint32_t state = UINT32_MAX;

    while (!feof(file)) {
        const size_t read_count = fread(buffer, 1U, sizeof(buffer), file);
        if (read_count == 0U) {
            if (ferror(file)) {
                return ESP_FAIL;
            }
            break;
        }
        if (read_count > WATCH_IMAGE_MAX_FILE_SIZE - size) {
            return ESP_ERR_INVALID_SIZE;
        }
        state = watch_images_crc32_update(state, buffer, read_count);
        for (size_t index = 0; index < read_count; ++index) {
            previous = last;
            last = buffer[index];
        }
        size += (uint32_t)read_count;
    }

    if (size < 4U || previous != 0xFFU || last != 0xD9U) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    info->size = size;
    info->crc32 = state ^ UINT32_MAX;
    return ESP_OK;
}

/**
 * @brief 解析 JPEG header 并只接受 8-bit SOF0 基线编码。
 *
 * @param file 已回到文件起点的句柄。
 * @param[out] info 接收宽高。
 * @return ESP_OK 成功，否则表示格式不兼容。
 */
static esp_err_t inspect_jpeg_header(FILE *file, watch_jpeg_info_t *info)
{
    if (fgetc(file) != 0xFF || fgetc(file) != 0xD8) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    bool found_sof0 = false;
    while (true) {
        const int prefix = fgetc(file);
        if (prefix != 0xFF) {
            return ESP_ERR_INVALID_RESPONSE;
        }

        int marker = 0;
        do {
            marker = fgetc(file);
        } while (marker == 0xFF);
        if (marker == EOF || marker == 0x00) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (marker == 0xDA) {
            return found_sof0 ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
        }
        if (marker == 0xD9) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            continue;
        }

        uint16_t segment_length = 0U;
        if (read_be16(file, &segment_length) != ESP_OK || segment_length < 2U) {
            return ESP_ERR_INVALID_SIZE;
        }
        const uint32_t payload_length = (uint32_t)segment_length - 2U;

        if (is_sof_marker((uint8_t)marker)) {
            if (marker != 0xC0 || found_sof0 || payload_length < 6U) {
                return ESP_ERR_NOT_SUPPORTED;
            }
            const int precision = fgetc(file);
            uint16_t height = 0U;
            uint16_t width = 0U;
            if (precision != 8 || read_be16(file, &height) != ESP_OK ||
                read_be16(file, &width) != ESP_OK || width == 0U || height == 0U) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            const int component_count = fgetc(file);
            if (component_count < 1 || component_count > 4 ||
                payload_length != 6U + 3U * (uint32_t)component_count) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            info->width = width;
            info->height = height;
            found_sof0 = true;
            if (skip_bytes(file, payload_length - 6U) != ESP_OK) {
                return ESP_ERR_INVALID_SIZE;
            }
        } else if (skip_bytes(file, payload_length) != ESP_OK) {
            return ESP_ERR_INVALID_SIZE;
        }
    }
}

/**
 * @brief 流式校验文件长度、CRC、EOI 和 8-bit SOF0 基线头。
 *
 * @param path JPEG 绝对路径。
 * @param[out] info 接收宽高、长度和 CRC。
 * @return ESP_OK 表示文件结构和边界有效。
 */
esp_err_t watch_jpeg_validate_file(
    const char *path,
    watch_jpeg_info_t *info
)
{
    if (path == NULL || info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return ESP_FAIL;
    }

    *info = (watch_jpeg_info_t){0};
    esp_err_t result = inspect_file_bytes(file, info);
    if (result == ESP_OK && fseek(file, 0L, SEEK_SET) == 0) {
        result = inspect_jpeg_header(file, info);
    } else if (result == ESP_OK) {
        result = ESP_FAIL;
    }

    fclose(file);
    return result;
}
