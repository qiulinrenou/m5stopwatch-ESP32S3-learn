#include "watch_images.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "wear_levelling.h"

#include "watch_images_internal.h"

#define WATCH_IMAGES_MOUNT_PATH "/badge"
#define WATCH_IMAGES_PARTITION_LABEL "images"
#define WATCH_IMAGES_CATALOG_MAGIC 0x31474D49U
#define WATCH_IMAGES_CATALOG_VERSION 1U
#define WATCH_IMAGES_CATALOG_SIZE 96U
#define WATCH_IMAGES_INVALID_SLOT 0xFFU
#define WATCH_IMAGES_PATH_CAPACITY 48U
#define WATCH_IMAGES_LOAD_QUEUE_LENGTH 3U
#define WATCH_IMAGES_LOAD_TASK_STACK 4096U
#define WATCH_IMAGES_JFIF_APP0_SIZE 18U

struct watch_image_writer {
    FILE *file;
    watch_image_metadata_t metadata;
    uint32_t written;
    uint32_t crc_state;
    char part_path[WATCH_IMAGES_PATH_CAPACITY];
    bool active;
};

typedef struct {
    uint8_t slot;
    uint32_t request_id;
    watch_image_load_callback_t callback;
    void *user_data;
} watch_image_load_request_t;

static const char *TAG = "watch_images";
static const char *CATALOG_PATH = WATCH_IMAGES_MOUNT_PATH "/catalog.bin";
static const char *CATALOG_PART_PATH = WATCH_IMAGES_MOUNT_PATH "/catalog.part";
static const char *CATALOG_BACKUP_PATH = WATCH_IMAGES_MOUNT_PATH "/catalog.bak";
static const uint8_t JFIF_APP0[WATCH_IMAGES_JFIF_APP0_SIZE] = {
    0xFFU, 0xE0U, 0x00U, 0x10U, 0x4AU, 0x46U, 0x49U, 0x46U, 0x00U,
    0x01U, 0x01U, 0x00U, 0x00U, 0x01U, 0x00U, 0x01U, 0x00U, 0x00U,
};
static const uint8_t ADOBE_JPEG_PREFIX[] = {
    0xFFU, 0xD8U, 0xFFU, 0xEEU, 0x00U, 0x0EU, 0x41U, 0x64U, 0x6FU, 0x62U,
};

static SemaphoreHandle_t s_lock;
static QueueHandle_t s_load_queue;
static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;
static watch_image_catalog_t s_catalog;
static watch_image_writer_t *s_active_writer;
static bool s_initialized;

/**
 * @brief 把 16 位数按小端写入目录缓冲。
 *
 * @param destination 目标地址。
 * @param value 待写值。
 */
static void put_le16(uint8_t *destination, uint16_t value)
{
    destination[0] = (uint8_t)(value & 0xFFU);
    destination[1] = (uint8_t)(value >> 8U);
}

/**
 * @brief 把 32 位数按小端写入目录缓冲。
 *
 * @param destination 目标地址。
 * @param value 待写值。
 */
static void put_le32(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)(value & 0xFFU);
    destination[1] = (uint8_t)((value >> 8U) & 0xFFU);
    destination[2] = (uint8_t)((value >> 16U) & 0xFFU);
    destination[3] = (uint8_t)(value >> 24U);
}

/**
 * @brief 从目录缓冲读取小端 16 位数。
 *
 * @param source 输入地址。
 * @return 解码值。
 */
static uint16_t get_le16(const uint8_t *source)
{
    return (uint16_t)((uint16_t)source[0] | ((uint16_t)source[1] << 8U));
}

/**
 * @brief 从目录缓冲读取小端 32 位数。
 *
 * @param source 输入地址。
 * @return 解码值。
 */
static uint32_t get_le32(const uint8_t *source)
{
    return (uint32_t)source[0] |
           ((uint32_t)source[1] << 8U) |
           ((uint32_t)source[2] << 16U) |
           ((uint32_t)source[3] << 24U);
}

/**
 * @brief 生成某个槽位的正式、临时或备份路径。
 *
 * @param path 输出缓冲。
 * @param capacity 缓冲容量。
 * @param slot 槽位编号。
 * @param suffix 文件后缀，例如空串、`.part` 或 `.bak`。
 * @return ESP_OK 成功，否则表示参数或缓冲不足。
 */
static esp_err_t make_slot_path(
    char *path,
    size_t capacity,
    uint8_t slot,
    const char *suffix
)
{
    if (path == NULL || suffix == NULL || slot >= WATCH_IMAGE_SLOT_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    const int length = snprintf(
        path,
        capacity,
        WATCH_IMAGES_MOUNT_PATH "/slot%u.jpg%s",
        (unsigned int)slot,
        suffix
    );
    return length > 0 && (size_t)length < capacity ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

/**
 * @brief 判断文件是否存在。
 *
 * @param path 绝对路径。
 * @return true 表示普通目录项存在。
 */
static bool path_exists(const char *path)
{
    struct stat info;
    return path != NULL && stat(path, &info) == 0;
}

/**
 * @brief 删除允许不存在的事务文件。
 *
 * @param path 绝对路径。
 * @return ESP_OK 表示已删除或原本不存在。
 */
static esp_err_t remove_optional(const char *path)
{
    if (remove(path) == 0 || errno == ENOENT) {
        return ESP_OK;
    }
    return ESP_FAIL;
}

/**
 * @brief 刷新 stdio 与底层 FAT 文件，缩小掉电窗口。
 *
 * @param file 已打开的可写文件。
 * @return ESP_OK 表示缓冲已同步。
 */
static esp_err_t sync_file(FILE *file)
{
    if (file == NULL) {
        return ESP_FAIL;
    }
    if (fflush(file) != 0) {
        ESP_LOGE(TAG, "图片文件刷新失败: errno=%d", errno);
        return ESP_FAIL;
    }
    const int descriptor = fileno(file);
    if (descriptor < 0 || fsync(descriptor) != 0) {
        ESP_LOGE(TAG, "图片文件同步失败: errno=%d", errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/**
 * @brief 把目录快照编码为无填充、带 CRC 的固定 96 字节格式。
 *
 * @param catalog 目录快照。
 * @param[out] bytes 输出字节。
 */
static void encode_catalog(
    const watch_image_catalog_t *catalog,
    uint8_t bytes[WATCH_IMAGES_CATALOG_SIZE]
)
{
    memset(bytes, 0, WATCH_IMAGES_CATALOG_SIZE);
    put_le32(bytes, WATCH_IMAGES_CATALOG_MAGIC);
    put_le16(bytes + 4U, WATCH_IMAGES_CATALOG_VERSION);
    put_le16(bytes + 6U, catalog->occupied_mask);
    bytes[8] = catalog->latest_slot;

    size_t offset = 12U;
    for (uint8_t slot = 0U; slot < WATCH_IMAGE_SLOT_COUNT; ++slot) {
        put_le32(bytes + offset, catalog->sizes[slot]);
        offset += 4U;
    }
    for (uint8_t slot = 0U; slot < WATCH_IMAGE_SLOT_COUNT; ++slot) {
        put_le32(bytes + offset, catalog->crc32[slot]);
        offset += 4U;
    }

    const uint32_t crc = watch_images_crc32_update(
        UINT32_MAX,
        bytes,
        WATCH_IMAGES_CATALOG_SIZE - 4U
    ) ^ UINT32_MAX;
    put_le32(bytes + WATCH_IMAGES_CATALOG_SIZE - 4U, crc);
}

/**
 * @brief 校验并解码固定目录格式。
 *
 * @param bytes 输入字节。
 * @param[out] catalog 输出目录快照。
 * @return ESP_OK 表示 magic、版本、CRC 和槽位边界均有效。
 */
static esp_err_t decode_catalog(
    const uint8_t bytes[WATCH_IMAGES_CATALOG_SIZE],
    watch_image_catalog_t *catalog
)
{
    const uint32_t expected_crc = get_le32(bytes + WATCH_IMAGES_CATALOG_SIZE - 4U);
    const uint32_t actual_crc = watch_images_crc32_update(
        UINT32_MAX,
        bytes,
        WATCH_IMAGES_CATALOG_SIZE - 4U
    ) ^ UINT32_MAX;
    if (get_le32(bytes) != WATCH_IMAGES_CATALOG_MAGIC ||
        get_le16(bytes + 4U) != WATCH_IMAGES_CATALOG_VERSION ||
        expected_crc != actual_crc) {
        return ESP_ERR_INVALID_CRC;
    }

    *catalog = (watch_image_catalog_t){
        .occupied_mask = get_le16(bytes + 6U),
        .latest_slot = bytes[8],
    };
    if ((catalog->occupied_mask & ~((1U << WATCH_IMAGE_SLOT_COUNT) - 1U)) != 0U ||
        (catalog->latest_slot != WATCH_IMAGES_INVALID_SLOT &&
         catalog->latest_slot >= WATCH_IMAGE_SLOT_COUNT)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    size_t offset = 12U;
    for (uint8_t slot = 0U; slot < WATCH_IMAGE_SLOT_COUNT; ++slot) {
        catalog->sizes[slot] = get_le32(bytes + offset);
        offset += 4U;
    }
    for (uint8_t slot = 0U; slot < WATCH_IMAGE_SLOT_COUNT; ++slot) {
        catalog->crc32[slot] = get_le32(bytes + offset);
        offset += 4U;
    }
    return ESP_OK;
}

/**
 * @brief 从正式目录文件读取并校验快照。
 *
 * @param[out] catalog 输出目录。
 * @return ESP_OK 成功，否则表示目录缺失或损坏。
 */
static esp_err_t load_catalog_file(watch_image_catalog_t *catalog)
{
    uint8_t bytes[WATCH_IMAGES_CATALOG_SIZE];
    FILE *file = fopen(CATALOG_PATH, "rb");
    if (file == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    const size_t count = fread(bytes, 1U, sizeof(bytes), file);
    const bool exact_length = count == sizeof(bytes) && fgetc(file) == EOF;
    fclose(file);
    return exact_length ? decode_catalog(bytes, catalog) : ESP_ERR_INVALID_SIZE;
}

/**
 * @brief 原子写入目录文件，并在最后一步失败时恢复旧目录。
 *
 * @param catalog 待持久化快照。
 * @return ESP_OK 成功，否则原正式目录仍可恢复。
 */
static esp_err_t save_catalog_file(const watch_image_catalog_t *catalog)
{
    uint8_t bytes[WATCH_IMAGES_CATALOG_SIZE];
    encode_catalog(catalog, bytes);

    FILE *file = fopen(CATALOG_PART_PATH, "wb");
    if (file == NULL) {
        return ESP_FAIL;
    }
    const bool written = fwrite(bytes, 1U, sizeof(bytes), file) == sizeof(bytes);
    const esp_err_t sync_result = written ? sync_file(file) : ESP_FAIL;
    fclose(file);
    if (sync_result != ESP_OK) {
        remove_optional(CATALOG_PART_PATH);
        return sync_result;
    }

    remove_optional(CATALOG_BACKUP_PATH);
    const bool had_catalog = path_exists(CATALOG_PATH);
    if (had_catalog && rename(CATALOG_PATH, CATALOG_BACKUP_PATH) != 0) {
        remove_optional(CATALOG_PART_PATH);
        return ESP_FAIL;
    }
    if (rename(CATALOG_PART_PATH, CATALOG_PATH) != 0) {
        if (had_catalog) {
            rename(CATALOG_BACKUP_PATH, CATALOG_PATH);
        }
        remove_optional(CATALOG_PART_PATH);
        return ESP_FAIL;
    }
    remove_optional(CATALOG_BACKUP_PATH);
    return ESP_OK;
}

/**
 * @brief 恢复槽位与目录事务，并清理未提交的 `.part` 和 `.del`。
 *
 * @note `.del` 优先于 `.bak`，保证掉电发生在删除事务中时不会复活已删除图片。
 */
static void recover_transaction_files(void)
{
    if (!path_exists(CATALOG_PATH) && path_exists(CATALOG_BACKUP_PATH)) {
        rename(CATALOG_BACKUP_PATH, CATALOG_PATH);
    } else if (path_exists(CATALOG_PATH)) {
        remove_optional(CATALOG_BACKUP_PATH);
    }
    remove_optional(CATALOG_PART_PATH);

    for (uint8_t slot = 0U; slot < WATCH_IMAGE_SLOT_COUNT; ++slot) {
        char final_path[WATCH_IMAGES_PATH_CAPACITY];
        char part_path[WATCH_IMAGES_PATH_CAPACITY];
        char backup_path[WATCH_IMAGES_PATH_CAPACITY];
        char delete_path[WATCH_IMAGES_PATH_CAPACITY];
        make_slot_path(final_path, sizeof(final_path), slot, "");
        make_slot_path(part_path, sizeof(part_path), slot, ".part");
        make_slot_path(backup_path, sizeof(backup_path), slot, ".bak");
        make_slot_path(delete_path, sizeof(delete_path), slot, ".del");

        if (path_exists(delete_path)) {
            remove_optional(final_path);               // 墓碑存在表示删除已开始，正式文件不可恢复
            remove_optional(part_path);                // 同槽位未完成传输也随删除事务清理
            remove_optional(backup_path);              // 禁止旧备份在后续启动时复活
            remove_optional(delete_path);              // 最后移除墓碑，完成幂等恢复
            continue;
        }

        if (!path_exists(final_path) && path_exists(backup_path)) {
            rename(backup_path, final_path);
        } else if (path_exists(final_path)) {
            remove_optional(backup_path);
        }
        remove_optional(part_path);
    }
}

/**
 * @brief 扫描并严格校验十个槽位，重建可信目录。
 *
 * @param[out] catalog 输出扫描结果。
 */
static void scan_slots(watch_image_catalog_t *catalog)
{
    *catalog = (watch_image_catalog_t){
        .latest_slot = WATCH_IMAGES_INVALID_SLOT,
    };
    time_t latest_time = 0;

    for (uint8_t slot = 0U; slot < WATCH_IMAGE_SLOT_COUNT; ++slot) {
        char path[WATCH_IMAGES_PATH_CAPACITY];
        make_slot_path(path, sizeof(path), slot, "");
        if (!path_exists(path)) {
            continue;
        }

        watch_jpeg_info_t jpeg = {0};
        struct stat info;
        const esp_err_t result = watch_jpeg_validate_file(path, &jpeg);
        if (result != ESP_OK || jpeg.width != WATCH_IMAGE_WIDTH ||
            jpeg.height != WATCH_IMAGE_HEIGHT || jpeg.size > WATCH_IMAGE_MAX_FILE_SIZE ||
            stat(path, &info) != 0) {
            ESP_LOGW(TAG, "删除无效图片槽位 %u", (unsigned int)slot);
            remove_optional(path);
            continue;
        }

        catalog->occupied_mask |= (uint16_t)(1U << slot);
        catalog->sizes[slot] = jpeg.size;
        catalog->crc32[slot] = jpeg.crc32;
        if (catalog->latest_slot == WATCH_IMAGES_INVALID_SLOT || info.st_mtime >= latest_time) {
            catalog->latest_slot = slot;
            latest_time = info.st_mtime;
        }
    }
}

/**
 * @brief 判断持久目录的槽位数据是否与扫描结果一致。
 *
 * @param stored 持久目录。
 * @param scanned 实际扫描目录。
 * @return true 表示可保留 stored 的最近槽位顺序。
 */
static bool catalogs_match(
    const watch_image_catalog_t *stored,
    const watch_image_catalog_t *scanned
)
{
    if (stored->occupied_mask != scanned->occupied_mask) {
        return false;
    }
    for (uint8_t slot = 0U; slot < WATCH_IMAGE_SLOT_COUNT; ++slot) {
        if (stored->sizes[slot] != scanned->sizes[slot] ||
            stored->crc32[slot] != scanned->crc32[slot]) {
            return false;
        }
    }
    if (stored->occupied_mask == 0U) {
        return stored->latest_slot == WATCH_IMAGES_INVALID_SLOT;
    }
    return stored->latest_slot < WATCH_IMAGE_SLOT_COUNT &&
           (stored->occupied_mask & (1U << stored->latest_slot)) != 0U;
}

/**
 * @brief 判断 JPEG 是否能被当前 esp_lv_decoder 的 LVGL8 前缀检测直接识别。
 *
 * @param data 已校验 JPEG 的起始地址。
 * @param size JPEG 字节数。
 * @return true 表示开头为第三方解码器支持的 JFIF 或 Adobe 签名。
 */
static bool decoder_accepts_jpeg_prefix(const uint8_t *data, size_t size)
{
    const size_t prefix_size = sizeof(ADOBE_JPEG_PREFIX);
    return size >= prefix_size &&
           (memcmp(data, "\xFF\xD8\xFF\xE0\x00\x10JFIF", prefix_size) == 0 ||
            memcmp(data, ADOBE_JPEG_PREFIX, prefix_size) == 0);
}

/**
 * @brief 在文件任务中读取一张图片并校验目录记录的长度与 CRC。
 *
 * @param request 入队请求。
 * @param[out] result 回调结果。
 */
static void load_image(
    const watch_image_load_request_t *request,
    watch_image_load_result_t *result
)
{
    *result = (watch_image_load_result_t){
        .result = ESP_ERR_NOT_FOUND,
        .request_id = request->request_id,
        .slot = request->slot,
    };

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const uint16_t bit = (uint16_t)(1U << request->slot);
    if ((s_catalog.occupied_mask & bit) == 0U) {
        xSemaphoreGive(s_lock);
        return;
    }

    const uint32_t expected_size = s_catalog.sizes[request->slot];
    const uint32_t expected_crc = s_catalog.crc32[request->slot];
    uint8_t *data = heap_caps_malloc(
        (size_t)expected_size + WATCH_IMAGES_JFIF_APP0_SIZE,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (data == NULL) {
        result->result = ESP_ERR_NO_MEM;
        xSemaphoreGive(s_lock);
        return;
    }

    char path[WATCH_IMAGES_PATH_CAPACITY];
    make_slot_path(path, sizeof(path), request->slot, "");
    FILE *file = fopen(path, "rb");
    uint8_t *stored_data = data + WATCH_IMAGES_JFIF_APP0_SIZE;
    const size_t count = file == NULL
        ? 0U
        : fread(stored_data, 1U, expected_size, file);
    const bool exact_length = file != NULL && count == expected_size && fgetc(file) == EOF;
    if (file != NULL) {
        fclose(file);
    }
    const uint32_t actual_crc = exact_length
        ? (watch_images_crc32_update(UINT32_MAX, stored_data, count) ^ UINT32_MAX)
        : 0U;
    xSemaphoreGive(s_lock);

    if (!exact_length || actual_crc != expected_crc) {
        heap_caps_free(data);
        result->result = exact_length ? ESP_ERR_INVALID_CRC : ESP_FAIL;
        return;
    }
    if (decoder_accepts_jpeg_prefix(stored_data, count)) {
        memmove(data, stored_data, count);
        result->size = count;
    } else {
        data[0] = stored_data[0];
        data[1] = stored_data[1];
        memcpy(data + 2U, JFIF_APP0, sizeof(JFIF_APP0));
        result->size = count + sizeof(JFIF_APP0);
    }
    result->result = ESP_OK;
    result->data = data;
}

/**
 * @brief 串行处理异步图片读取请求。
 *
 * @param argument 未使用。
 */
static void image_load_task(void *argument)
{
    (void)argument;
    watch_image_load_request_t request;
    while (true) {
        if (xQueueReceive(s_load_queue, &request, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        watch_image_load_result_t result;
        load_image(&request, &result);
        request.callback(&result, request.user_data);
    }
}

/**
 * @brief 挂载 images FATFS，并完成事务恢复、目录校验和读取任务创建。
 *
 * @return ESP_OK 表示图片存储可用；其他错误由 main 降级处理。
 */
esp_err_t watch_images_init(void)
{
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 5,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };
    esp_err_t result = esp_vfs_fat_spiflash_mount_rw_wl(
        WATCH_IMAGES_MOUNT_PATH,
        WATCH_IMAGES_PARTITION_LABEL,
        &mount_config,
        &s_wl_handle
    );
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "images FATFS 挂载失败: %s", esp_err_to_name(result));
        return result;
    }

    s_lock = xSemaphoreCreateMutex();
    s_load_queue = xQueueCreate(
        WATCH_IMAGES_LOAD_QUEUE_LENGTH,
        sizeof(watch_image_load_request_t)
    );
    if (s_lock == NULL || s_load_queue == NULL) {
        result = ESP_ERR_NO_MEM;
        goto fail;
    }

    recover_transaction_files();
    watch_image_catalog_t scanned;
    watch_image_catalog_t stored;
    scan_slots(&scanned);
    if (load_catalog_file(&stored) == ESP_OK && catalogs_match(&stored, &scanned)) {
        s_catalog = stored;
    } else {
        s_catalog = scanned;
        result = save_catalog_file(&s_catalog);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "图片目录重建后保存失败: %s", esp_err_to_name(result));
            goto fail;
        }
    }

    if (xTaskCreatePinnedToCore(
            image_load_task,
            "watch_image_load",
            WATCH_IMAGES_LOAD_TASK_STACK,
            NULL,
            4,
            NULL,
            0
        ) != pdPASS) {
        result = ESP_ERR_NO_MEM;
        goto fail;
    }

    s_initialized = true;
    ESP_LOGI(
        TAG,
        "图片分区已挂载，占用掩码=0x%03x，最近槽位=%u",
        s_catalog.occupied_mask,
        (unsigned int)s_catalog.latest_slot
    );
    return ESP_OK;

fail:
    if (s_load_queue != NULL) {
        vQueueDelete(s_load_queue);
        s_load_queue = NULL;
    }
    if (s_lock != NULL) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
    }
    esp_vfs_fat_spiflash_unmount_rw_wl(WATCH_IMAGES_MOUNT_PATH, s_wl_handle);
    s_wl_handle = WL_INVALID_HANDLE;
    return result;
}

/**
 * @brief 在线程安全的互斥区内复制当前图片目录。
 *
 * @param[out] catalog 接收槽位位图、最近槽位、长度和 CRC。
 * @return ESP_OK 成功；参数或初始化状态无效时返回对应错误。
 */
esp_err_t watch_images_get_catalog(watch_image_catalog_t *catalog)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (catalog == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *catalog = s_catalog;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

/**
 * @brief 从指定槽位之后按升序循环查找下一个已占用槽位。
 *
 * @param occupied_mask 已清除待删除槽位的占用位图。
 * @param after_slot 从该槽位的下一个编号开始查找。
 * @return 找到的槽位，位图为空时返回 WATCH_IMAGES_INVALID_SLOT。
 */
static uint8_t find_next_occupied_slot(uint16_t occupied_mask, uint8_t after_slot)
{
    for (uint8_t distance = 1U; distance <= WATCH_IMAGE_SLOT_COUNT; ++distance) {
        const uint8_t candidate = (uint8_t)(
            ((uint16_t)after_slot + distance) % WATCH_IMAGE_SLOT_COUNT
        );
        if ((occupied_mask & (uint16_t)(1U << candidate)) != 0U) {
            return candidate;
        }
    }
    return WATCH_IMAGES_INVALID_SLOT;
}

/**
 * @brief 使用 `.del` 墓碑原子删除一个图片槽位并提交新目录。
 *
 * @param slot 待删除槽位 0..9。
 * @param[out] catalog 成功时接收删除后的目录快照，可为 NULL。
 * @return ESP_OK 成功；写入事务活动、槽位不存在或文件系统失败时返回对应错误。
 * @note 本函数持有存储互斥锁，不与 BLE 图片写入事务并行。
 */
esp_err_t watch_images_delete(uint8_t slot, watch_image_catalog_t *catalog)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (slot >= WATCH_IMAGE_SLOT_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    char final_path[WATCH_IMAGES_PATH_CAPACITY];
    char delete_path[WATCH_IMAGES_PATH_CAPACITY];
    if (make_slot_path(final_path, sizeof(final_path), slot, "") != ESP_OK ||
        make_slot_path(delete_path, sizeof(delete_path), slot, ".del") != ESP_OK) {
        return ESP_ERR_INVALID_SIZE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_active_writer != NULL) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if ((s_catalog.occupied_mask & (uint16_t)(1U << slot)) == 0U) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }

    remove_optional(delete_path);
    if (rename(final_path, delete_path) != 0) {
        ESP_LOGE(TAG, "隔离待删除图片槽位 %u 失败: errno=%d",
                 (unsigned int)slot, errno);
        xSemaphoreGive(s_lock);
        return ESP_FAIL;
    }

    watch_image_catalog_t next_catalog = s_catalog;
    next_catalog.occupied_mask &= (uint16_t)~(1U << slot);
    next_catalog.sizes[slot] = 0U;
    next_catalog.crc32[slot] = 0U;
    if (next_catalog.latest_slot == slot ||
        next_catalog.latest_slot >= WATCH_IMAGE_SLOT_COUNT ||
        (next_catalog.occupied_mask & (uint16_t)(1U << next_catalog.latest_slot)) == 0U) {
        next_catalog.latest_slot = find_next_occupied_slot(
            next_catalog.occupied_mask,
            slot
        );
    }

    const esp_err_t result = save_catalog_file(&next_catalog);
    if (result != ESP_OK) {
        if (rename(delete_path, final_path) != 0) {
            ESP_LOGE(TAG, "删除目录提交失败且槽位 %u 恢复失败: errno=%d",
                     (unsigned int)slot, errno);
        }
        xSemaphoreGive(s_lock);
        return result;
    }

    s_catalog = next_catalog;
    if (catalog != NULL) {
        *catalog = next_catalog;
    }
    if (remove_optional(delete_path) != ESP_OK) {
        ESP_LOGW(TAG, "槽位 %u 删除已提交，墓碑将在下次启动清理",
                 (unsigned int)slot);
    }
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "图片槽位 %u 已删除，占用掩码=0x%03x",
             (unsigned int)slot, next_catalog.occupied_mask);
    return ESP_OK;
}

/**
 * @brief 为一个严格 466 x 466 JPEG 创建唯一活动写入事务。
 *
 * @param metadata 发送端已经解析并校验边界的元数据。
 * @param[out] writer 返回不透明事务句柄。
 * @return ESP_OK 成功；空间、参数或活动会话冲突返回对应错误。
 */
esp_err_t watch_images_begin(
    const watch_image_metadata_t *metadata,
    watch_image_writer_t **writer
)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (metadata == NULL || writer == NULL || metadata->slot >= WATCH_IMAGE_SLOT_COUNT ||
        metadata->width != WATCH_IMAGE_WIDTH || metadata->height != WATCH_IMAGE_HEIGHT ||
        metadata->total_size == 0U || metadata->total_size > WATCH_IMAGE_MAX_FILE_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }

    uint64_t total_bytes = 0U;
    uint64_t free_bytes = 0U;
    const esp_err_t info_result = esp_vfs_fat_info(
        WATCH_IMAGES_MOUNT_PATH,
        &total_bytes,
        &free_bytes
    );
    if (info_result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "查询图片分区空间失败: %s (0x%x), errno=%d",
            esp_err_to_name(info_result),
            (unsigned int)info_result,
            errno
        );
        return info_result;
    }
    if (free_bytes < (uint64_t)metadata->total_size + 4096U) {
        ESP_LOGW(
            TAG,
            "图片分区空间不足: free=%llu, required=%llu, total=%llu",
            (unsigned long long)free_bytes,
            (unsigned long long)metadata->total_size + 4096ULL,
            (unsigned long long)total_bytes
        );
        return ESP_ERR_NO_MEM;
    }

    watch_image_writer_t *new_writer = calloc(1U, sizeof(*new_writer));
    if (new_writer == NULL) {
        return ESP_ERR_NO_MEM;
    }
    new_writer->metadata = *metadata;
    new_writer->crc_state = UINT32_MAX;
    if (make_slot_path(
            new_writer->part_path,
            sizeof(new_writer->part_path),
            metadata->slot,
            ".part"
        ) != ESP_OK) {
        free(new_writer);
        return ESP_ERR_INVALID_SIZE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_active_writer != NULL) {
        xSemaphoreGive(s_lock);
        free(new_writer);
        return ESP_ERR_INVALID_STATE;
    }
    remove_optional(new_writer->part_path);
    new_writer->file = fopen(new_writer->part_path, "wb");
    if (new_writer->file == NULL) {
        xSemaphoreGive(s_lock);
        free(new_writer);
        return ESP_FAIL;
    }
    new_writer->active = true;
    s_active_writer = new_writer;
    xSemaphoreGive(s_lock);
    *writer = new_writer;
    return ESP_OK;
}

/**
 * @brief 按严格 offset 写入一个分片并推进 CRC 和下一偏移。
 *
 * @param writer 当前唯一写入事务。
 * @param offset 必须等于已经写入的长度。
 * @param data 分片数据。
 * @param length 分片字节数。
 * @param[out] next_offset 成功落盘后的下一偏移。
 * @return ESP_OK 成功；错序、越界或文件错误不推进偏移。
 */
esp_err_t watch_images_append(
    watch_image_writer_t *writer,
    uint32_t offset,
    const uint8_t *data,
    size_t length,
    uint32_t *next_offset
)
{
    if (!s_initialized || writer == NULL || data == NULL || length == 0U ||
        next_offset == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (writer != s_active_writer || !writer->active || writer->file == NULL) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (offset != writer->written || length > writer->metadata.total_size - writer->written) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t count = fwrite(data, 1U, length, writer->file);
    if (count != length) {
        ESP_LOGE(
            TAG,
            "图片分片写入失败: offset=%lu, expected=%u, actual=%u, errno=%d",
            (unsigned long)offset,
            (unsigned int)length,
            (unsigned int)count,
            errno
        );
        xSemaphoreGive(s_lock);
        return ESP_FAIL;
    }
    writer->crc_state = watch_images_crc32_update(writer->crc_state, data, length);
    writer->written += (uint32_t)length;
    *next_offset = writer->written;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

/**
 * @brief 在存储互斥区内完成 CRC、JPEG、文件替换和目录原子提交。
 *
 * @param writer 已写满的活动事务。
 * @param[out] catalog 可选的提交后目录快照。
 * @return ESP_OK 成功；失败时保留或恢复旧槽位，调用者随后取消事务。
 */
esp_err_t watch_images_commit(
    watch_image_writer_t *writer,
    watch_image_catalog_t *catalog
)
{
    if (!s_initialized || writer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (writer != s_active_writer || !writer->active || writer->file == NULL) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (writer->written != writer->metadata.total_size) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_SIZE;
    }
    if ((writer->crc_state ^ UINT32_MAX) != writer->metadata.crc32) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_CRC;
    }
    esp_err_t result = sync_file(writer->file);
    fclose(writer->file);
    writer->file = NULL;
    if (result != ESP_OK) {
        xSemaphoreGive(s_lock);
        return result;
    }

    watch_jpeg_info_t jpeg = {0};
    result = watch_jpeg_validate_file(writer->part_path, &jpeg);
    if (result != ESP_OK || jpeg.width != WATCH_IMAGE_WIDTH ||
        jpeg.height != WATCH_IMAGE_HEIGHT || jpeg.size != writer->metadata.total_size ||
        jpeg.crc32 != writer->metadata.crc32) {
        xSemaphoreGive(s_lock);
        return result == ESP_OK ? ESP_ERR_INVALID_RESPONSE : result;
    }

    char final_path[WATCH_IMAGES_PATH_CAPACITY];
    char backup_path[WATCH_IMAGES_PATH_CAPACITY];
    make_slot_path(final_path, sizeof(final_path), writer->metadata.slot, "");
    make_slot_path(backup_path, sizeof(backup_path), writer->metadata.slot, ".bak");

    if (writer != s_active_writer || !writer->active) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    remove_optional(backup_path);
    const bool had_previous = path_exists(final_path);
    if (had_previous && rename(final_path, backup_path) != 0) {
        xSemaphoreGive(s_lock);
        return ESP_FAIL;
    }
    if (rename(writer->part_path, final_path) != 0) {
        if (had_previous) {
            rename(backup_path, final_path);
        }
        xSemaphoreGive(s_lock);
        return ESP_FAIL;
    }

    watch_image_catalog_t next_catalog = s_catalog;
    const uint8_t slot = writer->metadata.slot;
    next_catalog.occupied_mask |= (uint16_t)(1U << slot);
    next_catalog.latest_slot = slot;
    next_catalog.sizes[slot] = jpeg.size;
    next_catalog.crc32[slot] = jpeg.crc32;
    result = save_catalog_file(&next_catalog);
    if (result != ESP_OK) {
        remove_optional(final_path);
        if (had_previous) {
            rename(backup_path, final_path);
        }
        xSemaphoreGive(s_lock);
        return result;
    }

    remove_optional(backup_path);
    s_catalog = next_catalog;
    if (catalog != NULL) {
        *catalog = next_catalog;
    }
    writer->active = false;
    s_active_writer = NULL;
    xSemaphoreGive(s_lock);
    free(writer);
    return ESP_OK;
}

/**
 * @brief 关闭并删除未提交事务，释放句柄所有权。
 *
 * @param writer 当前事务；NULL 时直接返回。
 */
void watch_images_cancel(watch_image_writer_t *writer)
{
    if (writer == NULL) {
        return;
    }
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    if (writer->file != NULL) {
        fclose(writer->file);
        writer->file = NULL;
    }
    if (writer->part_path[0] != '\0') {
        remove_optional(writer->part_path);
    }
    if (s_active_writer == writer) {
        s_active_writer = NULL;
    }
    writer->active = false;
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
    free(writer);
}

/**
 * @brief 无阻塞请求读取一个已占用槽位到 PSRAM。
 *
 * @param slot 槽位 0..9。
 * @param request_id UI 用于丢弃过期结果的编号。
 * @param callback 文件任务完成回调。
 * @param user_data 回调上下文。
 * @return ESP_OK 表示已入队；队列满、槽位不存在或参数错误返回对应错误。
 */
esp_err_t watch_images_request_load(
    uint8_t slot,
    uint32_t request_id,
    watch_image_load_callback_t callback,
    void *user_data
)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (slot >= WATCH_IMAGE_SLOT_COUNT || callback == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool occupied = (s_catalog.occupied_mask & (1U << slot)) != 0U;
    xSemaphoreGive(s_lock);
    if (!occupied) {
        return ESP_ERR_NOT_FOUND;
    }

    const watch_image_load_request_t request = {
        .slot = slot,
        .request_id = request_id,
        .callback = callback,
        .user_data = user_data,
    };
    return xQueueSend(s_load_queue, &request, 0U) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}
