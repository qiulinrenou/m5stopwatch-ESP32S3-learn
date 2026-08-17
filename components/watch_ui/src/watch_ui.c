#include "watch_ui.h"

#include <stdio.h>                                   // 提供 snprintf()
#include <string.h>                                  // 提供 memset()

#include "esp_log.h"                                // 提供菜单切换诊断日志
#include "lvgl.h"                                   // 提供 LVGL 控件、样式和绘图接口

#define WATCH_FACE_DIAL_SIZE        466              // 外圈使用完整 466 像素圆屏直径

#define WATCH_FACE_DATE_Y            67              // 顶部日期的纵向位置
#define WATCH_FACE_TIME_Y           105              // 主时间固定绘制区域的纵向位置
#define WATCH_FACE_STATUS_Y         184              // 状态行的纵向位置
#define WATCH_FACE_METRIC_Y         252              // 左右圆环上移后共同保持 y=330 底边
#define WATCH_FACE_RUN_METRIC_Y     240              // 中间圆环上移后共同保持 y=330 底边
#define WATCH_FACE_METRIC_X         124              // 左右数据圆环距中心的横向距离

#define WATCH_FACE_TIME_WIDTH       300              // 固定主时间区域宽度，保证五个字符完整居中
#define WATCH_FACE_TIME_HEIGHT       64              // 为 48 号字体保留稳定的实际绘制高度

#define WATCH_MENU_CARD_WIDTH        320             // HTML 设置卡片宽度
#define WATCH_MENU_CARD_HEIGHT       64              // HTML 设置卡片高度
#define WATCH_MENU_ICON_SIZE         44              // HTML 彩色图标圆尺寸
#define WATCH_MENU_CARD_GAP          12              // HTML 卡片垂直间距
#define WATCH_MENU_PADDING_Y         55              // HTML 列表上下留白

#define WATCH_COLOR_GREEN lv_color_hex(0x00E85C)     // 主表盘绿色
#define WATCH_COLOR_RED   lv_color_hex(0xFF1745)     // 心率圆环红色
#define WATCH_COLOR_BLUE  lv_color_hex(0x18B8FF)     // 天气圆环蓝色
#define WATCH_COLOR_WHITE lv_color_hex(0xF4F6F5)     // 主要文字白色
#define WATCH_COLOR_TICK  lv_color_hex(0x59615E)     // 普通刻度灰色

typedef struct {
    lv_display_t *display;                           // 保存当前 UI 所属的 Display
    lv_obj_t *main_page;                             // 保存已验证的主表盘 LVGL screen
    lv_obj_t *menu_page;                             // 设置菜单页面容器
    lv_obj_t *menu_list;                             // 可滚动的设置项列表
    lv_obj_t *date_label;                            // 保存需要更新的日期标签
    lv_obj_t *time_label;                            // 保存需要更新的主时间标签
    lv_obj_t *battery_label;                         // 保存需要更新的真实电量标签
    bool menu_visible;                                // 当前是否显示设置菜单
} watch_ui_runtime_t;

static watch_ui_runtime_t s_ui = {0};                // watch_ui 私有且唯一的运行状态
static const char *TAG = "WATCH_UI";                 // 菜单切换诊断日志标签

typedef struct {
    const char *label;                                // 菜单项显示文字
    const char *symbol;                               // LVGL 内置图标
    uint32_t color;                                   // 图标圆背景 RGB888 颜色
} watch_menu_item_spec_t;

static const watch_menu_item_spec_t s_menu_items[] = {
    {"Display",       LV_SYMBOL_EYE_OPEN,  0xFFAA00},
    {"Vibration",     LV_SYMBOL_VOLUME_MID, 0x00D285},
    {"DND Mode",      LV_SYMBOL_MUTE,      0x7C5CFF},
    {"Power Save",    LV_SYMBOL_CHARGE,    0x2ED573},
    {"Notifications",  LV_SYMBOL_BELL,      0xFF4757},
    {"System",        LV_SYMBOL_SETTINGS,  0x1E90FF},
};

/*
 * lv_line_set_points() 不会复制坐标数组，因此这些数组必须在界面存活期间持续有效。
 * 使用 static const 可避免栈地址在创建函数返回后失效。
 */
static const lv_point_t s_heart_points[] = {
    {13, 20}, {4, 12}, {1, 7}, {3, 3}, {7, 2},
    {13, 7}, {19, 2}, {23, 3}, {25, 7}, {22, 12}, {13, 20},
};

/* 跑步人形向右前方倾斜，四肢采用不同折线，避免显示成一条抽象符号。 */
static const lv_point_t s_runner_torso[] = {
    {12, 1}, {7, 10}, {14, 19},
};

static const lv_point_t s_runner_front_arm[] = {
    {9, 7}, {18, 11}, {24, 5},
};

static const lv_point_t s_runner_back_arm[] = {
    {8, 8}, {1, 14},
};

static const lv_point_t s_runner_front_leg[] = {
    {14, 19}, {24, 25}, {32, 23},
};

static const lv_point_t s_runner_back_leg[] = {
    {14, 19}, {7, 30}, {0, 31},
};

static const lv_point_t s_cloud_points[] = {
    {1, 16}, {3, 11}, {8, 9}, {10, 4}, {15, 1}, {21, 3},
    {23, 8}, {29, 9}, {32, 13}, {30, 17}, {3, 17}, {1, 16},
};

static const char *const s_weekday_names[] = {
    "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT",
};

/**
 * @brief 创建一个统一风格的文本标签。
 */
static lv_obj_t *create_label(
    lv_obj_t *parent,
    const char *text,
    const lv_font_t *font,
    lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);        // 由 LVGL 管理标签对象的生命周期

    lv_label_set_text(label, text);                   // 写入初始显示内容
    lv_obj_set_style_text_font(label, font, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(label, color, LV_STATE_DEFAULT);
    lv_obj_set_style_text_letter_space(label, 0, LV_STATE_DEFAULT);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);  // 主表盘文本不参与触摸命中测试

    return label;
}

/**
 * @brief 创建设置菜单卡片左侧的彩色圆形图标。
 *
 * @param card 设置项卡片父对象。
 * @param symbol LVGL 内置 FontAwesome 风格符号。
 * @param color 图标圆背景颜色。
 * @return 无返回值；对象生命周期由 LVGL 卡片父对象管理。
 * @note 只在 ui_init() 的 LVGL 锁内调用，不访问硬件。
 */
static void create_menu_icon(
    lv_obj_t *card,
    const char *symbol,
    lv_color_t color)
{
    lv_obj_t *icon = lv_obj_create(card);             // 创建 HTML icon-box 对应的圆形容器

    lv_obj_remove_style_all(icon);                    // 移除 LVGL 默认边框和内边距
    lv_obj_set_size(icon, WATCH_MENU_ICON_SIZE, WATCH_MENU_ICON_SIZE);
    lv_obj_set_pos(icon, 16, 10);                     // 对应卡片左右 16、上下居中布局
    lv_obj_set_style_radius(icon, LV_RADIUS_CIRCLE, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(icon, color, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon_label = lv_label_create(icon);     // 使用 LVGL 符号字体承载图标
    lv_label_set_text(icon_label, symbol);
    lv_obj_set_style_text_font(icon_label, &lv_font_montserrat_18, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(icon_label, WATCH_COLOR_WHITE, LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(icon_label, LV_TEXT_ALIGN_CENTER, LV_STATE_DEFAULT);
    lv_obj_center(icon_label);                        // 图标符号在彩色圆内居中
    lv_obj_clear_flag(icon_label, LV_OBJ_FLAG_CLICKABLE);
}

/**
 * @brief 创建一个 HTML 风格的设置菜单卡片。
 *
 * @param list 可滚动设置项列表父对象。
 * @param spec 菜单项文字、图标和颜色规格。
 * @return 无返回值；对象由列表父对象统一管理。
 * @note 卡片和子对象均在 ui_init() 的 LVGL 锁内创建，触摸按下状态由 LVGL 管理。
 */
static void create_menu_item(
    lv_obj_t *list,
    const watch_menu_item_spec_t *spec)
{
    lv_obj_t *card = lv_obj_create(list);             // 创建对应 HTML menu-item 的 LVGL 对象

    lv_obj_remove_style_all(card);                    // 保证背景、边框和内边距与 HTML 一致
    lv_obj_set_size(card, WATCH_MENU_CARD_WIDTH, WATCH_MENU_CARD_HEIGHT);
    lv_obj_set_style_radius(card, WATCH_MENU_CARD_HEIGHT / 2, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x22252A), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x333840), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(card, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(card, 0, LV_STATE_DEFAULT);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);      // 让触摸按下状态表现为 HTML :active

    create_menu_icon(card, spec->symbol, lv_color_hex(spec->color));

    lv_obj_t *label = lv_label_create(card);          // 创建 HTML item-label 对应的文本对象
    lv_label_set_text(label, spec->label);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_18, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(label, WATCH_COLOR_WHITE, LV_STATE_DEFAULT);
    lv_obj_set_style_text_letter_space(label, 0, LV_STATE_DEFAULT);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 76, 0);     // 图标右侧保留 16 像素文字间距
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);  // 点击事件由卡片接收
}

/**
 * @brief 创建 HTML 设置菜单页面及其可滚动列表。
 *
 * @param parent 页面父对象；传入 NULL 时创建独立的 LVGL screen。
 * @return 创建完成的菜单页面 screen。
 * @note 所有 LVGL API 必须在 watch_lvgl_start() 的初始化锁内调用。
 */
static lv_obj_t *create_menu_page(lv_obj_t *parent)
{
    lv_obj_t *menu_page = lv_obj_create(parent);      // 创建与主表盘并列的页面容器

    lv_obj_remove_style_all(menu_page);
    lv_obj_set_size(menu_page, WATCH_FACE_DIAL_SIZE, WATCH_FACE_DIAL_SIZE);
    lv_obj_set_pos(menu_page, 0, 0);
    lv_obj_set_style_bg_color(menu_page, lv_color_black(), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(menu_page, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_clear_flag(menu_page, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *list = lv_obj_create(menu_page);        // 创建 HTML list-container 对应的滚动容器

    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, WATCH_FACE_DIAL_SIZE, WATCH_FACE_DIAL_SIZE);
    lv_obj_set_pos(list, 0, 0);
    lv_obj_set_style_bg_color(list, lv_color_black(), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(list, WATCH_MENU_PADDING_Y, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(list, WATCH_MENU_PADDING_Y, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(list, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(list, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_row(list, WATCH_MENU_CARD_GAP, LV_STATE_DEFAULT);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);   // 让卡片按 HTML 的单列方向自动排列
    lv_obj_set_flex_align(
        list,
        LV_FLEX_ALIGN_START,
        LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER
    );
    lv_obj_set_scroll_dir(list, LV_DIR_VER);           // 6 项总高度超过 466，保留触摸上下滚动
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

    for (size_t i = 0; i < sizeof(s_menu_items) / sizeof(s_menu_items[0]); ++i) {
        create_menu_item(list, &s_menu_items[i]);     // 按 HTML 顺序创建 Display 至 System
    }

    lv_obj_t *status = create_label(                  // 对应 HTML 左上角 status-number
        menu_page,
        "2",
        &lv_font_montserrat_24,
        WATCH_COLOR_WHITE
    );
    lv_obj_set_pos(status, 45, 24);                   // 固定在滚动列表上层，不随列表移动

    s_ui.menu_list = list;                             // 保存列表句柄，进入菜单时回到顶部
    return menu_page;
}

/**
 * @brief 创建底部数据区域使用的彩色圆环。
 */
static lv_obj_t *create_metric_ring(
    lv_obj_t *parent,
    lv_coord_t size,
    lv_coord_t x_offset,
    lv_coord_t y_offset,
    lv_color_t color)
{
    lv_obj_t *ring = lv_obj_create(parent);           // 创建数据圆环的父对象

    lv_obj_set_size(ring, size, size);
    lv_obj_align(ring, LV_ALIGN_TOP_MID, x_offset, y_offset);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ring, lv_color_black(), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ring, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(ring, color, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ring, 3, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(ring, 0, LV_STATE_DEFAULT);
    lv_obj_clear_flag(                                // 数据圆环当前只显示信息，不响应触摸
        ring,
        LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE
    );

    return ring;
}

/**
 * @brief 使用持久化坐标数组创建一条图标线段。
 */
static lv_obj_t *create_line(
    lv_obj_t *parent,
    const lv_point_t *points,
    uint16_t point_count,
    lv_coord_t x,
    lv_coord_t y,
    lv_color_t color,
    lv_coord_t width)
{
    lv_obj_t *line = lv_line_create(parent);          // 创建 LVGL 折线对象

    lv_line_set_points(line, points, point_count);    // points 必须在对象存活期间保持有效
    lv_obj_set_pos(line, x, y);
    lv_obj_set_style_line_color(line, color, LV_STATE_DEFAULT);
    lv_obj_set_style_line_width(line, width, LV_STATE_DEFAULT);
    lv_obj_set_style_line_rounded(line, true, LV_STATE_DEFAULT);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);

    return line;
}

/**
 * @brief 创建外圈分钟刻度和顶部绿色标记。
 */
static void create_dial(lv_obj_t *screen)
{
    lv_obj_t *meter = lv_meter_create(screen);        // meter 统一绘制 60 根圆周刻度

    lv_obj_set_size(meter, WATCH_FACE_DIAL_SIZE, WATCH_FACE_DIAL_SIZE);
    lv_obj_center(meter);
    lv_obj_set_style_bg_color(meter, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(meter, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(meter, WATCH_COLOR_TICK, LV_PART_MAIN);
    lv_obj_set_style_border_width(meter, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(meter, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_pad_all(meter, 0, LV_PART_MAIN); // 刻度半径直接使用完整外圈内容区
    lv_obj_set_style_text_font(meter, &lv_font_montserrat_14, LV_PART_TICKS);
    lv_obj_set_style_text_color(meter, WATCH_COLOR_TICK, LV_PART_TICKS);
    lv_obj_clear_flag(
        meter,
        LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE
    );

    lv_meter_scale_t *scale = lv_meter_add_scale(meter);  // 添加唯一的分钟刻度层

    lv_meter_set_scale_range(                         // 270° 使零点对准屏幕十二点方向
        meter,
        scale,
        0,
        59,
        354,
        270
    );
    lv_meter_set_scale_ticks(meter, scale, 60, 2, 12, WATCH_COLOR_TICK);
    lv_meter_set_scale_major_ticks(
        meter,
        scale,
        5,                                             // 每五分钟绘制一根绿色主刻度
        3,
        19,
        WATCH_COLOR_GREEN,
        8
    );

    lv_obj_t *top_marker = lv_obj_create(screen);      // 强调十二点方向

    lv_obj_set_size(top_marker, 6, 14);
    lv_obj_align(top_marker, LV_ALIGN_TOP_MID, 0, 0); // 与满屏外圈的零点刻度共用中心线
    lv_obj_set_style_radius(top_marker, 3, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(top_marker, WATCH_COLOR_GREEN, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(top_marker, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(top_marker, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(top_marker, 0, LV_STATE_DEFAULT);
    lv_obj_clear_flag(
        top_marker,
        LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE
    );
}

/**
 * @brief 创建左下角的模拟心率区域。
 */
static void create_heart_metric(lv_obj_t *screen)
{
    lv_obj_t *ring = create_metric_ring(
        screen,
        78,
        -WATCH_FACE_METRIC_X,
        WATCH_FACE_METRIC_Y,
        WATCH_COLOR_RED
    );

    create_line(ring, s_heart_points,
                sizeof(s_heart_points) / sizeof(s_heart_points[0]),
                25, 11, WATCH_COLOR_RED, 3);

    lv_obj_t *value = create_label(                   // 心率仍保持固定模拟值
        ring,
        "128",
        &lv_font_montserrat_14,
        WATCH_COLOR_WHITE
    );
    lv_obj_align(value, LV_ALIGN_BOTTOM_MID, 0, -8);
}

/**
 * @brief 创建底部中央的完整跑步人形图标。
 */
static void create_runner_metric(lv_obj_t *screen)
{
    lv_obj_t *ring = create_metric_ring(
        screen,
        90,
        0,
        WATCH_FACE_RUN_METRIC_Y,
        WATCH_COLOR_GREEN
    );

    lv_obj_t *head = lv_obj_create(ring);             // 使用圆点绘制跑步人形的头部

    lv_obj_set_size(head, 12, 12);
    lv_obj_set_pos(head, 47, 12);
    lv_obj_set_style_radius(head, LV_RADIUS_CIRCLE, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(head, WATCH_COLOR_GREEN, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(head, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(head, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(head, 0, LV_STATE_DEFAULT);
    lv_obj_clear_flag(
        head,
        LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE
    );

    create_line(ring, s_runner_torso,
                sizeof(s_runner_torso) / sizeof(s_runner_torso[0]),
                35, 27, WATCH_COLOR_GREEN, 5);
    create_line(ring, s_runner_front_arm,
                sizeof(s_runner_front_arm) / sizeof(s_runner_front_arm[0]),
                35, 29, WATCH_COLOR_GREEN, 5);
    create_line(ring, s_runner_back_arm,
                sizeof(s_runner_back_arm) / sizeof(s_runner_back_arm[0]),
                34, 29, WATCH_COLOR_GREEN, 5);
    create_line(ring, s_runner_front_leg,
                sizeof(s_runner_front_leg) / sizeof(s_runner_front_leg[0]),
                34, 38, WATCH_COLOR_GREEN, 5);
    create_line(ring, s_runner_back_leg,
                sizeof(s_runner_back_leg) / sizeof(s_runner_back_leg[0]),
                34, 38, WATCH_COLOR_GREEN, 5);
}

/**
 * @brief 创建右下角的模拟天气区域。
 */
static void create_weather_metric(lv_obj_t *screen)
{
    lv_obj_t *ring = create_metric_ring(
        screen,
        78,
        WATCH_FACE_METRIC_X,
        WATCH_FACE_METRIC_Y,
        WATCH_COLOR_BLUE
    );

    create_line(ring, s_cloud_points,
                sizeof(s_cloud_points) / sizeof(s_cloud_points[0]),
                22, 12, WATCH_COLOR_BLUE, 3);

    lv_obj_t *value = create_label(                   // 天气仍保持固定模拟值
        ring,
        "24°",
        &lv_font_montserrat_14,
        WATCH_COLOR_WHITE
    );
    lv_obj_align(value, LV_ALIGN_BOTTOM_MID, 0, -8);
}

/**
 * @brief 创建日期、主时间和真实电量状态行。
 */
static void create_text_content(lv_obj_t *screen)
{
    s_ui.date_label = create_label(
        screen,
        "RTC STATUS",
        &lv_font_montserrat_24,                       // 日期使用原生 24 号字体直接绘制
        WATCH_COLOR_WHITE
    );
    lv_label_set_recolor(s_ui.date_label, true);      // 星期使用绿色，日期使用白色
    lv_obj_align(s_ui.date_label, LV_ALIGN_TOP_MID, 0, WATCH_FACE_DATE_Y);

    s_ui.time_label = create_label(
        screen,
        "WAIT",
        &lv_font_montserrat_48,
        WATCH_COLOR_WHITE
    );
    lv_obj_set_size(                                  // 使用真实对象尺寸承载大号字体，避免缩放图层不出图
        s_ui.time_label,
        WATCH_FACE_TIME_WIDTH,
        WATCH_FACE_TIME_HEIGHT
    );
    lv_label_set_long_mode(                           // 时间始终限制在固定区域内，不参与滚动
        s_ui.time_label,
        LV_LABEL_LONG_CLIP
    );
    lv_obj_set_style_text_align(
        s_ui.time_label,
        LV_TEXT_ALIGN_CENTER,
        LV_STATE_DEFAULT
    );
    lv_obj_align(
        s_ui.time_label,
        LV_ALIGN_TOP_MID,
        0,
        WATCH_FACE_TIME_Y
    );

    lv_obj_t *status_name = create_label(
        screen,
        "StopWatch",
        &lv_font_montserrat_18,
        WATCH_COLOR_GREEN
    );
    lv_obj_align(status_name, LV_ALIGN_TOP_MID, -46, WATCH_FACE_STATUS_Y);

    s_ui.battery_label = create_label(
        screen,
        LV_SYMBOL_BATTERY_EMPTY " --%",
        &lv_font_montserrat_18,
        WATCH_COLOR_WHITE
    );
    lv_obj_align(s_ui.battery_label, LV_ALIGN_TOP_MID, 65, WATCH_FACE_STATUS_Y);
}

/**
 * @brief 根据电量百分比选择最接近的电池图标。
 */
static const char *battery_symbol(uint8_t percent)
{
    if (percent >= 90U) {
        return LV_SYMBOL_BATTERY_FULL;
    }
    if (percent >= 65U) {
        return LV_SYMBOL_BATTERY_3;
    }
    if (percent >= 40U) {
        return LV_SYMBOL_BATTERY_2;
    }
    if (percent >= 15U) {
        return LV_SYMBOL_BATTERY_1;
    }
    return LV_SYMBOL_BATTERY_EMPTY;
}

void watch_ui_update(
    lv_display_t *display,
    const watch_ui_data_t *data)
{
    if (display == NULL || data == NULL ||           // 拒绝无效参数和错误 Display
        display != s_ui.display ||
        s_ui.date_label == NULL ||
        s_ui.time_label == NULL ||
        s_ui.battery_label == NULL) {
        return;
    }

    if (data->time_valid &&                            // 只渲染字段范围完整合法的 RTC 快照
        data->weekday < 7U &&
        data->day >= 1U && data->day <= 31U &&
        data->hour < 24U &&
        data->minute < 60U &&
        data->second < 60U) {
        char date_text[24];                           // 颜色控制串、星期和日期所需空间
        char time_text[8];                            // 同时容纳格式化器推断的 uint8_t 最坏输入

        (void)snprintf(
            date_text,
            sizeof(date_text),
            "#00E85C %s# %02u",
            s_weekday_names[data->weekday],
            (unsigned int)data->day
        );
        (void)snprintf(
            time_text,
            sizeof(time_text),
            "%02u:%02u",                            // 冒号固定显示，避免主时间每秒闪烁
            (unsigned int)data->hour,
            (unsigned int)data->minute
        );

        lv_label_set_text(s_ui.date_label, date_text);
        lv_label_set_text(s_ui.time_label, time_text);
    } else {
        lv_label_set_text(s_ui.date_label, "TIME INVALID");
        lv_label_set_text(s_ui.time_label, "RTC");   // 使用 48 号主时间区域明确提示 RTC 无效
    }

    if (data->battery_valid && data->battery_percent <= 100U) {
        char battery_text[24];                        // FontAwesome 图标最多占 3 个 UTF-8 字节

        (void)snprintf(
            battery_text,
            sizeof(battery_text),
            "%s %u%%",
            battery_symbol(data->battery_percent),
            (unsigned int)data->battery_percent
        );
        lv_label_set_text(s_ui.battery_label, battery_text);
    } else {
        lv_label_set_text(
            s_ui.battery_label,
            LV_SYMBOL_BATTERY_EMPTY " --%"
        );
    }
}

void ui_init(lv_display_t *display)
{
    if (display == NULL) {                            // 防止使用无效 Display
        return;
    }

    memset(&s_ui, 0, sizeof(s_ui));                   // 清除所有旧的 LVGL 对象地址
    s_ui.display = display;                           // 后续更新只接受当前 Display

    lv_obj_t *screen =
        lv_disp_get_scr_act(display);                 // 获取该 Display 当前的活动屏幕

    if (screen == NULL) {                             // Display 没有活动屏幕时停止创建界面
        memset(&s_ui, 0, sizeof(s_ui));
        return;
    }

    lv_obj_clean(screen);                             // 删除旧 Demo 或旧表盘的全部子对象
    lv_obj_remove_style_all(screen);                  // 移除默认主题在活动屏幕上的样式
    lv_obj_set_style_bg_color(screen, lv_color_black(), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(screen, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(screen, 0, LV_STATE_DEFAULT);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);

    create_dial(screen);                              // 保持原有根屏表盘层级，先创建背景和刻度
    create_text_content(screen);                      // 保持 RTC 时间和真实电量区域直接挂在活动屏幕
    create_heart_metric(screen);                      // 保持左侧模拟心率区域直接挂在活动屏幕
    create_runner_metric(screen);                     // 保持中央完整跑步人形直接挂在活动屏幕
    create_weather_metric(screen);                    // 保持右侧模拟天气区域直接挂在活动屏幕

    s_ui.main_page = screen;                          // 保存已验证的主表盘 screen
    s_ui.menu_page = create_menu_page(NULL);          // 创建独立的 HTML 风格菜单 screen
    s_ui.menu_visible = false;                        // 初始页面状态与隐藏标志保持一致
}

/**
 * @brief 在主表盘和设置菜单之间切换当前页面。
 *
 * @param display 由 watch_lvgl 注册并传入的 LVGL Display。
 * @return 无返回值；Display 或页面句柄无效时忽略请求。
 * @note 必须由 watch_lvgl_run() 在 LVGL 互斥锁内调用，不能从 GPIO ISR 直接调用。
 */
void watch_ui_toggle_menu(lv_display_t *display)
{
    if (display == NULL || display != s_ui.display ||
        s_ui.main_page == NULL || s_ui.menu_page == NULL) {
        return;
    }

    s_ui.menu_visible = !s_ui.menu_visible;           // 切换页面状态机
    if (s_ui.menu_visible) {
        lv_scr_load(s_ui.menu_page);                  // 加载独立菜单 screen，触发完整屏幕刷新
        if (s_ui.menu_list != NULL) {
            lv_obj_scroll_to_y(s_ui.menu_list, 0, LV_ANIM_OFF); // 每次进入从第一项开始
        }
    } else {
        lv_scr_load(s_ui.main_page);                  // 加载主表盘 screen，恢复动态表盘显示
    }

    ESP_LOGI(
        TAG,
        "G1 菜单页面已切换: %s",
        s_ui.menu_visible ? "显示菜单" : "显示主表盘"
    );
}
