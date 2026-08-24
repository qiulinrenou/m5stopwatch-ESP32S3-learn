$ErrorActionPreference = 'Stop'

$firmwareRoot = Split-Path -Parent $PSScriptRoot
$workspaceRoot = Split-Path -Parent $firmwareRoot
$miniProgramRoot = Join-Path $workspaceRoot 'E_Badge-master\E_Badge-master'
$failures = [System.Collections.Generic.List[string]]::new()

<#
.SYNOPSIS
检查文件是否存在且包含全部必需的静态契约片段。
#>
function Assert-FileContains {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string[]]$Patterns
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        $failures.Add("Missing file: $Path")
        return
    }

    $content = Get-Content -LiteralPath $Path -Raw
    foreach ($pattern in $Patterns) {
        if ($content -notmatch $pattern) {
            $failures.Add("$Path missing contract: $pattern")
        }
    }
}

<#
.SYNOPSIS
检查文件中不存在会破坏线程边界的片段。
#>
function Assert-FileExcludes {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string[]]$Patterns
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        $failures.Add("Missing file: $Path")
        return
    }

    $content = Get-Content -LiteralPath $Path -Raw
    foreach ($pattern in $Patterns) {
        if ($content -match $pattern) {
            $failures.Add("$Path contains forbidden pattern: $pattern")
        }
    }
}

<#
.SYNOPSIS
按 ESP-IDF CSV 顺序计算分区末地址，确认不超出 16 MiB Flash。
#>
function Assert-PartitionRange {
    param([Parameter(Mandatory = $true)][string]$Path)

    $cursor = 0
    foreach ($line in Get-Content -LiteralPath $Path) {
        if ($line.TrimStart().StartsWith('#') -or [string]::IsNullOrWhiteSpace($line)) {
            continue
        }
        $fields = $line.Split(',') | ForEach-Object { $_.Trim() }
        if ($fields.Count -lt 5) {
            $failures.Add("Invalid partition row: $line")
            continue
        }
        $offsetText = $fields[3]
        $sizeText = $fields[4]
        if ($offsetText) {
            $cursor = [Convert]::ToInt64($offsetText.Substring(2), 16)
        } else {
            $cursor = [Math]::Ceiling($cursor / 0x1000) * 0x1000
        }
        if ($sizeText -match '^(\d+)([KkMm])$') {
            $unit = if ($Matches[2] -match '[Mm]') { 1MB } else { 1KB }
            $size = [int64]$Matches[1] * $unit
        } elseif ($sizeText -match '^0x') {
            $size = [Convert]::ToInt64($sizeText.Substring(2), 16)
        } else {
            $size = [int64]$sizeText
        }
        $cursor += $size
    }
    if ($cursor -gt 16MB) {
        $failures.Add(("Partition end 0x{0:X} exceeds 16 MiB flash" -f $cursor))
    }
}

Assert-FileContains (Join-Path $firmwareRoot 'partitions.csv') @(
    '(?m)^factory,\s*app,\s*factory,\s*0x10000,\s*6M',
    '(?m)^images,\s*data,\s*fat,\s*,\s*5M'
)
Assert-PartitionRange (Join-Path $firmwareRoot 'partitions.csv')

Assert-FileContains (Join-Path $firmwareRoot 'components\watch_board\include\watch_board.h') @(
    'WATCH_BUTTON_LONG_PRESS_MS\s+1500U'
)
Assert-FileContains (Join-Path $firmwareRoot 'components\watch_board\include\key.h') @(
    'g2_on_long_press',
    'g2_long_press_user_data'
)
Assert-FileContains (Join-Path $firmwareRoot 'components\watch_board\src\key.c') @(
    'long_press_fired',
    'pressed_at',
    '!key->long_press_fired\s*&&\s*key->on_click',
    'key->on_long_press\(key->long_press_user_data\)'
)

Assert-FileContains (Join-Path $firmwareRoot 'components\watch_images\include\watch_images.h') @(
    'WATCH_IMAGE_WIDTH\s+466',
    'WATCH_IMAGE_HEIGHT\s+466',
    'WATCH_IMAGE_SLOT_COUNT\s+10',
    'WATCH_IMAGE_MAX_FILE_SIZE\s+307200',
    'watch_images_begin',
    'watch_images_append',
    'watch_images_commit',
    'watch_images_cancel',
    'watch_images_delete',
    'watch_images_request_load'
)

Assert-FileContains (Join-Path $firmwareRoot 'components\watch_images\src\watch_images.c') @(
    'WATCH_IMAGES_MOUNT_PATH\s+"/badge"',
    'slot%u\.jpg%s',
    'catalog\.part',
    'catalog\.bak',
    'MALLOC_CAP_SPIRAM',
    'WATCH_IMAGES_JFIF_APP0_SIZE\s+18U',
    'decoder_accepts_jpeg_prefix',
    'offset != writer->written',
    'esp_vfs_fat_info\(\s*WATCH_IMAGES_MOUNT_PATH',
    'sync_file\(writer->file\)',
    'watch_jpeg_validate_file\(writer->part_path',
    'save_catalog_file\(&next_catalog\)',
    '\.del',
    's_active_writer\s*!=\s*NULL'
)
Assert-FileExcludes (Join-Path $firmwareRoot 'components\watch_images\src\watch_images.c') @(
    'watch_images_append\([\s\S]*?fflush\(writer->file\)',
    '\bstatvfs\s*\(',
    'sys/statvfs\.h'
)

Assert-FileContains (Join-Path $firmwareRoot 'components\watch_images\src\watch_jpeg.c') @(
    'marker != 0xC0',
    'precision != 8',
    'previous != 0xFFU \|\| last != 0xD9U',
    '0xEDB88320U'
)

Assert-FileContains (Join-Path $firmwareRoot 'components\watch_ble\include\watch_ble.h') @(
    'WATCH_BLE_DEVICE_NAME\s+"SF-BADGE"',
    '1E7A3B6F-5A4D-219C-0145-474441424653',
    '1E7A3B6F-5A4D-219C-0245-474441424653',
    '1E7A3B6F-5A4D-219C-0345-474441424653',
    '1E7A3B6F-5A4D-219C-0445-474441424653',
    'watch_ble_start'
)

Assert-FileContains (Join-Path $firmwareRoot 'components\watch_ble\src\watch_ble_internal.h') @(
    'WATCH_BLE_PROTOCOL_VERSION\s+1U',
    'WATCH_BLE_STATUS_LENGTH\s+16U',
    'WATCH_BLE_ATT_VALUE_MAX\s+236U',
    'WATCH_BLE_DATA_PAYLOAD_MAX\s+224U',
    '5LL \* 60LL \* 1000000LL',
    'WATCH_BLE_STATE_COMPLETE\s*=\s*5',
    'WATCH_BLE_ERROR_NOT_FOUND\s*=\s*12'
)

Assert-FileContains (Join-Path $firmwareRoot 'components\watch_ble\src\watch_ble_protocol.c') @(
    'WATCH_BLE_OPCODE_START\s+1U',
    'WATCH_BLE_OPCODE_END\s+2U',
    'WATCH_BLE_OPCODE_CANCEL\s+3U',
    'WATCH_BLE_OPCODE_QUERY\s+4U',
    'WATCH_BLE_CONTROL_SIMPLE_LENGTH\s+8U',
    'WATCH_BLE_CONTROL_START_LENGTH\s+24U',
    'WATCH_BLE_DATA_HEADER_LENGTH\s+12U',
    'offset < protocol->next_offset',
    'payload_length <= protocol->next_offset - offset',
    'protocol->has_committed_session &&\s*session_id == protocol->committed_session_id',
    'protocol->has_committed_session = false;\s*protocol->committed_session_id = 0U;',
    'protocol->committed_size',
    'protocol->committed_slot',
    'if \(session_id == 0U\) \{\s*publish\(\s*protocol,\s*WATCH_BLE_STATE_ERROR,\s*WATCH_BLE_ERROR_PARAMETER,\s*0U,',
    'WATCH_BLE_STATE_VERIFYING',
    'WATCH_BLE_STATE_COMPLETE',
    'START session=',
    'START 存储初始化失败',
    '进度 session=',
    '协议错误 session=',
    'COMPLETE session='
)
Assert-FileExcludes (Join-Path $firmwareRoot 'components\watch_ble\src\watch_ble_protocol.c') @(
    'offset != protocol->next_offset'
)
Assert-FileExcludes (Join-Path $firmwareRoot 'components\watch_ble\src\watch_ble_protocol.c') @(
    'static\s+uint16_t\s+get_le16\s*\(',
    'static\s+uint32_t\s+get_le32\s*\('
)

Assert-FileContains (Join-Path $firmwareRoot 'components\watch_ble\src\watch_ble_gatt.c') @(
    'BLE_GATT_CHR_F_WRITE_NO_RSP',
    'watch_ble_internal_enqueue'
)
Assert-FileExcludes (Join-Path $firmwareRoot 'components\watch_ble\src\watch_ble_gatt.c') @(
    'BLE_GATT_CHR_F_WRITE_ENC',
    'BLE_GATT_CHR_F_WRITE_AUTHEN',
    'BLE_GATT_CHR_F_READ_ENC',
    'BLE_GATT_CHR_F_READ_AUTHEN',
    'watch_images_',
    'watch_lvgl_',
    '\blv_[a-zA-Z0-9_]+\s*\(',
    '\bfopen\s*\(',
    '\bmalloc\s*\('
)

Assert-FileContains (Join-Path $firmwareRoot 'components\watch_ble\src\watch_ble.c') @(
    '#include\s+"host/util/util\.h"',
    'WATCH_BLE_WORK_QUEUE_LENGTH\s+8U',
    'xQueueReset\(s_work_queue\)'
)
Assert-FileExcludes (Join-Path $firmwareRoot 'components\watch_ble\src\watch_ble.c') @(
    'ble_gap_security_initiate\(',
    'connection_is_authenticated\(',
    '!connection_is_authenticated',
    'static\s+void\s+put_le16\s*\(',
    'static\s+void\s+put_le32\s*\('
)

Assert-FileContains (Join-Path $firmwareRoot 'components\watch_ui\include\watch_ui.h') @(
    'watch_ui_set_image_request_callback',
    'watch_ui_update_image_catalog',
    'watch_ui_accept_image',
    'watch_ui_show_pairing_code',
    'watch_ui_toggle_gallery',
    'watch_ui_get_deletable_image_slot',
    'watch_ui_cancel_image_delete',
    'watch_ui_apply_image_deleted'
)

Assert-FileContains (Join-Path $firmwareRoot 'main\main.c') @(
    'APP_EVENT_QUEUE_LENGTH\s+8U',
    'xQueueSend\(s_app_event_queue',
    'app_event_task',
    'watch_lvgl_run\(',
    'watch_images_init\(\)',
    'watch_ble_start\(&ble_config\)',
    'APP_EVENT_DELETE_CURRENT_IMAGE',
    'watch_images_delete',
    '\.g2_on_long_press\s*='
)

Assert-FileContains (Join-Path $firmwareRoot 'sdkconfig.defaults.esp32s3') @(
    'CONFIG_BT_ENABLED=y',
    'CONFIG_BT_NIMBLE_ENABLED=y',
    'CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1',
    'CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=247',
    'CONFIG_BT_NIMBLE_NVS_PERSIST=y',
    'CONFIG_BT_NIMBLE_MAX_BONDS=3'
)

Assert-FileContains (Join-Path $firmwareRoot 'sdkconfig.defaults') @(
    'CONFIG_ESP_LVGL_ADAPTER_ENABLE_DECODER=y',
    'CONFIG_FATFS_LFN_HEAP=y'
)

Assert-FileContains (Join-Path $miniProgramRoot 'src\utils\bleImageProtocol.ts') @(
    "BADGE_DEVICE_NAME = 'SF-BADGE'",
    "ESP32_BADGE_DEVICE_NAME = 'SF-BADGE'",
    "ESP32_BADGE_LEGACY_DEVICE_NAME = 'M5STOPWATCH'",
    'if \(normalized === BADGE_DEVICE_NAME \|\| normalized === ESP32_BADGE_LEGACY_DEVICE_NAME\)',
    'return BADGE_DEVICE_PROFILES\.esp32s3',
    'esp32s3:\s*\{\s*name: ESP32_BADGE_DEVICE_NAME,\s*platform: ''esp32s3'',\s*dynamicPairing: false',
    "BADGE_SERVICE_UUID = '1E7A3B6F-5A4D-219C-0145-474441424653'",
    "BADGE_CONTROL_UUID = '1E7A3B6F-5A4D-219C-0245-474441424653'",
    "BADGE_DATA_UUID = '1E7A3B6F-5A4D-219C-0345-474441424653'",
    "BADGE_STATUS_UUID = '1E7A3B6F-5A4D-219C-0445-474441424653'",
    'BADGE_PROTOCOL_VERSION = 1',
    'BADGE_IMAGE_SIZE = 466',
    'BADGE_MAX_FILE_SIZE = 307200',
    'BADGE_MAX_PAYLOAD_SIZE = 224',
    "BadgeDevicePlatform = 'sf32' \| 'esp32s3'",
    'START = 1',
    'END = 2',
    'CANCEL = 3',
    'QUERY = 4',
    'COMPLETE = 5',
    'NOT_FOUND = 12',
    'new ArrayBuffer\(24\)',
    'new ArrayBuffer\(8\)',
    'new ArrayBuffer\(12 \+ payload.byteLength\)',
    'buffer.byteLength !== 16',
    '0xedb88320'
)

Assert-FileContains (Join-Path $miniProgramRoot 'src\services\badgeBluetoothService.ts') @(
    "'pairing'",
    'platform',
    '30000',
    'writeType: ''writeNoResponse''',
    'existing\?\.platform',
    "nameProfile\?\.platform \|\| existing\?\.platform \|\| 'esp32s3'",
    'options.width !== BADGE_IMAGE_SIZE',
    'options.height !== BADGE_IMAGE_SIZE'
)

if ($failures.Count -gt 0) {
    $failures | ForEach-Object { Write-Error $_ -ErrorAction Continue }
    throw "BLE image transfer static contract failed: $($failures.Count) issue(s)"
}

Write-Host 'BLE image transfer static contract passed.'
