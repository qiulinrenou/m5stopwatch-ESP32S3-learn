# ESP32 BLE 图片接收端接入文档

本文供 ESP32 固件开发者接入本项目的微信小程序图片传输功能。推荐方案是让 ESP32 作为 BLE Peripheral，完整兼容当前 `SF-BADGE` GATT 服务和二进制协议。按本文实现后，小程序现有的扫描、连接、分片、CRC 校验、超时重试和断线续传逻辑无需重写。

本文以 **ESP-IDF 5.x + NimBLE** 为主要环境。Arduino、Bluedroid 或其他 BLE 协议栈也可以接入，但 GATT 属性和应用层协议必须完全一致。

相关实现：

- [`1、wechat_miniprogram_ble_image_transfer.md`](./1、wechat_miniprogram_ble_image_transfer.md)：小程序侧协议说明；
- `src/utils/bleImageProtocol.ts`：UUID、帧格式、CRC32 和图片限制；
- `src/services/badgeBluetoothService.ts`：GATT 检查、超时重试和状态机；
- `src/pages/Editor/index.tsx`：JPEG 导出尺寸和质量。

---

## 1. 系统架构

```text
微信小程序（BLE Central / GATT Client）
        |
        | Control: Write Request
        | Data: Write Without Response
        | Status: Read + Notify
        v
ESP32（BLE Peripheral / GATT Server）
        |
        +-- 图片接收任务
        +-- 临时文件和 CRC32
        +-- 最终图片槽位
        +-- JPEG 解码/显示任务
```

ESP32 固件需要完成：

1. 广播图片传输 Service UUID；
2. 提供 Control、Data、Status 三个特征；
3. 解析 `START`、`END`、`CANCEL`、`QUERY`；
4. 按 offset 顺序接收 JPEG 分片；
5. 每处理一个命令或分片后更新 Status；
6. 校验文件长度、CRC32 和 JPEG 格式；
7. 校验成功后原子提交槽位文件；
8. BLE 断开时保留未完成会话，支持重连后 `QUERY` 续传。

---

## 2. 芯片、组件和存储

### 2.1 芯片能力

| 芯片 | BLE | 说明 |
|---|---|---|
| ESP32 | 支持 | 可用 NimBLE 或 Bluedroid |
| ESP32-C3/C6 | 支持 | 推荐 NimBLE |
| ESP32-S3 | 支持 | 有 PSRAM 的型号更适合图片显示 |
| ESP32-H2 | 支持 | 支持 BLE，没有 Wi-Fi |
| ESP32-S2 | 不支持 | 芯片没有 BLE |
| ESP32-P4 | 不直接支持 | 需要外接无线协处理器 |

推荐组件：ESP-IDF 5.x、NimBLE、FreeRTOS、LittleFS/SPIFFS/FATFS/SD 卡，以及适合目标屏幕的 JPEG 解码器。

### 2.2 存储容量

当前协议最多保存 10 张图片，单张最大 307200 字节。10 张最大图片约占 3 MB，尚未计算文件系统和磨损均衡开销。默认 4 MB Flash 通常无法同时容纳应用、OTA 和这些图片。建议使用 SD 卡、8/16 MB Flash，或者同步减少槽位和文件上限。

不要在 RAM 中缓存整张图片。ESP32 应将每个分片直接写入临时文件，或写入专用 Flash 分区。

---

## 3. BLE 连接模型

| 端 | BLE 角色 | GATT 角色 |
|---|---|---|
| 微信小程序 | Central | Client |
| ESP32 | Peripheral | Server |

小程序连接后依次执行：

1. Android 请求 `ATT_MTU=247`；
2. 读取最终协商 MTU；
3. 按 UUID 查找 Service 和三个特征；
4. 检查特征属性；
5. 订阅 Status notification；
6. 主动读取一次 Status；
7. 收到有效的 16 字节 Status 后才认为连接就绪。

因此 Status 必须同时支持 **Read 和 Notify**。没有活动传输时也必须能读到有效状态。

第一版建议仅允许一个 Central 连接、一个图片传输会话。不要让两个连接同时写同一个文件。

---

## 4. 广播要求

为实现小程序零改动，建议广播名称继续使用：

```text
SF-BADGE
```

Advertising Data 必须包含完整的 128-bit Service UUID：

```text
1E7A3B6F-5A4D-219C-0145-474441424653
```

推荐将 Flags 和 Service UUID 放入 Advertising Data，将完整设备名放入 Scan Response，以避免传统 31 字节广播包空间不足。

小程序按 Service UUID 或名称筛选设备。只要广播中有正确 Service UUID，设备名也可改成 `ESP-BADGE`；但联调阶段保留 `SF-BADGE` 更容易排除扫描过滤问题。

BLE 断开后必须重新广播。断开不等于取消，不能在 GAP Disconnect 事件中删除未完成传输。

---

## 5. GATT 服务

| 用途 | UUID | 必需属性 | 最大值长度 |
|---|---|---|---:|
| Service | `1E7A3B6F-5A4D-219C-0145-474441424653` | Primary | - |
| Control | `1E7A3B6F-5A4D-219C-0245-474441424653` | Write | 24 |
| Data | `1E7A3B6F-5A4D-219C-0345-474441424653` | Write Without Response | 236 |
| Status | `1E7A3B6F-5A4D-219C-0445-474441424653` | Read、Notify | 16 |

属性必须精确匹配：Control 不能只有 Write Without Response，Data 不能只有普通 Write，Status 不能只有 Notify。Status 的 CCCD 应由协议栈正确创建。

### 5.1 NimBLE UUID 字节序

`BLE_UUID128_INIT` 使用低字节在前的数组：

```c
#include "host/ble_uuid.h"

static const ble_uuid128_t image_service_uuid = BLE_UUID128_INIT(
    0x53, 0x46, 0x42, 0x41, 0x44, 0x47, 0x45, 0x01,
    0x9c, 0x21, 0x4d, 0x5a, 0x6f, 0x3b, 0x7a, 0x1e);
static const ble_uuid128_t image_control_uuid = BLE_UUID128_INIT(
    0x53, 0x46, 0x42, 0x41, 0x44, 0x47, 0x45, 0x02,
    0x9c, 0x21, 0x4d, 0x5a, 0x6f, 0x3b, 0x7a, 0x1e);
static const ble_uuid128_t image_data_uuid = BLE_UUID128_INIT(
    0x53, 0x46, 0x42, 0x41, 0x44, 0x47, 0x45, 0x03,
    0x9c, 0x21, 0x4d, 0x5a, 0x6f, 0x3b, 0x7a, 0x1e);
static const ble_uuid128_t image_status_uuid = BLE_UUID128_INIT(
    0x53, 0x46, 0x42, 0x41, 0x44, 0x47, 0x45, 0x04,
    0x9c, 0x21, 0x4d, 0x5a, 0x6f, 0x3b, 0x7a, 0x1e);
```

使用接受标准 UUID 字符串的其他库时不要手动反转。只有 API 明确要求 16 字节小端数组时才使用上述顺序。

### 5.2 NimBLE GATT 表骨架

```c
#include "host/ble_gatt.h"

static uint16_t status_value_handle;

static int control_access_cb(uint16_t, uint16_t,
                             struct ble_gatt_access_ctxt *, void *);
static int data_access_cb(uint16_t, uint16_t,
                          struct ble_gatt_access_ctxt *, void *);
static int status_access_cb(uint16_t, uint16_t,
                            struct ble_gatt_access_ctxt *, void *);

static const struct ble_gatt_svc_def image_gatt_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &image_service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &image_control_uuid.u,
                .access_cb = control_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = &image_data_uuid.u,
                .access_cb = data_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &image_status_uuid.u,
                .access_cb = status_access_cb,
                .val_handle = &status_value_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 }
        },
    },
    { 0 }
};
```

不同 ESP-IDF 5.x 小版本的 Host 初始化入口可能不同，但 Service、UUID、属性和回调关系不能改变。注册时检查 `ble_gatts_count_cfg()` 和 `ble_gatts_add_svcs()` 返回值。

---

## 6. ATT MTU 和分片

- 最小 ATT MTU：27；
- 推荐 ATT MTU：247；
- ESP32 本地首选 MTU应至少设置为 247；
- 最终 MTU由双方协商，不能假设一定为 247。

START 长 24 字节，ATT Write 应用数据上限为 `MTU-3`，因此 MTU 至少为 27。小程序计算 JPEG payload：

```text
payload_size = min(224, ATT_MTU - 3 - 12)
data_frame_size = 12 + payload_size
```

| ATT MTU | Data 帧上限 | JPEG payload |
|---:|---:|---:|
| 27 | 24 | 12 |
| 185 | 182 | 170 |
| 247 | 236 | 224 |

NimBLE Host 初始化时设置：

```c
ble_att_set_preferred_mtu(247);
```

Data 回调不能固定认为每次都是 236 字节。较小 MTU和最后一个分片都会产生更短的帧。

---

## 7. 编码规则和常量

所有多字节整数都是小端序。不要直接把 BLE 字节指针强制转换成 C 结构体，以免遇到 padding、未对齐和别名问题。

```c
static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void write_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void write_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
```

```c
#define IMAGE_PROTOCOL_VERSION  1
#define IMAGE_MAX_SLOTS         10
#define IMAGE_MAX_FILE_SIZE     307200U
#define IMAGE_MAX_PAYLOAD_SIZE  224U
#define IMAGE_DATA_HEADER_SIZE  12U
#define IMAGE_DATA_MAX_SIZE     236U
#define IMAGE_STATUS_SIZE       16U

enum image_opcode {
    IMAGE_OP_START = 1,
    IMAGE_OP_END = 2,
    IMAGE_OP_CANCEL = 3,
    IMAGE_OP_QUERY = 4,
};

enum image_state {
    IMAGE_STATE_OFF = 0,
    IMAGE_STATE_ADVERTISING = 1,
    IMAGE_STATE_CONNECTED = 2,
    IMAGE_STATE_RECEIVING = 3,
    IMAGE_STATE_VERIFYING = 4,
    IMAGE_STATE_COMPLETE = 5,
    IMAGE_STATE_ERROR = 6,
};

enum image_error {
    IMAGE_ERROR_NONE = 0,
    IMAGE_ERROR_VERSION = 1,
    IMAGE_ERROR_FRAME = 2,
    IMAGE_ERROR_PARAMETER = 3,
    IMAGE_ERROR_STATE = 4,
    IMAGE_ERROR_BUSY = 5,
    IMAGE_ERROR_OFFSET = 6,
    IMAGE_ERROR_SPACE = 7,
    IMAGE_ERROR_FILE_IO = 8,
    IMAGE_ERROR_LENGTH = 9,
    IMAGE_ERROR_CRC = 10,
    IMAGE_ERROR_FORMAT = 11,
    IMAGE_ERROR_NOT_FOUND = 12,
};
```

---

## 8. Control 帧

公共头为 8 字节：

| 偏移 | 长度 | 字段 | 说明 |
|---:|---:|---|---|
| 0 | 1 | version | 固定 1 |
| 1 | 1 | opcode | 1 至 4 |
| 2 | 2 | payload_len | 小端序 |
| 4 | 4 | session_id | 小端序且非零 |

先检查：实际长度至少 8、版本为 1、session_id 非零、opcode 合法、`实际长度 == 8 + payload_len`。

### 8.1 START

START 固定 24 字节，payload_len=16：

| 偏移 | 长度 | 字段 | 约束 |
|---:|---:|---|---|
| 0 | 8 | 公共头 | opcode=1 |
| 8 | 1 | slot | 0 至 9 |
| 9 | 1 | format | JPEG 固定为 1 |
| 10 | 2 | width | 1 至 466 |
| 12 | 2 | height | 1 至 466 |
| 14 | 2 | reserved | 必须为 0 |
| 16 | 4 | total_size | 1 至 307200 |
| 20 | 4 | crc32 | 整个 JPEG 的 CRC32 |

START 处理规则：

1. 验证所有参数并检查存储空间；
2. 没有活动会话时创建临时文件；
3. 相同 session_id 且元数据相同时视为重试，返回当前 next_offset，不清空已有数据；
4. 不同 session_id 占用设备时返回 `ERROR/BUSY`；
5. 成功后返回 `RECEIVING/NONE`，detail=total_size。

小程序 3 秒未收到 Status 会重发 START，所以 START 必须幂等。

### 8.2 END

END 为 8 字节，payload_len=0。处理流程：

1. 检查 session_id 和 `next_offset == total_size`；
2. Status 改为 VERIFYING，可先 Notify；
3. `fflush/fsync` 并关闭临时文件；
4. 校验长度、CRC32 和 JPEG；
5. 原子替换目标槽位文件；
6. Status 改为 COMPLETE/NONE；
7. next_offset=total_size，detail=slot；
8. Notify COMPLETE。

小程序等待 COMPLETE 最长约 12 秒。END 也应幂等：相同 session 的图片已提交时，重发 END 应再次返回 COMPLETE。

### 8.3 CANCEL

CANCEL 为 8 字节，payload_len=0。关闭并删除匹配 session 的临时文件，清理会话，回到 CONNECTED。不得删除已提交的槽位文件。

### 8.4 QUERY

QUERY 为 8 字节，payload_len=0：

- 找到相同未完成 session：返回 RECEIVING 和当前 next_offset；
- 未找到：返回 `ERROR/NOT_FOUND`；
- BLE 断开不能立即删除会话；
- ESP32 重启后允许返回 NOT_FOUND，小程序会重新 START。

---

## 9. Data 帧

Data 使用 Write Without Response：

| 偏移 | 长度 | 字段 | 说明 |
|---:|---:|---|---|
| 0 | 1 | version | 固定 1 |
| 1 | 1 | flags | 固定 0 |
| 2 | 2 | payload_len | 1 至 224 |
| 4 | 4 | session_id | 匹配活动会话 |
| 8 | 4 | offset | JPEG 字节偏移 |
| 12 | N | payload | JPEG 原始字节 |

必须验证：

```text
frame_len >= 13
version == 1
flags == 0
1 <= payload_len <= 224
frame_len == 12 + payload_len
session_id == active_session_id
payload_len <= total_size - offset
```

处理规则：

- `offset == next_offset`：写入 payload，写成功后推进 next_offset、更新 CRC，然后 Notify；
- `offset < next_offset` 且整个分片已落盘：这是 notification 丢失后的重发，不重复写、不重复累计 CRC，只重新 Notify 当前 next_offset；
- `offset > next_offset`：不写入，返回 `ERROR/OFFSET`，Status.next_offset 填真实偏移；
- 分片跨越 next_offset：第一版返回 `ERROR/OFFSET`，不要自行截断；
- 越过 total_size：返回 `ERROR/LENGTH`。

只有数据真正被文件系统接受后才能推进 next_offset。Data 没有 ATT Response，所有应用层结果必须通过 Status Notify 或 Status Read 暴露。小程序每 700 ms 还会主动 Read Status 作为 Notify 丢失时的兜底。

---

## 10. Status 帧

Status 固定 16 字节：

| 偏移 | 长度 | 字段 | 说明 |
|---:|---:|---|---|
| 0 | 1 | version | 固定 1 |
| 1 | 1 | state | 状态码 |
| 2 | 2 | error | 错误码 |
| 4 | 4 | session_id | 当前/相关会话 |
| 8 | 4 | next_offset | 已确认的下一偏移 |
| 12 | 4 | detail | 状态相关信息 |

detail 推荐值：RECEIVING/VERIFYING 填 total_size，COMPLETE 填 slot，其他状态没有附加信息时填 0。

```c
struct image_status_snapshot {
    uint8_t state;
    uint16_t error;
    uint32_t session_id;
    uint32_t next_offset;
    uint32_t detail;
};

static void encode_status(uint8_t out[IMAGE_STATUS_SIZE],
                          const struct image_status_snapshot *s)
{
    out[0] = IMAGE_PROTOCOL_VERSION;
    out[1] = s->state;
    write_le16(&out[2], s->error);
    write_le32(&out[4], s->session_id);
    write_le32(&out[8], s->next_offset);
    write_le32(&out[12], s->detail);
}
```

初始 Status 建议为：

```text
version=1, state=CONNECTED(2), error=NONE(0),
session_id=0, next_offset=0, detail=0
```

Read 回调应加锁复制快照，然后将 16 字节 append 到 `ctxt->om`。Notify 示例：

```c
static int notify_status(uint16_t conn_handle)
{
    uint8_t value[IMAGE_STATUS_SIZE];
    struct image_status_snapshot snapshot;

    image_status_get_snapshot(&snapshot);
    encode_status(value, &snapshot);
    struct os_mbuf *om = ble_hs_mbuf_from_flat(value, sizeof(value));
    if (om == NULL) {
        return BLE_HS_ENOMEM;
    }
    return ble_gatts_notify_custom(conn_handle, status_value_handle, om);
}
```

只在已连接且客户端已订阅时 Notify。GAP `BLE_GAP_EVENT_SUBSCRIBE` 中记录订阅状态。Notify 失败不能回滚已落盘的 next_offset，Status Read 仍应返回最新快照。

---

## 11. 状态和错误

| 状态值 | 名称 | 含义 |
|---:|---|---|
| 0 | OFF | 图片服务关闭 |
| 1 | ADVERTISING | 正在广播 |
| 2 | CONNECTED | 已连接，无活动传输 |
| 3 | RECEIVING | 正在接收 |
| 4 | VERIFYING | END 后校验/提交 |
| 5 | COMPLETE | 图片已提交 |
| 6 | ERROR | 查看 error |

| 错误值 | 名称 | 使用场景 |
|---:|---|---|
| 0 | NONE | 无错误 |
| 1 | VERSION | 协议版本错误 |
| 2 | FRAME | 帧长度、opcode、flags 错误 |
| 3 | PARAMETER | 槽位、尺寸、格式、长度错误 |
| 4 | STATE | 当前状态不允许操作 |
| 5 | BUSY | 其他 session 正在传输 |
| 6 | OFFSET | offset 不一致 |
| 7 | SPACE | 存储空间不足 |
| 8 | FILE_IO | 文件操作失败 |
| 9 | LENGTH | 分片/最终长度错误 |
| 10 | CRC | CRC32 不匹配 |
| 11 | FORMAT | JPEG 格式无效 |
| 12 | NOT_FOUND | QUERY 找不到 session |

错误 Status 应尽量填写触发错误的 session_id，next_offset 填 ESP32 当前真实偏移。除 OFFSET 外，大多数错误会终止小程序当前发送任务。

---

## 12. 固件状态机

```text
启动 -> 文件系统/GATT初始化 -> ADVERTISING
连接 -> CONNECTED
START -> 创建 slot-N.tmp -> RECEIVING(offset=0)
DATA -> 写文件 -> 更新 CRC/offset -> RECEIVING
END -> VERIFYING -> 校验/原子提交 -> COMPLETE
CANCEL -> 删除临时文件 -> CONNECTED
断开(RECEIVING) -> 保留会话 -> ADVERTISING
重连 + QUERY -> RECEIVING(offset=已有长度)
```

推荐会话结构：

```c
struct image_transfer_session {
    bool active;
    bool committed;
    uint32_t session_id;
    uint8_t slot;
    uint8_t format;
    uint16_t width;
    uint16_t height;
    uint32_t total_size;
    uint32_t expected_crc32;
    uint32_t next_offset;
    uint32_t running_crc32;
    FILE *file;
    char temp_path[48];
    char final_path[48];
};
```

还需保存 conn_handle、Status Notify 订阅状态、Status 快照、互斥锁和工作队列。第一版只需支持 BLE 断线后的内存续传，不要求 ESP32 重启续传。

---

## 13. BLE 回调与文件任务

不要在 NimBLE Host 回调中擦除 Flash、遍历文件、解码 JPEG或等待显示。推荐：

```text
NimBLE access callback
  -> 检查 ATT 写长度
  -> 把 mbuf 扁平复制到 work item
  -> 投递 FreeRTOS Queue
  -> 立即返回

image_transfer_task
  -> 解析协议
  -> 文件写入/校验/提交
  -> 更新 Status
  -> Notify
```

小程序采用 stop-and-wait，队列深度 4 至 8 通常足够。

```c
struct image_work_item {
    enum { IMAGE_WORK_CONTROL, IMAGE_WORK_DATA } type;
    uint16_t conn_handle;
    uint16_t length;
    uint8_t value[IMAGE_DATA_MAX_SIZE];
};

static int copy_write_value(struct ble_gatt_access_ctxt *ctxt,
                            uint8_t *out, uint16_t capacity,
                            uint16_t *out_len)
{
    uint16_t length = OS_MBUF_PKTLEN(ctxt->om);
    if (length > capacity) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    if (ble_hs_mbuf_to_flat(ctxt->om, out, capacity, out_len) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    return 0;
}
```

不能只读取 `ctxt->om->om_data`，较长写入可能由多段 mbuf 组成。Data 队列满时保持 next_offset 不变，小程序会超时重发；Control 队列满可返回 ATT resource error。

会话和 Status 由单一传输任务修改。Read 回调仅在短临界区复制 Status 快照。不要持锁调用 Notify 或 JPEG 解码。

---

## 14. Data 处理伪代码

```c
static void process_data(const uint8_t *f, size_t len)
{
    if (len < 13) { set_error(IMAGE_ERROR_FRAME, 0); return; }

    uint8_t version = f[0];
    uint8_t flags = f[1];
    uint16_t payload_len = read_le16(&f[2]);
    uint32_t session_id = read_le32(&f[4]);
    uint32_t offset = read_le32(&f[8]);
    const uint8_t *payload = &f[12];

    if (version != 1) {
        set_error(IMAGE_ERROR_VERSION, session_id); return;
    }
    if (flags != 0 || payload_len == 0 || payload_len > 224 ||
        len != 12U + payload_len) {
        set_error(IMAGE_ERROR_FRAME, session_id); return;
    }
    if (!session.active || session.session_id != session_id) {
        set_error(IMAGE_ERROR_STATE, session_id); return;
    }
    if (offset > session.next_offset) {
        set_offset_error(session.next_offset); return;
    }
    if (offset < session.next_offset) {
        if (offset + payload_len <= session.next_offset) {
            publish_receiving_status();  // 完整重复包
        } else {
            set_offset_error(session.next_offset);
        }
        return;
    }
    if (payload_len > session.total_size - session.next_offset) {
        set_error(IMAGE_ERROR_LENGTH, session_id); return;
    }
    if (fwrite(payload, 1, payload_len, session.file) != payload_len) {
        set_error(IMAGE_ERROR_FILE_IO, session_id); return;
    }

    session.running_crc32 = crc32_update(session.running_crc32,
                                         payload, payload_len);
    session.next_offset += payload_len;
    publish_receiving_status();
}
```

代码还应检查 `offset + payload_len` 的整数溢出。推荐写法是先确保 `offset <= total_size`，再判断 `payload_len > total_size - offset`。

---

## 15. CRC32

协议使用 CRC-32/ISO-HDLC：多项式 `0xEDB88320`、初值和最终异或都是 `0xFFFFFFFF`。CRC 只覆盖 JPEG payload，不包含 Control/Data 协议头。

```c
static uint32_t crc32_update(uint32_t crc,
                             const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1U) ? 0xedb88320U : 0U);
        }
    }
    return crc;
}
```

START 时 `running_crc32=0xFFFFFFFF`，每个新分片成功写入后更新；重复分片不能更新。END 时：

```c
uint32_t actual_crc32 = session.running_crc32 ^ 0xffffffffU;
```

标准测试向量：ASCII `123456789` 的 CRC32 应为 `0xCBF43926`。也可以在 END 时重新读取临时文件计算 CRC，以避免内存 CRC 状态与文件不一致；不要在 BLE Host 回调中执行。

---

## 16. 文件和 JPEG 提交

建议文件名：

```text
/images/slot-0.jpg ... /images/slot-9.jpg
/images/slot-2.tmp
```

END 至少检查：

1. 文件长度等于 total_size；
2. 前三字节为 `FF D8 FF`；
3. 末尾有 JPEG EOI `FF D9`；
4. CRC32 正确；
5. 可选：JPEG width/height 与 START 一致；
6. 可选：解码器支持该 JPEG 编码。

提交顺序：

```text
写 slot-N.tmp
 -> fflush/fsync
 -> 长度/CRC/JPEG 校验
 -> 原子替换 slot-N.jpg
 -> COMPLETE
```

校验失败不能覆盖旧图。根据文件系统确认 rename 的覆盖和原子性；必要时使用 `.bak` 文件完成可恢复替换。

图片显示应在 COMPLETE 后由独立任务刷新。COMPLETE 表示文件已可靠保存，不需要等待屏幕完成绘制。

---

## 17. 断线恢复和会话过期

BLE 断开时：

- 清除 conn_handle 和 Notify 订阅状态；
- 保留 session_id、next_offset、running_crc32 和临时文件；
- 重新广播；
- 重连后接受 QUERY。

ESP32 重启后允许不恢复。小程序 QUERY 时返回 NOT_FOUND，小程序会从 START/offset 0 重传。没有持久化完整元数据和 CRC 状态时，不要仅根据 `.tmp` 长度假装跨重启恢复。

可设置 5 至 30 分钟会话过期时间，过期后删除临时文件并让 QUERY 返回 NOT_FOUND。时间应明显大于正常 300 KiB 图片传输耗时。

---

## 18. GAP 事件

| GAP 事件 | 固件行为 |
|---|---|
| CONNECT 成功 | 保存 conn_handle，状态置 CONNECTED 或保留 RECEIVING |
| CONNECT 失败 | 重新广播 |
| DISCONNECT | 清连接/订阅状态，保留活动会话，重新广播 |
| SUBSCRIBE | 记录 Status Notify 是否开启 |
| MTU | 记录最终协商 MTU |
| ADV_COMPLETE | 按需要重新广播 |

连接参数可从 15 至 30 ms interval、0 latency、4 至 6 s supervision timeout 开始，再根据功耗和稳定性实测调整。

---

## 19. 完整时序

```text
小程序                                             ESP32
  |--- Connect / MTU / Discover -------------------->|
  |--- Subscribe Status + Read --------------------->|
  |<-- CONNECTED, session=0, offset=0 ---------------|
  |--- START(write), session=S, size=N, crc=C ------>|
  |<-- RECEIVING, session=S, offset=0, detail=N ------|
  |--- DATA(no rsp), offset=0 ---------------------->|
  |                                  写入并更新 CRC   |
  |<-- RECEIVING, offset=P ---------------------------|
  |--- DATA(no rsp), offset=P ---------------------->|
  |<-- RECEIVING, offset=2P --------------------------|
  |                 ...                              |
  |--- END(write), session=S ------------------------>|
  |<-- VERIFYING, offset=N ---------------------------| 可选
  |                       长度/CRC/JPEG/原子提交       |
  |<-- COMPLETE, offset=N, detail=slot ---------------|
```

小程序不会将 BLE Write API success 当作数据落盘，只认 Status.next_offset。

---

## 20. 十六进制报文示例

示例参数：session=`0x12345678`、slot=2、宽高=466、total=1000、CRC=`0x89ABCDEF`。

START：

```text
01 01 10 00 78 56 34 12 02 01 D2 01 D2 01 00 00
E8 03 00 00 EF CD AB 89
```

第一个 Data，payload=`FF D8 FF`：

```text
01 00 03 00 78 56 34 12 00 00 00 00 FF D8 FF
```

确认 3 字节，detail=1000：

```text
01 03 00 00 78 56 34 12 03 00 00 00 E8 03 00 00
```

END：

```text
01 02 00 00 78 56 34 12
```

COMPLETE，slot=2：

```text
01 05 00 00 78 56 34 12 E8 03 00 00 02 00 00 00
```

START 的 total 和 CRC 仅用于展示编码，不能用这组数据完成真实 END 校验。

---

## 21. 性能与日志

当前协议每个 Data 都等待一次 Status，属于 stop-and-wait。第一版不要改为多包并发，先保证重试、CRC 和断线恢复正确。

建议：

- MTU 247；
- 连接间隔 15 至 30 ms；
- BLE 回调仅复制和入队；
- 文件使用缓冲写入；
- JPEG显示使用独立任务；
- Control/Data 在 3 秒内确认；
- END 在 12 秒内返回 COMPLETE。

日志记录连接原因、MTU、订阅状态、opcode、session、slot、total_size、CRC、next_offset、文件错误和 END 耗时。不要记录 JPEG payload。300 KiB 图片有一千多个分片，不要逐片输出 INFO 日志；可每 16 KiB 或每 10% 打印一次。

---

## 22. 修改默认配置时

零改动兼容参数：

```text
设备名：SF-BADGE
UUID：本文原值
格式：Baseline JPEG
尺寸：最大 466x466
文件：最大 307200 字节
槽位：0 至 9
协议版本：1
```

- 改设备名：广播 Service UUID正确时仍可发现，但建议同步修改界面提示；
- 改 UUID：ESP32 和小程序四个 UUID必须一起改；
- 改为 240x240：同步修改小程序导出尺寸和固件校验；
- 改为 320x240 等非方屏：小程序当前单一 `BADGE_IMAGE_SIZE` 必须重构为 width/height；
- 改槽位或文件上限：同步修改小程序校验、UI、ESP32 校验和分区容量。

---

## 23. 联调步骤

### 23.1 GATT 基础检查

用 nRF Connect 等工具确认：

1. 广播包含正确 Service UUID；
2. Control 支持 Write；
3. Data 支持 Write Without Response；
4. Status 支持 Read 和 Notify；
5. Status Read 恰好返回 16 字节；
6. Notify 可以订阅；
7. MTU 至少 27，推荐 247。

### 23.2 手工协议检查

1. 读取初始 Status；
2. 写 START，确认 RECEIVING；
3. 写 Data，确认 next_offset 前进；
4. 重写同一 Data，确认不重复写且 offset 不变；
5. 写超前 offset，确认 ERROR/OFFSET 返回真实 offset；
6. CANCEL，确认只删除临时文件；
7. 用真实 JPEG 和正确 CRC 完成 END。

### 23.3 小程序端到端

- Android 和 iOS；
- 466x466 小图和接近 300 KiB 图片；
- 槽位 0、1、9；
- 覆盖已有槽位；
- 丢失一次 Status Notify；
- 发送中断开 BLE 后重连；
- ESP32 重启后 QUERY 返回 NOT_FOUND 并重传；
- CRC 错误、空间不足、最后一个短分片；
- 手机关闭蓝牙后 ESP32 重新广播。

---

## 24. 常见故障

### 扫描不到

检查广播是否启动、Service UUID 是否在 Advertising Data 中、128-bit UUID 数组是否写反、设备是否被其他手机连接。

### 连接后提示没有服务/属性不完整

检查 GATT 表 UUID；Control 是否有 Write；Data 是否有 Write Without Response；Status 是否同时有 Read/Notify；CCCD 是否存在。

### 未收到初始 Status

检查 Read 回调是否 append 恰好 16 字节、version 是否为 1、回调是否返回 ATT 错误、Status handle 是否正确。

### START 超时

ESP32 不能只返回 ATT Write Response，还必须 Notify Status；检查 START 是否按 24 字节和小端序解析、文件创建是否超过 3 秒、Status.session_id 是否匹配。

### Data 只传一片

检查落盘后是否 Notify 新 next_offset、next_offset 是否指向下一字节、mbuf 是否完整 flatten、Status.state 是否为 RECEIVING。

### CRC 总是错误

检查初值/终值、是否误用非反射算法、是否重复累计重发包、是否把 Data 头算入 CRC、START CRC 是否按小端解析、文件是否用 `"wb"` 打开。

### END 超时

检查 VERIFYING 后是否最终发送并保留 COMPLETE、session_id 是否匹配、校验是否阻塞 Host、Notify 丢失时 Read 是否仍返回 COMPLETE。不能在小程序读到 COMPLETE 前立即重置为 CONNECTED。

---

## 25. 安全与健壮性

BLE 输入必须按不可信数据处理：

- 所有长度先校验后访问；
- 用减法检查范围，避免 offset 加法溢出；
- 文件路径只能由已验证的 slot 生成；
- 限制文件长度和活动会话数；
- 图片校验成功前不交给解码器；
- 错误不能破坏已提交图片；
- 不打印用户图片内容。

当前图片协议本身没有身份认证。公开环境或商业产品应启用 BLE 配对/绑定，或增加独立的应用层认证特征。不要为了加认证随意改变现有图片帧格式，可以在接受 START 前增加连接授权状态。

---

## 26. 完成检查表

- [ ] ESP32 广播正确 Service UUID；
- [ ] 小程序能发现并连接；
- [ ] MTU不小于 27，推荐 247；
- [ ] 三个特征的属性正确；
- [ ] 初始 Status Read 返回有效 16 字节；
- [ ] START 重试不会清空同一会话；
- [ ] Data 按 offset 落盘并确认；
- [ ] 重复 Data 不重复写、不重复计算 CRC；
- [ ] 错误 offset 返回真实 next_offset；
- [ ] END 校验长度、CRC 和 JPEG；
- [ ] 提交失败不覆盖旧图片；
- [ ] COMPLETE 可被 Notify 或 Read 到；
- [ ] CANCEL 只删除临时文件；
- [ ] 断开后保留会话并重新广播；
- [ ] QUERY 可恢复 offset；
- [ ] 重启后 QUERY 至少正确返回 NOT_FOUND；
- [ ] 300 KiB 图片不导致内存溢出；
- [ ] Android/iOS 完成端到端测试；
- [ ] 保存后能异步解码并显示图片。

完成上述基础协议后，再考虑吞吐率优化、认证、动态分辨率或跨重启续传。基础联调阶段不要同时修改帧格式和发送窗口。
