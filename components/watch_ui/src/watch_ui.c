#include "watch_ui.h"

#include <stdio.h>                                   // 提供 snprintf()
#include <string.h>                                  // 提供 memset()、strlen()
#include <stdint.h>                                  // 提供 int16_t

#include "esp_log.h"                               // 提供菜单和锁定状态日志
#include "lvgl.h"                                  // 提供 LVGL 8 绘制、事件和定时器 API

#define WATCH_FACE_SIZE                    466       // CO5300 圆屏完整绘制尺寸
#define WATCH_FACE_CENTER                  233       // 未锁定表盘中心
#define WATCH_FACE_HOUR_RADIUS              89       // 小时盘半径，按满屏外圈同步放大
#define WATCH_FACE_MINUTE_RADIUS           161       // 分钟盘半径，按满屏外圈同步放大
#define WATCH_FACE_SECOND_RADIUS           233       // 秒钟盘半径，直达 466 x 466 屏幕四边
#define WATCH_FACE_TIMER_PERIOD_MS           8       // S3 局部刷新目标约 120 FPS 节奏
#define WATCH_FACE_LOCK_DURATION_MS        450       // 锁定/释放动画时长
#define WATCH_FACE_LOCK_PROGRESS_MAX      1000       // 动画原始进度上限
#define WATCH_FACE_TIP_WIDTH               150       // 首次操作提示宽度
#define WATCH_FACE_TIP_HEIGHT               28       // 首次操作提示高度

#define WATCH_MENU_CARD_WIDTH              320       // 已验证菜单卡片宽度
#define WATCH_MENU_CARD_HEIGHT              64       // 已验证菜单卡片高度
#define WATCH_MENU_ICON_SIZE                44       // 已验证菜单图标圆尺寸
#define WATCH_MENU_CARD_GAP                 12       // 已验证菜单卡片间距
#define WATCH_MENU_PADDING_Y                55       // 已验证菜单上下留白

#define WATCH_COLOR_BG       lv_color_hex(0x030508)  // 极光表盘深黑蓝背景
#define WATCH_COLOR_HOUR     lv_color_hex(0xFFFFFF)  // 小时盘纯白
#define WATCH_COLOR_MINUTE   lv_color_hex(0x00E5FF)  // 分钟盘冰蓝
#define WATCH_COLOR_SECOND   lv_color_hex(0x3A82F6)  // 秒钟盘钴蓝
#define WATCH_COLOR_MENU_BG  lv_color_hex(0x000000)  // 菜单黑色背景

typedef struct {
    lv_display_t *display;                           // 当前 UI 所属 Display
    lv_obj_t *main_page;                             // 极光主表盘 screen
    lv_obj_t *face_draw;                             // 单一自定义绘制对象，无像素缓冲
    lv_obj_t *menu_page;                             // 设置菜单独立 screen
    lv_obj_t *menu_list;                             // 可滚动设置列表
    lv_timer_t *animation_timer;                     // LVGL 平滑刷新定时器
    watch_ui_data_t time_snapshot;                   // 最近合法 RTC 快照
    uint32_t snapshot_tick;                          // 快照对应的 LVGL tick
    uint32_t frame_tick;                             // 上一动画帧 tick
    uint16_t lock_progress;                          // 0-1000 锁定原始进度
    bool time_valid;                                 // 是否已有合法 RTC 快照
    bool lock_target;                                // true 表示目标为锁定状态
    bool tip_visible;                                // 首次操作提示是否可见
    bool menu_visible;                               // 当前是否显示菜单
    lv_area_t dynamic_areas[3];                      // 未锁定三根指针上一帧脏区域
    bool dynamic_area_valid;                         // 上一帧脏区域是否有效
    bool redraw_full_next;                           // 状态切换后强制完整重绘一次
} watch_ui_runtime_t;

typedef struct {
    float hour_value;                                // 连续 12 小时制小时值
    float minute_value;                              // 包含秒小数的分钟值
    float second_value;                              // 包含毫秒小数的秒值
    uint8_t hour;                                    // HUD 24 小时制小时
    uint8_t minute;                                  // HUD 分钟
    uint8_t second;                                  // HUD 秒钟
    uint8_t centisecond;                             // HUD 百分之一秒
    float eased_lock;                                // cubic 缓动后的锁定进度
    float scale;                                     // 当前缩放比例
    lv_point_t center;                               // 当前表盘中心
} watch_face_draw_state_t;

typedef struct {
    const char *label;                               // 菜单项文字
    const char *symbol;                              // LVGL 内置图标
    uint32_t color;                                  // 图标圆 RGB888 颜色
} watch_menu_item_spec_t;

static watch_ui_runtime_t s_ui = {0};                // watch_ui 唯一私有运行状态
static const char *TAG = "WATCH_UI";                // 本模块日志标签

/*
 * 未锁定时三层表盘共用同一组 6 度刻度几何。预先缓存 LVGL 查表结果，
 * 避免每一帧为 180 个刻度重复调用 720 次三角函数查表/插值。
 */
static int16_t s_tick_cos[60];
static int16_t s_tick_sin[60];

static const char *const s_hour_labels[12] = {
    "12", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11"
};
static const char *const s_minute_labels[12] = {
    "00", "05", "10", "15", "20", "25", "30", "35", "40", "45", "50", "55"
};

static const watch_menu_item_spec_t s_menu_items[] = {
    {"Display",       LV_SYMBOL_EYE_OPEN,   0xFFAA00},
    {"Vibration",     LV_SYMBOL_VOLUME_MID, 0x00D285},
    {"DND Mode",      LV_SYMBOL_MUTE,       0x7C5CFF},
    {"Power Save",    LV_SYMBOL_CHARGE,     0x2ED573},
    {"Notifications", LV_SYMBOL_BELL,       0xFF4757},
    {"System",        LV_SYMBOL_SETTINGS,   0x1E90FF},
};

/**
 * @brief 将浮点屏幕坐标四舍五入为 LVGL 坐标。
 * @param value 浮点坐标。
 * @return 最接近 value 的 LVGL 坐标。
 * @note 纯数学辅助函数，不访问 LVGL 对象或硬件。
 */
static lv_coord_t round_coord(float value)
{
    return (lv_coord_t)(value >= 0.0f ? value + 0.5f : value - 0.5f);
}

/**
 * @brief 把角度归一化到 0 至 360 度区间。
 * @param angle 任意角度。
 * @return 与 angle 等价的 0.0-360.0 度角度。
 * @note 纯数学辅助函数，不访问共享状态。
 */
static float normalize_angle(float angle)
{
    while (angle < 0.0f) {
        angle += 360.0f;
    }
    while (angle >= 360.0f) {
        angle -= 360.0f;
    }
    return angle;
}

/**
 * @brief 计算指定角度到水平向右极轴的最小夹角。
 * @param angle 需要比较的角度。
 * @return 0.0-180.0 度的无符号最小夹角。
 * @note 锁定状态使用该值衰减远离极轴的刻度。
 */
static float focus_angle(float angle)
{
    const float normalized = normalize_angle(angle);
    return normalized > 180.0f ? 360.0f - normalized : normalized;
}

/**
 * @brief 在线性插值相邻整度查表值后返回平滑三角函数结果。
 * @param angle_deg 任意浮点角度。
 * @param cosine true 计算余弦，false 计算正弦。
 * @return 归一化到 -1.0 至 1.0 的三角函数结果。
 * @note 使用 LVGL 查表 API，避免引入 libm，同时保留亚角度秒针动画。
 */
static float interpolated_trig(float angle_deg, bool cosine)
{
    const float normalized = normalize_angle(angle_deg);
    const int16_t lower_angle = (int16_t)normalized;
    const float fraction = normalized - (float)lower_angle;
    const int16_t lower = cosine ?
        lv_trigo_cos(lower_angle) : lv_trigo_sin(lower_angle);
    const int16_t upper = cosine ?
        lv_trigo_cos(lower_angle + 1) : lv_trigo_sin(lower_angle + 1);
    return ((float)lower + ((float)upper - (float)lower) * fraction) /
           (float)LV_TRIGO_SIN_MAX;
}

/**
 * @brief 根据中心、半径和角度计算屏幕坐标。
 * @param center 极坐标中心。
 * @param radius 已包含锁定缩放的半径。
 * @param angle_deg 角度；0 度向右，90 度向下。
 * @return 对应的 LVGL 屏幕坐标。
 * @note 使用浮点三角函数保留 16 ms 刷新节奏下的秒针插值精度。
 */
static lv_point_t polar_point(
    lv_point_t center,
    float radius,
    float angle_deg)
{
    const lv_point_t point = {
        .x = center.x + round_coord(radius * interpolated_trig(angle_deg, true)),
        .y = center.y + round_coord(radius * interpolated_trig(angle_deg, false)),
    };
    return point;
}

/**
 * @brief 预计算未锁定表盘的整数刻度三角函数值。
 * @return 无返回值。
 * @note 只在 ui_init() 的 LVGL 上下文中调用一次，不访问硬件。
 */
static void prepare_tick_geometry(void)
{
    for (uint8_t index = 0; index < 60U; ++index) {
        const int16_t angle = (int16_t)((index * 6U + 270U) % 360U);
        s_tick_cos[index] = lv_trigo_cos(angle);
        s_tick_sin[index] = lv_trigo_sin(angle);
    }
}

/**
 * @brief 使用缓存三角函数计算未锁定刻度坐标。
 * @param center 当前表盘中心。
 * @param radius 当前半径。
 * @param index 0-59 的 6 度刻度索引。
 * @return 对应的 LVGL 屏幕坐标。
 * @note 只用于静态几何路径，锁定动画仍使用 polar_point() 保持亚角度精度。
 */
static lv_point_t cached_polar_point(
    lv_point_t center,
    float radius,
    uint8_t index)
{
    const float scale = 1.0f / (float)LV_TRIGO_SIN_MAX;
    const lv_point_t point = {
        .x = center.x + round_coord(radius * (float)s_tick_cos[index] * scale),
        .y = center.y + round_coord(radius * (float)s_tick_sin[index] * scale),
    };
    return point;
}

/**
 * @brief 用一次旋转三角函数复用所有刻度的基准三角值。
 * @param center 当前表盘中心。
 * @param radius 当前半径。
 * @param index 0-59 的 6 度刻度索引。
 * @param rotation_cos 盘面旋转角余弦。
 * @param rotation_sin 盘面旋转角正弦。
 * @return 对应的 LVGL 屏幕坐标。
 * @note 锁定动画每层只计算一次旋转三角函数，避免每个刻度重复查表。
 */
static lv_point_t rotated_cached_polar_point(
    lv_point_t center,
    float radius,
    uint8_t index,
    float rotation_cos,
    float rotation_sin)
{
    const float base_cos = (float)s_tick_cos[index] /
                           (float)LV_TRIGO_SIN_MAX;
    const float base_sin = (float)s_tick_sin[index] /
                           (float)LV_TRIGO_SIN_MAX;
    const float cos_value = base_cos * rotation_cos - base_sin * rotation_sin;
    const float sin_value = base_sin * rotation_cos + base_cos * rotation_sin;
    const lv_point_t point = {
        .x = center.x + round_coord(radius * cos_value),
        .y = center.y + round_coord(radius * sin_value),
    };
    return point;
}

/**
 * @brief 对锁定进度应用与 HTML 一致的 cubic ease-in-out。
 * @param progress 0-WATCH_FACE_LOCK_PROGRESS_MAX 原始进度。
 * @return 0.0-1.0 的缓动结果。
 * @note 纯数学辅助函数，不访问 LVGL 或硬件。
 */
static float ease_in_out_cubic(uint16_t progress)
{
    const float x = (float)progress / (float)WATCH_FACE_LOCK_PROGRESS_MAX;
    if (x < 0.5f) {
        return 4.0f * x * x * x;
    }

    const float inverse = -2.0f * x + 2.0f;
    return 1.0f - (inverse * inverse * inverse) / 2.0f;
}

/**
 * @brief 将浮点透明度限制并转换为 LVGL 透明度。
 * @param value 可能越过 0-255 的透明度。
 * @return 限制后的 LVGL 透明度。
 * @note 只用于绘制描述符赋值。
 */
static lv_opa_t clamp_opa(float value)
{
    if (value <= 0.0f) {
        return LV_OPA_TRANSP;
    }
    if (value >= 255.0f) {
        return LV_OPA_COVER;
    }
    return (lv_opa_t)(value + 0.5f);
}

/**
 * @brief 根据 RTC 快照和 LVGL tick 构建本帧平滑绘制状态。
 * @param state 输出时间、缩放、缓动和中心坐标。
 * @param area 自定义绘制对象的绝对坐标。
 * @return 无返回值；RTC 无效时输出零位时间。
 * @note 必须在 LVGL 上下文调用，只读取 watch_ui 私有状态。
 */
static void build_draw_state(
    watch_face_draw_state_t *state,
    const lv_area_t *area)
{
    memset(state, 0, sizeof(*state));

    if (s_ui.time_valid) {
        const uint64_t base_ms =
            ((uint64_t)s_ui.time_snapshot.hour * 3600ULL +
             (uint64_t)s_ui.time_snapshot.minute * 60ULL +
             (uint64_t)s_ui.time_snapshot.second) * 1000ULL;
        const uint64_t current_ms =
            (base_ms + lv_tick_elaps(s_ui.snapshot_tick)) %
            (24ULL * 60ULL * 60ULL * 1000ULL);

        state->hour = (uint8_t)(current_ms / 3600000ULL);
        state->minute = (uint8_t)((current_ms / 60000ULL) % 60ULL);
        state->second = (uint8_t)((current_ms / 1000ULL) % 60ULL);
        state->centisecond = (uint8_t)((current_ms % 1000ULL) / 10ULL);
        state->second_value = (float)(current_ms % 60000ULL) / 1000.0f;
        state->minute_value = (float)(current_ms % 3600000ULL) / 60000.0f;
        state->hour_value = (float)(current_ms % 43200000ULL) / 3600000.0f;
    }

    state->eased_lock = ease_in_out_cubic(s_ui.lock_progress);
    state->scale = 1.0f + 0.22f * state->eased_lock;
    const float camera_x =
        -(float)WATCH_FACE_SECOND_RADIUS * 0.72f * state->eased_lock;
    state->center.x =
        area->x1 + WATCH_FACE_CENTER + round_coord(camera_x * state->scale);
    state->center.y = area->y1 + WATCH_FACE_CENTER;
}

/**
 * @brief 绘制统一风格的直线。
 * @param draw_ctx 当前 LVGL 绘制上下文。
 * @param start 起点。
 * @param end 终点。
 * @param color 颜色。
 * @param opa 透明度。
 * @param width 线宽。
 * @param rounded 是否使用圆头。
 * @return 无返回值。
 * @note 只能在 LV_EVENT_DRAW_MAIN 中调用。
 */
static void draw_line(
    lv_draw_ctx_t *draw_ctx,
    lv_point_t start,
    lv_point_t end,
    lv_color_t color,
    lv_opa_t opa,
    lv_coord_t width,
    bool rounded)
{
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = color;
    dsc.opa = opa;
    dsc.width = width > 0 ? width : 1;
    dsc.round_start = rounded;
    dsc.round_end = rounded;
    lv_draw_line(draw_ctx, &dsc, &start, &end);
}

/**
 * @brief 绘制实心圆。
 * @param draw_ctx 当前 LVGL 绘制上下文。
 * @param center 圆心。
 * @param radius 半径。
 * @param color 填充颜色。
 * @param opa 填充透明度。
 * @return 无返回值。
 * @note 只能在 LV_EVENT_DRAW_MAIN 中调用。
 */
static void draw_filled_circle(
    lv_draw_ctx_t *draw_ctx,
    lv_point_t center,
    lv_coord_t radius,
    lv_color_t color,
    lv_opa_t opa)
{
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = LV_RADIUS_CIRCLE;
    dsc.bg_color = color;
    dsc.bg_opa = opa;
    const lv_area_t circle = {
        .x1 = center.x - radius,
        .y1 = center.y - radius,
        .x2 = center.x + radius,
        .y2 = center.y + radius,
    };
    lv_draw_rect(draw_ctx, &dsc, &circle);
}

/**
 * @brief 在指定区域绘制单行文本。
 * @param draw_ctx 当前 LVGL 绘制上下文。
 * @param area 文本区域。
 * @param text 零结尾文本。
 * @param font 已启用字体。
 * @param color 文本颜色。
 * @param opa 文本透明度。
 * @param align 水平对齐方式。
 * @return 无返回值。
 * @note text 必须在同步绘制返回前保持有效。
 */
static void draw_text(
    lv_draw_ctx_t *draw_ctx,
    const lv_area_t *area,
    const char *text,
    const lv_font_t *font,
    lv_color_t color,
    lv_opa_t opa,
    lv_text_align_t align)
{
    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.font = font;
    dsc.color = color;
    dsc.opa = opa;
    dsc.align = align;
    dsc.letter_space = 0;
    lv_draw_label(draw_ctx, &dsc, area, text, NULL);
}

/**
 * @brief 判断绘制区域是否与当前 LVGL 刷新裁剪区相交。
 * @param draw_ctx 当前 LVGL 绘制上下文。
 * @param area 待绘制区域。
 * @return true 表示至少有一个像素需要绘制。
 * @note 用于局部刷新时跳过完全位于脏区外的静态刻度。
 */
static bool area_visible_in_clip(
    const lv_draw_ctx_t *draw_ctx,
    const lv_area_t *area)
{
    if (draw_ctx == NULL || draw_ctx->clip_area == NULL || area == NULL) {
        return true;
    }

    return !(area->x2 < draw_ctx->clip_area->x1 ||
             area->x1 > draw_ctx->clip_area->x2 ||
             area->y2 < draw_ctx->clip_area->y1 ||
             area->y1 > draw_ctx->clip_area->y2);
}

/**
 * @brief 将两点连线的包围盒写入区域并扩展线宽余量。
 * @param area 输出区域。
 * @param start 线段起点。
 * @param end 线段终点。
 * @param padding 线宽、圆头和抗锯齿余量。
 * @return 无返回值。
 * @note 只用于局部刷新脏区计算和绘制裁剪。
 */
static void line_area(
    lv_area_t *area,
    lv_point_t start,
    lv_point_t end,
    lv_coord_t padding)
{
    area->x1 = start.x < end.x ? start.x : end.x;
    area->x2 = start.x > end.x ? start.x : end.x;
    area->y1 = start.y < end.y ? start.y : end.y;
    area->y2 = start.y > end.y ? start.y : end.y;
    area->x1 -= padding;
    area->x2 += padding;
    area->y1 -= padding;
    area->y2 += padding;
}

/**
 * @brief 绘制一层可旋转刻度盘和对应时间指针。
 * @param draw_ctx 当前 LVGL 绘制上下文。
 * @param state 本帧时间和动画状态。
 * @param radius 未缩放半径。
 * @param max_value 一周最大值，小时为 12，其余为 60。
 * @param current_value 当前连续值。
 * @param color 本层主题颜色。
 * @param is_hour 是否使用 1-12 主刻度文字。
 * @param hand_width 未缩放指针宽度。
 * @return 无返回值。
 * @note 未锁定时使用缓存刻度几何；锁定动画复用旋转三角值，指针保留浮点插值精度。
 */
static void draw_dial_layer(
    lv_draw_ctx_t *draw_ctx,
    const watch_face_draw_state_t *state,
    float radius,
    float max_value,
    float current_value,
    lv_color_t color,
    bool is_hour,
    float hand_width)
{
    const float scaled_radius = radius * state->scale;
    const float native_current =
        current_value / max_value * 360.0f - 90.0f;
    const float dial_rotation = -native_current * state->eased_lock;
    const bool static_geometry = state->eased_lock <= 0.0001f;
    const float rotation_cos = static_geometry ?
        1.0f : interpolated_trig(dial_rotation, true);
    const float rotation_sin = static_geometry ?
        0.0f : interpolated_trig(dial_rotation, false);

    lv_draw_line_dsc_t tick_dsc;
    lv_draw_line_dsc_init(&tick_dsc);
    tick_dsc.color = color;
    tick_dsc.round_start = false;
    tick_dsc.round_end = false;

    lv_draw_label_dsc_t label_dsc;
    lv_draw_label_dsc_init(&label_dsc);
    label_dsc.font = &lv_font_montserrat_14;
    label_dsc.color = color;
    label_dsc.align = LV_TEXT_ALIGN_CENTER;
    label_dsc.letter_space = 0;

    lv_draw_arc_dsc_t arc_dsc;
    lv_draw_arc_dsc_init(&arc_dsc);
    arc_dsc.color = WATCH_COLOR_HOUR;
    arc_dsc.opa = 10;
    arc_dsc.width = 1;
    const lv_coord_t arc_radius = (lv_coord_t)round_coord(scaled_radius + 2.0f);
    const lv_area_t arc_area = {
        .x1 = state->center.x - arc_radius,
        .y1 = state->center.y - arc_radius,
        .x2 = state->center.x + arc_radius,
        .y2 = state->center.y + arc_radius,
    };
    if (area_visible_in_clip(draw_ctx, &arc_area)) {
        lv_draw_arc(draw_ctx, &arc_dsc, &state->center,
                    (uint16_t)round_coord(scaled_radius), 0, 360);
    }

    for (uint8_t index = 0; index < 60U; ++index) {
        const bool major = (index % 5U) == 0U;
        const float angle = (float)index * 6.0f - 90.0f + dial_rotation;
        float opacity = 1.0f;
        if (state->eased_lock > 0.01f) {
            float falloff = 1.0f - focus_angle(angle) / (180.0f / 3.2f);
            if (falloff < 0.0f) {
                falloff = 0.0f;
            }
            opacity = 0.15f + 0.85f * falloff * falloff;
        }

        const float tick_length = major ? 10.0f : 4.0f;
        const lv_point_t outer = static_geometry ?
            cached_polar_point(state->center, scaled_radius, index) :
            rotated_cached_polar_point(state->center, scaled_radius, index,
                                       rotation_cos, rotation_sin);
        const lv_point_t inner = static_geometry ?
            cached_polar_point(state->center,
                               (radius - tick_length) * state->scale,
                               index) :
            rotated_cached_polar_point(state->center,
                                       (radius - tick_length) * state->scale,
                                       index, rotation_cos, rotation_sin);
        tick_dsc.opa = clamp_opa(255.0f * opacity);
        tick_dsc.width = round_coord((major ? 2.0f : 1.0f) * state->scale);
        if (tick_dsc.width <= 0) {
            tick_dsc.width = 1;
        }
        lv_area_t tick_area;
        line_area(&tick_area, outer, inner, tick_dsc.width + 2);
        if (area_visible_in_clip(draw_ctx, &tick_area)) {
            lv_draw_line(draw_ctx, &tick_dsc, &outer, &inner);
        }

        if (major) {
            const char *number = is_hour ?
                s_hour_labels[index / 5U] : s_minute_labels[index / 5U];

            const float distance = 18.0f + 8.0f * state->eased_lock;
            const float text_radius =
                (radius - tick_length - distance) * state->scale;
            const lv_point_t text_center = static_geometry ?
                cached_polar_point(state->center, text_radius, index) :
                rotated_cached_polar_point(state->center, text_radius, index,
                                           rotation_cos, rotation_sin);
            const lv_area_t text_area = {
                .x1 = text_center.x - 19,
                .y1 = text_center.y - 8,
                .x2 = text_center.x + 19,
                .y2 = text_center.y + 9,
            };
            label_dsc.opa = clamp_opa(255.0f * opacity);
            if (area_visible_in_clip(draw_ctx, &text_area)) {
                lv_draw_label(draw_ctx, &label_dsc, &text_area, number, NULL);
            }
        }
    }

    const float hand_angle = native_current * (1.0f - state->eased_lock);
    const lv_point_t hand_end = polar_point(state->center, scaled_radius, hand_angle);
    tick_dsc.opa = LV_OPA_COVER;
    tick_dsc.width = round_coord(hand_width * state->scale);
    if (tick_dsc.width <= 0) {
        tick_dsc.width = 1;
    }
    tick_dsc.round_start = true;
    tick_dsc.round_end = true;
    lv_area_t hand_area;
    line_area(&hand_area, state->center, hand_end, tick_dsc.width + 3);
    if (area_visible_in_clip(draw_ctx, &hand_area)) {
        lv_draw_line(draw_ctx, &tick_dsc, &state->center, &hand_end);
    }
}

/**
 * @brief 计算未锁定状态三根指针和中心轴的最小动态脏区。
 * @param state 当前表盘绘制状态。
 * @param area 输出的 Display 绝对坐标区域。
 * @return 无返回值。
 * @note 未锁定表盘的刻度和文字不变，因此只需刷新该区域即可。
 */
static void build_unlocked_dynamic_areas(
    const watch_face_draw_state_t *state,
    lv_area_t areas[3])
{
    const float radii[] = {
        WATCH_FACE_HOUR_RADIUS,
        WATCH_FACE_MINUTE_RADIUS,
        WATCH_FACE_SECOND_RADIUS,
    };
    const float max_values[] = {12.0f, 60.0f, 60.0f};
    const float current_values[] = {
        state->hour_value,
        state->minute_value,
        state->second_value,
    };
    const float hand_widths[] = {4.5f, 2.5f, 1.5f};

    for (uint8_t index = 0; index < 3U; ++index) {
        const float angle =
            current_values[index] / max_values[index] * 360.0f - 90.0f;
        const lv_point_t hand_end = polar_point(
            state->center,
            radii[index] * state->scale,
            angle
        );
        line_area(&areas[index], state->center, hand_end,
                  round_coord(hand_widths[index] * state->scale) + 3);
    }
}

/**
 * @brief 绘制一个 HUD 文本片段并返回下一片段横坐标。
 * @param draw_ctx 当前 LVGL 绘制上下文。
 * @param text_x 当前横坐标。
 * @param baseline_y 激光线纵坐标。
 * @param text 文本片段。
 * @param font 字体。
 * @param color 颜色。
 * @param opa 透明度。
 * @param gap 片段后的像素间距。
 * @return 下一片段起始横坐标。
 * @note 只能在锁定 HUD 同步绘制期间调用。
 */
static lv_coord_t draw_hud_segment(
    lv_draw_ctx_t *draw_ctx,
    lv_coord_t text_x,
    lv_coord_t baseline_y,
    const char *text,
    const lv_font_t *font,
    lv_color_t color,
    lv_opa_t opa,
    lv_coord_t gap)
{
    const lv_coord_t width = lv_txt_get_width(
        text,
        (uint32_t)strlen(text),
        font,
        0,
        LV_TEXT_FLAG_NONE
    );
    const lv_area_t area = {
        .x1 = text_x,
        .y1 = baseline_y - 27,
        .x2 = text_x + width + 1,
        .y2 = baseline_y - 3,
    };
    draw_text(draw_ctx, &area, text, font, color, opa, LV_TEXT_ALIGN_LEFT);
    return text_x + width + gap;
}

/**
 * @brief 绘制锁定状态的极轴和三色数字 HUD。
 * @param draw_ctx 当前 LVGL 绘制上下文。
 * @param state 本帧时间和动画状态。
 * @param area 全屏绘制区域。
 * @return 无返回值；未进入锁定过渡时不绘制。
 * @note 文本缓冲只在同步绘制返回前有效。
 */
static void draw_lock_hud(
    lv_draw_ctx_t *draw_ctx,
    const watch_face_draw_state_t *state,
    const lv_area_t *area)
{
    if (state->eased_lock <= 0.001f) {
        return;
    }

    const lv_coord_t y = state->center.y;
    const lv_coord_t start = state->center.x - round_coord(70.0f * state->scale);
    const lv_coord_t middle = state->center.x + round_coord(88.0f * state->scale);
    const lv_coord_t end = area->x2 - 18;
    draw_line(draw_ctx, (lv_point_t){start, y}, (lv_point_t){state->center.x, y},
              WATCH_COLOR_HOUR, clamp_opa(95.0f * state->eased_lock), 1, false);
    draw_line(draw_ctx, (lv_point_t){state->center.x, y}, (lv_point_t){middle, y},
              WATCH_COLOR_MINUTE, clamp_opa(205.0f * state->eased_lock), 1, false);
    draw_line(draw_ctx, (lv_point_t){middle, y}, (lv_point_t){end, y},
              WATCH_COLOR_SECOND, clamp_opa(180.0f * state->eased_lock), 1, false);

    char hour_text[4];
    char minute_text[4];
    char second_text[4];
    char fraction_text[5];
    if (s_ui.time_valid) {
        (void)snprintf(hour_text, sizeof(hour_text), "%02u", (unsigned int)state->hour);
        (void)snprintf(minute_text, sizeof(minute_text), "%02u", (unsigned int)state->minute);
        (void)snprintf(second_text, sizeof(second_text), "%02u", (unsigned int)state->second);
        (void)snprintf(fraction_text, sizeof(fraction_text), ".%02u",
                       (unsigned int)state->centisecond);
    } else {
        (void)snprintf(hour_text, sizeof(hour_text), "--");
        (void)snprintf(minute_text, sizeof(minute_text), "--");
        (void)snprintf(second_text, sizeof(second_text), "--");
        (void)snprintf(fraction_text, sizeof(fraction_text), ".--");
    }

    const lv_opa_t opa =
        clamp_opa(255.0f * state->eased_lock * state->eased_lock);
    lv_coord_t x = state->center.x +
        round_coord(((float)WATCH_FACE_SECOND_RADIUS + 16.0f) * state->scale);
    const lv_coord_t hud_width =
        lv_txt_get_width(hour_text, (uint32_t)strlen(hour_text),
                         &lv_font_montserrat_18, 0, LV_TEXT_FLAG_NONE) + 2 +
        lv_txt_get_width(":", 1, &lv_font_montserrat_18, 0, LV_TEXT_FLAG_NONE) + 2 +
        lv_txt_get_width(minute_text, (uint32_t)strlen(minute_text),
                         &lv_font_montserrat_18, 0, LV_TEXT_FLAG_NONE) + 2 +
        lv_txt_get_width(":", 1, &lv_font_montserrat_18, 0, LV_TEXT_FLAG_NONE) + 2 +
        lv_txt_get_width(second_text, (uint32_t)strlen(second_text),
                         &lv_font_montserrat_18, 0, LV_TEXT_FLAG_NONE) + 3 +
        lv_txt_get_width(fraction_text, (uint32_t)strlen(fraction_text),
                         &lv_font_montserrat_14, 0, LV_TEXT_FLAG_NONE);
    const lv_coord_t hud_limit = area->x2 - 18;
    if (x + hud_width > hud_limit) {
        x = hud_limit - hud_width;
    }
    if (x < area->x1 + 4) {
        x = area->x1 + 4;
    }
    x = draw_hud_segment(draw_ctx, x, y, hour_text, &lv_font_montserrat_18,
                         WATCH_COLOR_HOUR, opa, 2);
    x = draw_hud_segment(draw_ctx, x, y, ":", &lv_font_montserrat_18,
                         lv_color_hex(0x89919C), opa, 2);
    x = draw_hud_segment(draw_ctx, x, y, minute_text, &lv_font_montserrat_18,
                         WATCH_COLOR_MINUTE, opa, 2);
    x = draw_hud_segment(draw_ctx, x, y, ":", &lv_font_montserrat_18,
                         lv_color_hex(0x89919C), opa, 2);
    x = draw_hud_segment(draw_ctx, x, y, second_text, &lv_font_montserrat_18,
                         WATCH_COLOR_SECOND, opa, 3);
    (void)draw_hud_segment(draw_ctx, x, y, fraction_text, &lv_font_montserrat_14,
                           lv_color_hex(0xB5BBC4), opa, 0);
}

/**
 * @brief 绘制中心双层轴点。
 * @param draw_ctx 当前 LVGL 绘制上下文。
 * @param state 本帧中心和缩放状态。
 * @return 无返回值。
 * @note 最后绘制以覆盖三根指针起点。
 */
static void draw_center_hub(
    lv_draw_ctx_t *draw_ctx,
    const watch_face_draw_state_t *state)
{
    draw_filled_circle(draw_ctx, state->center,
                       round_coord(4.5f * state->scale),
                       WATCH_COLOR_SECOND, LV_OPA_COVER);
    draw_filled_circle(draw_ctx, state->center,
                       round_coord(2.0f * state->scale),
                       lv_color_hex(0x000000), LV_OPA_COVER);
}

/**
 * @brief 绘制首次触摸前的底部操作提示。
 * @param draw_ctx 当前 LVGL 绘制上下文。
 * @param area 全屏绘制区域。
 * @return 无返回值；提示隐藏后直接返回。
 * @note 使用现有 Montserrat 14 字体，不增加字体资源。
 */
static void draw_lock_tip(
    lv_draw_ctx_t *draw_ctx,
    const lv_area_t *area)
{
    if (!s_ui.tip_visible) {
        return;
    }

    const lv_coord_t x1 =
        area->x1 + (WATCH_FACE_SIZE - WATCH_FACE_TIP_WIDTH) / 2;
    const lv_coord_t y1 = area->y2 - 26 - WATCH_FACE_TIP_HEIGHT + 1;
    const lv_area_t tip = {
        .x1 = x1,
        .y1 = y1,
        .x2 = x1 + WATCH_FACE_TIP_WIDTH - 1,
        .y2 = y1 + WATCH_FACE_TIP_HEIGHT - 1,
    };

    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = WATCH_FACE_TIP_HEIGHT / 2;
    dsc.bg_color = lv_color_hex(0x080D16);
    dsc.bg_opa = 190;
    dsc.border_color = WATCH_COLOR_HOUR;
    dsc.border_opa = 30;
    dsc.border_width = 1;
    lv_draw_rect(draw_ctx, &dsc, &tip);

    lv_area_t text_area = tip;
    text_area.y1 += 6;
    draw_text(draw_ctx, &text_area, "TAP TO LOCK", &lv_font_montserrat_14,
              lv_color_hex(0xA6ADB7), 210, LV_TEXT_ALIGN_CENTER);
}

/**
 * @brief 响应主表盘的 LVGL 自定义绘制事件。
 * @param event LVGL 绘制事件。
 * @return 无返回值；上下文或对象无效时忽略本帧。
 * @note 回调已经位于 LVGL 上下文，不调用 watch_lvgl_run()。
 */
static void draw_watch_face_event(lv_event_t *event)
{
    lv_draw_ctx_t *draw_ctx = lv_event_get_draw_ctx(event);
    lv_obj_t *face = lv_event_get_target(event);
    if (draw_ctx == NULL || face == NULL) {
        return;
    }

    lv_area_t area;
    lv_obj_get_coords(face, &area);
    watch_face_draw_state_t state;
    build_draw_state(&state, &area);

    /* 主 screen 已提供 #030508 背景，这里从表盘几何开始，避免重复整屏填充。 */
    draw_dial_layer(draw_ctx, &state, WATCH_FACE_HOUR_RADIUS, 12.0f,
                    state.hour_value, WATCH_COLOR_HOUR, true, 4.5f);
    draw_dial_layer(draw_ctx, &state, WATCH_FACE_MINUTE_RADIUS, 60.0f,
                    state.minute_value, WATCH_COLOR_MINUTE, false, 2.5f);
    draw_dial_layer(draw_ctx, &state, WATCH_FACE_SECOND_RADIUS, 60.0f,
                    state.second_value, WATCH_COLOR_SECOND, false, 1.5f);
    draw_lock_hud(draw_ctx, &state, &area);
    draw_center_hub(draw_ctx, &state);
    draw_lock_tip(draw_ctx, &area);
}

/**
 * @brief 切换主表盘的极轴锁定目标状态。
 * @param event LVGL 主表盘按压事件。
 * @return 无返回值。
 * @note 运行在 LVGL 上下文，只修改 watch_ui 私有状态。
 */
static void watch_face_press_event(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_PRESSED) {
        return;
    }

    s_ui.lock_target = !s_ui.lock_target;
    s_ui.tip_visible = false;
    s_ui.redraw_full_next = true;
    s_ui.dynamic_area_valid = false;
    ESP_LOGI(TAG, "极轴锁定目标已切换: %s",
             s_ui.lock_target ? "锁定" : "释放");
}

/**
 * @brief 推进锁定动画并请求主表盘重绘。
 * @param timer 创建于 ui_init() 的 LVGL 定时器。
 * @return 无返回值。
 * @note 运行在 LVGL 上下文；菜单显示时不发起隐藏页面重绘。
 */
static void watch_face_timer_cb(lv_timer_t *timer)
{
    lv_obj_t *face = timer != NULL ? (lv_obj_t *)timer->user_data : NULL;
    if (face == NULL || face != s_ui.face_draw) {
        return;
    }

    const uint16_t previous_progress = s_ui.lock_progress;
    const uint32_t now = lv_tick_get();
    uint32_t elapsed = lv_tick_elaps(s_ui.frame_tick);
    s_ui.frame_tick = now;
    if (elapsed > WATCH_FACE_LOCK_DURATION_MS) {
        elapsed = WATCH_FACE_LOCK_DURATION_MS;
    }

    uint32_t delta = elapsed * WATCH_FACE_LOCK_PROGRESS_MAX /
                     WATCH_FACE_LOCK_DURATION_MS;
    if (elapsed > 0U && delta == 0U) {
        delta = 1U;
    }

    if (s_ui.lock_target) {
        const uint32_t next = (uint32_t)s_ui.lock_progress + delta;
        s_ui.lock_progress = next > WATCH_FACE_LOCK_PROGRESS_MAX ?
            WATCH_FACE_LOCK_PROGRESS_MAX : (uint16_t)next;
    } else if (delta >= s_ui.lock_progress) {
        s_ui.lock_progress = 0U;
    } else {
        s_ui.lock_progress = (uint16_t)(s_ui.lock_progress - delta);
    }

    if (s_ui.display != NULL &&
        lv_disp_get_scr_act(s_ui.display) == s_ui.main_page) {
        const bool settled_unlocked =
            !s_ui.lock_target && s_ui.lock_progress == 0U;
        const bool settled_locked =
            s_ui.lock_target &&
            s_ui.lock_progress == WATCH_FACE_LOCK_PROGRESS_MAX;
        const bool animation_finished =
            previous_progress != s_ui.lock_progress &&
            (s_ui.lock_progress == 0U ||
             s_ui.lock_progress == WATCH_FACE_LOCK_PROGRESS_MAX);

        if (animation_finished) {
            s_ui.redraw_full_next = true;
            s_ui.dynamic_area_valid = false;
        }

        if (!settled_unlocked || settled_locked || s_ui.redraw_full_next) {
            lv_obj_invalidate(face);
            s_ui.dynamic_area_valid = false;
            s_ui.redraw_full_next = false;
        } else {
            lv_area_t face_area;
            lv_obj_get_coords(face, &face_area);
            watch_face_draw_state_t state;
            build_draw_state(&state, &face_area);

            lv_area_t current_areas[3];
            build_unlocked_dynamic_areas(&state, current_areas);
            for (uint8_t index = 0; index < 3U; ++index) {
                lv_obj_invalidate_area(face, &current_areas[index]);
                if (s_ui.dynamic_area_valid) {
                    lv_obj_invalidate_area(face, &s_ui.dynamic_areas[index]);
                }
                s_ui.dynamic_areas[index] = current_areas[index];
            }
            s_ui.dynamic_area_valid = true;
        }
    }
}

/**
 * @brief 创建统一风格的菜单文本标签。
 * @param parent 标签父对象。
 * @param text 初始文本。
 * @param font 标签字体。
 * @param color 标签颜色。
 * @return 创建完成的 LVGL label。
 * @note 只能在 ui_init() 的 LVGL 锁内调用。
 */
static lv_obj_t *create_label(
    lv_obj_t *parent,
    const char *text,
    const lv_font_t *font,
    lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(label, color, LV_STATE_DEFAULT);
    lv_obj_set_style_text_letter_space(label, 0, LV_STATE_DEFAULT);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    return label;
}

/**
 * @brief 创建设置菜单卡片左侧的彩色圆形图标。
 * @param card 设置项卡片。
 * @param symbol LVGL 内置图标。
 * @param color 图标圆颜色。
 * @return 无返回值。
 * @note 只在 ui_init() 的 LVGL 锁内调用，不访问硬件。
 */
static void create_menu_icon(
    lv_obj_t *card,
    const char *symbol,
    lv_color_t color)
{
    lv_obj_t *icon = lv_obj_create(card);
    lv_obj_remove_style_all(icon);
    lv_obj_set_size(icon, WATCH_MENU_ICON_SIZE, WATCH_MENU_ICON_SIZE);
    lv_obj_set_pos(icon, 16, 10);
    lv_obj_set_style_radius(icon, LV_RADIUS_CIRCLE, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(icon, color, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(icon);
    lv_label_set_text(label, symbol);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_18, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(label, WATCH_COLOR_HOUR, LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_STATE_DEFAULT);
    lv_obj_center(label);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
}

/**
 * @brief 创建一个已验证布局的设置菜单卡片。
 * @param list 可滚动列表。
 * @param spec 菜单项规格。
 * @return 无返回值。
 * @note 在 ui_init() 的 LVGL 锁内创建，按下状态由 LVGL 管理。
 */
static void create_menu_item(
    lv_obj_t *list,
    const watch_menu_item_spec_t *spec)
{
    lv_obj_t *card = lv_obj_create(list);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, WATCH_MENU_CARD_WIDTH, WATCH_MENU_CARD_HEIGHT);
    lv_obj_set_style_radius(card, WATCH_MENU_CARD_HEIGHT / 2, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x22252A), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x333840), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(card, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(card, 0, LV_STATE_DEFAULT);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    create_menu_icon(card, spec->symbol, lv_color_hex(spec->color));

    lv_obj_t *label = lv_label_create(card);
    lv_label_set_text(label, spec->label);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_18, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(label, WATCH_COLOR_HOUR, LV_STATE_DEFAULT);
    lv_obj_set_style_text_letter_space(label, 0, LV_STATE_DEFAULT);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 76, 0);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
}

/**
 * @brief 创建保持现有行为的六项设置菜单 screen。
 * @param parent 页面父对象；NULL 表示独立 screen。
 * @return 创建完成的菜单 screen。
 * @note 所有 LVGL API 必须在 watch_lvgl_start() 的初始化锁内调用。
 */
static lv_obj_t *create_menu_page(lv_obj_t *parent)
{
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, WATCH_FACE_SIZE, WATCH_FACE_SIZE);
    lv_obj_set_pos(page, 0, 0);
    lv_obj_set_style_bg_color(page, WATCH_COLOR_MENU_BG, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *list = lv_obj_create(page);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, WATCH_FACE_SIZE, WATCH_FACE_SIZE);
    lv_obj_set_pos(list, 0, 0);
    lv_obj_set_style_bg_color(list, WATCH_COLOR_MENU_BG, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(list, WATCH_MENU_PADDING_Y, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(list, WATCH_MENU_PADDING_Y, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(list, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(list, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_row(list, WATCH_MENU_CARD_GAP, LV_STATE_DEFAULT);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

    for (size_t index = 0;
         index < sizeof(s_menu_items) / sizeof(s_menu_items[0]);
         ++index) {
        create_menu_item(list, &s_menu_items[index]);
    }

    lv_obj_t *status = create_label(page, "2", &lv_font_montserrat_24,
                                    WATCH_COLOR_HOUR);
    lv_obj_set_pos(status, 45, 24);
    s_ui.menu_list = list;
    return page;
}

/**
 * @brief 缓存完整数据快照作为极光表盘的时间基准。
 * @param display watch_lvgl 已注册的 Display。
 * @param data 在 LVGL 锁外读取完成的数据快照。
 * @return 无返回值；参数或对象无效时忽略请求。
 * @note 必须由 watch_lvgl_run() 在锁内调用，不访问硬件。
 */
void watch_ui_update(
    lv_display_t *display,
    const watch_ui_data_t *data)
{
    if (display == NULL || data == NULL ||
        display != s_ui.display || s_ui.face_draw == NULL) {
        return;
    }

    if (data->time_valid &&
        data->month >= 1U && data->month <= 12U &&
        data->day >= 1U && data->day <= 31U &&
        data->weekday < 7U &&
        data->hour < 24U &&
        data->minute < 60U &&
        data->second < 60U) {
        s_ui.time_snapshot = *data;
        s_ui.snapshot_tick = lv_tick_get();
        s_ui.time_valid = true;
    }

    if (lv_disp_get_scr_act(display) == s_ui.main_page) {
        s_ui.redraw_full_next = false;
        s_ui.dynamic_area_valid = false;
        lv_obj_invalidate(s_ui.face_draw);
    }
}

/**
 * @brief 创建极光主表盘和保持现状的菜单 screen。
 * @param display 已由 watch_lvgl 注册的 LVGL Display。
 * @return 无返回值；Display 或活动 screen 无效时停止创建。
 * @note 只能由 watch_lvgl_start() 在统一 LVGL 锁内调用。
 */
void ui_init(lv_display_t *display)
{
    if (display == NULL) {
        return;
    }

    if (s_ui.animation_timer != NULL) {
        lv_timer_del(s_ui.animation_timer);
        s_ui.animation_timer = NULL;
    }

    memset(&s_ui, 0, sizeof(s_ui));
    s_ui.display = display;
    s_ui.tip_visible = true;
    s_ui.frame_tick = lv_tick_get();
    s_ui.redraw_full_next = true;
    prepare_tick_geometry();

    lv_obj_t *screen = lv_disp_get_scr_act(display);
    if (screen == NULL) {
        memset(&s_ui, 0, sizeof(s_ui));
        return;
    }

    lv_obj_clean(screen);
    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_color(screen, WATCH_COLOR_BG, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(screen, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(screen, 0, LV_STATE_DEFAULT);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *face = lv_obj_create(screen);
    lv_obj_remove_style_all(face);
    lv_obj_set_size(face, WATCH_FACE_SIZE, WATCH_FACE_SIZE);
    lv_obj_set_pos(face, 0, 0);
    lv_obj_clear_flag(face, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(face, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(face, draw_watch_face_event, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(face, watch_face_press_event, LV_EVENT_PRESSED, NULL);

    s_ui.main_page = screen;
    s_ui.face_draw = face;
    s_ui.menu_page = create_menu_page(NULL);
    s_ui.menu_visible = false;
    s_ui.animation_timer = lv_timer_create(
        watch_face_timer_cb,
        WATCH_FACE_TIMER_PERIOD_MS,
        face
    );
    if (s_ui.animation_timer == NULL) {
        ESP_LOGW(TAG, "极光表盘动画定时器创建失败，表盘将保持静态");
    }
}

/**
 * @brief 在极光主表盘和设置菜单之间切换当前 screen。
 * @param display watch_lvgl 注册的 LVGL Display。
 * @return 无返回值；Display 或页面句柄无效时忽略请求。
 * @note 必须由 watch_lvgl_run() 在锁内调用，不能从 GPIO ISR 调用。
 */
void watch_ui_toggle_menu(lv_display_t *display)
{
    if (display == NULL || display != s_ui.display ||
        s_ui.main_page == NULL || s_ui.menu_page == NULL) {
        return;
    }

    s_ui.menu_visible = !s_ui.menu_visible;
    if (s_ui.menu_visible) {
        lv_scr_load(s_ui.menu_page);
        if (s_ui.menu_list != NULL) {
            lv_obj_scroll_to_y(s_ui.menu_list, 0, LV_ANIM_OFF);
        }
    } else {
        lv_scr_load(s_ui.main_page);
        if (s_ui.face_draw != NULL) {
            s_ui.frame_tick = lv_tick_get();
            lv_obj_invalidate(s_ui.face_draw);
        }
    }

    ESP_LOGI(TAG, "G1 菜单页面已切换: %s",
             s_ui.menu_visible ? "显示菜单" : "显示极光主表盘");
}
