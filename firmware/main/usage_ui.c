#include "usage_ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "aiud_protocol.h"
#include "app_settings.h"
#include "display_bsp.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "service_logos.h"
#include "usb_hid.h"

#define COLOR_BG       0x030809
#define COLOR_CARD     0x101C20
#define COLOR_CARD_ALT 0x0B1417
#define COLOR_INK      0xE8F5F0
#define COLOR_MUTED    0x829A9D
#define COLOR_CODEX    0x35D39A
#define COLOR_CLAUDE   0xFF7043
#define COLOR_TRACK    0x263238
#define COLOR_WARNING  0xF4C95D
#define PAGE_COUNT     4
#define PAGE_ANIM_MS   240

typedef struct {
    lv_obj_t *card;
    lv_obj_t *name;
    lv_obj_t *bar;
    lv_obj_t *percent;
    lv_obj_t *session_reset;
    lv_obj_t *weekly;
    uint32_t accent;
    const char *provider_name;
} provider_widgets_t;

typedef struct {
    lv_obj_t *page;
    lv_obj_t *status;
    lv_obj_t *weekly_total;
    lv_obj_t *empty;
    lv_obj_t *bars[AIUD_DAILY_DAYS];
    lv_obj_t *values[AIUD_DAILY_DAYS];
    uint32_t accent;
    bool is_codex;
} weekly_page_widgets_t;

static QueueHandle_t s_queue;
static aiud_snapshot_t s_snapshot;
static bool s_has_snapshot;
static int64_t s_received_monotonic_us;
static lv_obj_t *s_home;
static lv_obj_t *s_detail;
static lv_obj_t *s_header_time;
static lv_obj_t *s_connection_dot;
static lv_obj_t *s_waiting;
static lv_obj_t *s_waiting_title;
static lv_obj_t *s_waiting_hint;
static lv_obj_t *s_detail_title;
static lv_obj_t *s_detail_status;
static lv_obj_t *s_detail_session_bar;
static lv_obj_t *s_detail_session_value;
static lv_obj_t *s_detail_session_reset;
static lv_obj_t *s_detail_weekly_bar;
static lv_obj_t *s_detail_weekly_value;
static provider_widgets_t s_codex;
static provider_widgets_t s_claude;
static weekly_page_widgets_t s_codex_weekly;
static weekly_page_widgets_t s_claude_weekly;
static lv_obj_t *s_settings_page;
static lv_obj_t *s_brightness_page;
static lv_obj_t *s_auto_dim_page;
static lv_obj_t *s_return_home_page;
static lv_obj_t *s_brightness_slider;
static lv_obj_t *s_brightness_value;
static lv_obj_t *s_settings_brightness_value;
static lv_obj_t *s_settings_auto_dim_value;
static lv_obj_t *s_settings_return_home_value;
static lv_obj_t *s_auto_dim_marks[4];
static lv_obj_t *s_auto_dim_switch;
static lv_obj_t *s_return_home_switch;
static lv_obj_t *s_page_indicator;
static lv_obj_t *s_page_dots[PAGE_COUNT];
static bool s_detail_is_codex;
static uint8_t s_current_page;
static bool s_navigation_gesture;
static bool s_display_dimmed;
static int64_t s_last_activity_us;

static lv_obj_t *make_label(lv_obj_t *parent, const char *text,
                            const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

static lv_obj_t *make_box(lv_obj_t *parent, uint32_t color, int radius)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_style_bg_color(box, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(box, radius, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    return box;
}

static lv_obj_t *make_screen(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    return screen;
}

static lv_obj_t *make_bar(lv_obj_t *parent, uint32_t accent)
{
    lv_obj_t *bar = lv_bar_create(parent);
    lv_bar_set_range(bar, 0, 1000);
    lv_obj_set_height(bar, 12);
    lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COLOR_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(accent), LV_PART_INDICATOR);
    lv_obj_set_style_anim_time(bar, 500, 0);
    return bar;
}

static lv_obj_t *make_service_logo(lv_obj_t *parent, bool is_codex)
{
    lv_obj_t *image = lv_img_create(parent);
    lv_img_set_src(image, is_codex ? &aiud_logo_codex : &aiud_logo_claude);
    lv_obj_set_style_img_recolor(image,
        lv_color_hex(is_codex ? COLOR_INK : COLOR_CLAUDE), 0);
    lv_obj_set_style_img_recolor_opa(image, LV_OPA_COVER, 0);
    return image;
}

static const char *status_text(aiud_provider_status_t status)
{
    switch (status) {
    case AIUD_STATUS_AVAILABLE: return "Live";
    case AIUD_STATUS_TEMPORARILY_UNAVAILABLE: return "Stale";
    case AIUD_STATUS_AUTHENTICATION_REQUIRED: return "Sign in";
    case AIUD_STATUS_NOT_CONFIGURED: return "Not set";
    default: return "Waiting";
    }
}

static uint32_t status_color(aiud_provider_status_t status, uint32_t accent)
{
    if (status == AIUD_STATUS_AVAILABLE) return accent;
    if (status == AIUD_STATUS_TEMPORARILY_UNAVAILABLE) return COLOR_WARNING;
    if (status == AIUD_STATUS_AUTHENTICATION_REQUIRED) return COLOR_CLAUDE;
    return COLOR_MUTED;
}

static int64_t estimated_now(void)
{
    if (!s_has_snapshot) return 0;
    return s_snapshot.generated_at +
           (esp_timer_get_time() - s_received_monotonic_us) / 1000000;
}

static void update_header(void)
{
    if (!s_has_snapshot) {
        lv_label_set_text(s_header_time, "-- --, ----  --:--");
        lv_obj_align(s_header_time, LV_ALIGN_TOP_RIGHT, -30, 10);
        lv_obj_set_style_bg_color(s_connection_dot,
                                  lv_color_hex(COLOR_MUTED), 0);
        return;
    }

    time_t local_seconds = (time_t)(estimated_now() +
        (int64_t)s_snapshot.utc_offset_minutes * 60);
    struct tm local_time;
    char text[32];
    if (gmtime_r(&local_seconds, &local_time) == NULL ||
        strftime(text, sizeof(text), "%b %d, %Y  %H:%M", &local_time) == 0) {
        snprintf(text, sizeof(text), "-- --, ----  --:--");
    }
    lv_label_set_text(s_header_time, text);
    lv_obj_align(s_header_time, LV_ALIGN_TOP_RIGHT, -30, 10);

    const bool stale = esp_timer_get_time() - s_received_monotonic_us >
                       15LL * 60 * 1000000;
    lv_obj_set_style_bg_color(s_connection_dot,
        lv_color_hex(stale ? COLOR_WARNING : COLOR_CODEX), 0);
}

static void capture_active_screen(void)
{
    const lv_img_cf_t format = LV_IMG_CF_TRUE_COLOR;
    lv_obj_t *active = lv_scr_act();
    const uint32_t size = lv_snapshot_buf_size_needed(active, format);
    void *pixels = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pixels == NULL) return;

    lv_img_dsc_t image;
    if (lv_snapshot_take_to_buf(active, format, &image, pixels, size) ==
        LV_RES_OK) {
        usage_usb_send_capture(image.data, image.data_size,
                               image.header.w, image.header.h);
    }
    heap_caps_free(pixels);
}

static void format_reset(char *output, size_t size, uint32_t reset_at)
{
    if (reset_at == 0 || !s_has_snapshot) {
        snprintf(output, size, "reset --");
        return;
    }
    int64_t seconds = (int64_t)reset_at - estimated_now();
    if (seconds <= 0) {
        snprintf(output, size, "reset now");
    } else if (seconds >= 86400) {
        snprintf(output, size, "reset %lldd %lldh",
                 (long long)(seconds / 86400),
                 (long long)((seconds % 86400) / 3600));
    } else {
        snprintf(output, size, "reset %lldh %02lldm",
                 (long long)(seconds / 3600),
                 (long long)((seconds % 3600) / 60));
    }
}

static void format_reset_compact(char *output, size_t size, uint32_t reset_at)
{
    if (reset_at == 0 || !s_has_snapshot) {
        snprintf(output, size, "--");
        return;
    }
    int64_t seconds = (int64_t)reset_at - estimated_now();
    if (seconds <= 0) {
        snprintf(output, size, "now");
    } else if (seconds >= 86400) {
        snprintf(output, size, "%lldd %lldh",
                 (long long)(seconds / 86400),
                 (long long)((seconds % 86400) / 3600));
    } else {
        snprintf(output, size, "%lldh %02lldm",
                 (long long)(seconds / 3600),
                 (long long)((seconds % 3600) / 60));
    }
}

static void format_percent(char *output, size_t size,
                           const aiud_usage_window_t *window)
{
    if (!window->valid) snprintf(output, size, "--");
    else snprintf(output, size, "%u%%",
                  (1000U - (window->used_permille > 1000U ? 1000U :
                             window->used_permille) + 5) / 10);
}

static uint16_t remaining_permille(const aiud_usage_window_t *window)
{
    if (!window->valid) return 0;
    const uint16_t used = window->used_permille > 1000U ? 1000U :
                          window->used_permille;
    return (uint16_t)(1000U - used);
}

static void format_used_percent(char *output, size_t size,
                                const aiud_usage_window_t *window)
{
    if (!window->valid) snprintf(output, size, "--");
    else snprintf(output, size, "%u%%",
                  ((window->used_permille > 1000U ? 1000U :
                     window->used_permille) + 5) / 10);
}

static provider_widgets_t create_provider_card(lv_obj_t *parent,
                                                const char *name,
                                                uint32_t accent,
                                                bool is_codex);
static void show_detail(bool codex);
static void show_page(uint8_t page);
static void update_settings_summaries(void);
static void setting_back_clicked(lv_event_t *event);

static void interaction_pressed(lv_event_t *event)
{
    (void)event;
    s_navigation_gesture = false;
}

static void card_clicked(lv_event_t *event)
{
    if (s_navigation_gesture) return;
    show_detail((bool)(uintptr_t)lv_event_get_user_data(event));
}

static provider_widgets_t create_provider_card(lv_obj_t *parent,
                                                const char *name,
                                                uint32_t accent,
                                                bool is_codex)
{
    provider_widgets_t widgets = {
        .accent = accent,
        .provider_name = name,
    };
    widgets.card = make_box(parent, COLOR_CARD, 16);
    lv_obj_set_size(widgets.card, 210, 218);
    lv_obj_set_pos(widgets.card, is_codex ? 12 : 234, 50);
    lv_obj_add_flag(widgets.card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(widgets.card, interaction_pressed, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(widgets.card, card_clicked, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)is_codex);

    lv_obj_t *logo = make_service_logo(widgets.card, is_codex);
    lv_obj_set_pos(logo, 16, 23);
    widgets.name = make_label(widgets.card, name, &lv_font_montserrat_32,
                              COLOR_INK);
    lv_obj_set_pos(widgets.name, 54, 18);

    widgets.bar = make_bar(widgets.card, accent);
    lv_obj_set_size(widgets.bar, 110, 20);
    lv_obj_set_pos(widgets.bar, 16, 82);
    widgets.percent = make_label(widgets.card, "--", &lv_font_montserrat_24,
                                 COLOR_INK);
    lv_obj_align(widgets.percent, LV_ALIGN_TOP_RIGHT, -13, 78);

    lv_obj_t *session_label = make_label(widgets.card, "Session",
                                         &lv_font_montserrat_20, COLOR_INK);
    lv_obj_set_pos(session_label, 16, 132);
    widgets.session_reset = make_label(widgets.card, "--",
                                       &lv_font_montserrat_20, COLOR_INK);
    lv_obj_align(widgets.session_reset, LV_ALIGN_TOP_RIGHT, -14, 132);

    lv_obj_t *weekly_label = make_label(widgets.card, "Weekly",
                                        &lv_font_montserrat_20, COLOR_INK);
    lv_obj_set_pos(weekly_label, 16, 168);
    widgets.weekly = make_label(widgets.card, "--", &lv_font_montserrat_20,
                                COLOR_INK);
    lv_obj_align(widgets.weekly, LV_ALIGN_TOP_RIGHT, -14, 168);
    return widgets;
}

static void update_card(provider_widgets_t *widgets,
                        const aiud_provider_t *provider)
{
    char text[32];
    lv_bar_set_value(widgets->bar,
                     remaining_permille(&provider->session),
                     LV_ANIM_ON);
    format_percent(text, sizeof(text), &provider->session);
    lv_label_set_text(widgets->percent, text);
    lv_obj_align(widgets->percent, LV_ALIGN_TOP_RIGHT, -13, 78);
    format_reset_compact(text, sizeof(text), provider->session.resets_at);
    lv_label_set_text(widgets->session_reset, text);
    lv_obj_align(widgets->session_reset, LV_ALIGN_TOP_RIGHT, -14, 132);
    format_percent(text, sizeof(text), &provider->weekly);
    lv_label_set_text(widgets->weekly, text);
    lv_obj_align(widgets->weekly, LV_ALIGN_TOP_RIGHT, -14, 168);
}

static const aiud_provider_t *selected_provider(void)
{
    return s_detail_is_codex ? &s_snapshot.codex : &s_snapshot.claude;
}

static uint32_t selected_accent(void)
{
    return s_detail_is_codex ? COLOR_CODEX : COLOR_CLAUDE;
}

static void update_detail(void)
{
    if (!s_has_snapshot) return;
    const aiud_provider_t *provider = selected_provider();
    const uint32_t accent = selected_accent();
    char text[32];
    lv_label_set_text(s_detail_title, s_detail_is_codex ? "Codex" : "Claude");
    lv_label_set_text(s_detail_status, status_text(provider->status));
    lv_obj_set_style_text_color(s_detail_status,
        lv_color_hex(status_color(provider->status, accent)), 0);

    lv_obj_set_style_bg_color(s_detail_session_bar, lv_color_hex(accent),
                              LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_detail_weekly_bar, lv_color_hex(accent),
                              LV_PART_INDICATOR);
    lv_bar_set_value(s_detail_session_bar,
        remaining_permille(&provider->session),
        LV_ANIM_ON);
    lv_bar_set_value(s_detail_weekly_bar,
        remaining_permille(&provider->weekly),
        LV_ANIM_ON);
    format_percent(text, sizeof(text), &provider->session);
    lv_label_set_text(s_detail_session_value, text);
    format_reset(text, sizeof(text), provider->session.resets_at);
    lv_label_set_text(s_detail_session_reset, text);
    format_percent(text, sizeof(text), &provider->weekly);
    lv_label_set_text(s_detail_weekly_value, text);
}

static void show_detail(bool codex)
{
    s_detail_is_codex = codex;
    update_detail();
    lv_obj_add_flag(s_page_indicator, LV_OBJ_FLAG_HIDDEN);
    lv_scr_load_anim(s_detail, LV_SCR_LOAD_ANIM_MOVE_LEFT,
                     PAGE_ANIM_MS, 0, false);
}

static void back_clicked(lv_event_t *event)
{
    (void)event;
    s_current_page = 0;
    lv_obj_clear_flag(s_page_indicator, LV_OBJ_FLAG_HIDDEN);
    lv_scr_load_anim(s_home, LV_SCR_LOAD_ANIM_MOVE_RIGHT,
                     PAGE_ANIM_MS, 0, false);
}

static void update_page_indicator(uint8_t active)
{
    const lv_coord_t gap = 10;
    const lv_coord_t total_width = 14 + (PAGE_COUNT - 1) * 5 +
                                   (PAGE_COUNT - 1) * gap;
    lv_coord_t x = (lv_obj_get_width(s_page_indicator) - total_width) / 2;
    for (uint8_t i = 0; i < PAGE_COUNT; ++i) {
        const lv_coord_t width = i == active ? 14 : 5;
        lv_obj_set_size(s_page_dots[i], width, 5);
        lv_obj_set_pos(s_page_dots[i], x, 5);
        lv_obj_set_style_bg_color(s_page_dots[i],
            lv_color_hex(i == active ? COLOR_INK : COLOR_TRACK), 0);
        x += width + gap;
    }
}

static void create_page_indicator(void)
{
    s_page_indicator = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_page_indicator);
    lv_obj_set_size(s_page_indicator, 72, 15);
    lv_obj_align(s_page_indicator, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_clear_flag(s_page_indicator, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_page_indicator, LV_OBJ_FLAG_SCROLLABLE);
    for (uint8_t i = 0; i < PAGE_COUNT; ++i) {
        s_page_dots[i] = make_box(s_page_indicator, COLOR_TRACK,
                                  LV_RADIUS_CIRCLE);
        lv_obj_clear_flag(s_page_dots[i], LV_OBJ_FLAG_CLICKABLE);
    }
    update_page_indicator(0);
}

static weekly_page_widgets_t create_weekly_page(lv_obj_t *root,
                                                 const char *name,
                                                 uint32_t accent,
                                                 bool is_codex,
                                                 uint8_t page_index)
{
    static const char *const day_names[AIUD_DAILY_DAYS] = {
        "M", "T", "W", "T", "F", "S", "S"
    };
    weekly_page_widgets_t widgets = {
        .accent = accent,
        .is_codex = is_codex,
    };
    (void)root;
    (void)page_index;
    widgets.page = make_screen();

    lv_obj_t *eyebrow = make_label(widgets.page, "WEEKLY USAGE",
                                    &lv_font_montserrat_14, COLOR_MUTED);
    lv_obj_set_pos(eyebrow, 14, 8);
    lv_obj_t *title = make_label(widgets.page, name,
                                 &lv_font_montserrat_32, COLOR_INK);
    lv_obj_set_pos(title, 14, 22);
    widgets.status = make_label(widgets.page, "Waiting",
                                &lv_font_montserrat_14, COLOR_MUTED);
    lv_obj_align(widgets.status, LV_ALIGN_TOP_RIGHT, -16, 10);
    widgets.weekly_total = make_label(widgets.page, "--",
                                      &lv_font_montserrat_24, accent);
    lv_obj_align(widgets.weekly_total, LV_ALIGN_TOP_RIGHT, -16, 29);

    lv_obj_t *chart = make_box(widgets.page, COLOR_CARD, 16);
    lv_obj_set_pos(chart, 12, 64);
    lv_obj_set_size(chart, 432, 195);
    lv_obj_t *caption = make_label(chart, "DAILY TOKENS · PEAK = 100",
                                   &lv_font_montserrat_14, COLOR_MUTED);
    lv_obj_set_pos(caption, 16, 9);

    for (uint8_t i = 0; i < AIUD_DAILY_DAYS; ++i) {
        const lv_coord_t x = 18 + i * 57;
        lv_obj_t *track = make_box(chart, COLOR_CARD_ALT, 7);
        lv_obj_set_pos(track, x, 47);
        lv_obj_set_size(track, 28, 103);
        widgets.bars[i] = make_box(track, accent, 7);
        lv_obj_set_size(widgets.bars[i], 28, 2);
        lv_obj_align(widgets.bars[i], LV_ALIGN_BOTTOM_MID, 0, 0);
        widgets.values[i] = make_label(chart, "--",
                                       &lv_font_montserrat_14, COLOR_MUTED);
        lv_obj_set_width(widgets.values[i], 40);
        lv_obj_set_style_text_align(widgets.values[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(widgets.values[i], x - 6, 28);
        lv_obj_t *day = make_label(chart, day_names[i],
                                   &lv_font_montserrat_16, COLOR_MUTED);
        lv_obj_set_width(day, 28);
        lv_obj_set_style_text_align(day, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(day, x, 159);
    }

    widgets.empty = make_label(chart, "Daily history starts with the host",
                                &lv_font_montserrat_16, COLOR_MUTED);
    lv_obj_align(widgets.empty, LV_ALIGN_CENTER, 0, 4);
    return widgets;
}

static void update_weekly_page(weekly_page_widgets_t *widgets)
{
    if (!s_has_snapshot) return;
    const aiud_provider_t *provider = widgets->is_codex
        ? &s_snapshot.codex : &s_snapshot.claude;
    const aiud_daily_usage_t *daily = widgets->is_codex
        ? &s_snapshot.codex_daily : &s_snapshot.claude_daily;
    char text[12];

    lv_label_set_text(widgets->status, status_text(provider->status));
    lv_obj_set_style_text_color(widgets->status,
        lv_color_hex(status_color(provider->status, widgets->accent)), 0);
    format_used_percent(text, sizeof(text), &provider->weekly);
    lv_label_set_text(widgets->weekly_total, text);

    uint8_t scale_max = 20;
    for (uint8_t i = 0; i < AIUD_DAILY_DAYS; ++i) {
        if (daily->percent[i] != AIUD_UNKNOWN_DAILY &&
            daily->percent[i] > scale_max) {
            scale_max = daily->percent[i];
        }
    }
    scale_max = (uint8_t)(((scale_max + 9) / 10) * 10);
    if (scale_max > 100) scale_max = 100;

    if (daily->valid) lv_obj_add_flag(widgets->empty, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_clear_flag(widgets->empty, LV_OBJ_FLAG_HIDDEN);
    for (uint8_t i = 0; i < AIUD_DAILY_DAYS; ++i) {
        const uint8_t value = daily->percent[i];
        if (value == AIUD_UNKNOWN_DAILY) {
            lv_obj_set_height(widgets->bars[i], 2);
            lv_obj_set_style_bg_color(widgets->bars[i],
                                      lv_color_hex(COLOR_TRACK), 0);
            lv_label_set_text(widgets->values[i], "--");
            lv_obj_set_style_text_color(widgets->values[i],
                                        lv_color_hex(COLOR_MUTED), 0);
        } else {
            const lv_coord_t height = value == 0 ? 2 :
                (lv_coord_t)((value * 100U) / scale_max);
            lv_obj_set_height(widgets->bars[i], height < 4 && value ? 4 : height);
            lv_obj_set_style_bg_color(widgets->bars[i],
                                      lv_color_hex(widgets->accent), 0);
            snprintf(text, sizeof(text), "%u", value);
            lv_label_set_text(widgets->values[i], text);
            lv_obj_set_style_text_color(widgets->values[i],
                lv_color_hex(i == s_snapshot.current_weekday
                    ? widgets->accent : COLOR_MUTED), 0);
        }
    }
}

static uint16_t selected_dim_seconds(uint16_t selected)
{
    static const uint16_t values[] = {300, 1800, 3600, 10800};
    return selected < sizeof(values) / sizeof(values[0]) ? values[selected] : 300;
}

static uint16_t dim_seconds_selection(uint16_t seconds)
{
    if (seconds == 1800) return 1;
    if (seconds == 3600) return 2;
    if (seconds == 10800) return 3;
    return 0;
}

static void brightness_changed(lv_event_t *event)
{
    (void)event;
    const uint8_t value = (uint8_t)lv_slider_get_value(s_brightness_slider);
    char text[8];
    snprintf(text, sizeof(text), "%u%%", value);
    lv_label_set_text(s_brightness_value, text);
    lv_obj_align(s_brightness_value, LV_ALIGN_TOP_RIGHT, -16, 8);
    board_display_set_brightness(value);
    s_display_dimmed = false;
    s_last_activity_us = esp_timer_get_time();
}

static void brightness_released(lv_event_t *event)
{
    (void)event;
    app_settings_set_brightness(
        (uint8_t)lv_slider_get_value(s_brightness_slider));
    update_settings_summaries();
}

static void return_home_changed(lv_event_t *event)
{
    (void)event;
    app_settings_set_return_home(
        lv_obj_has_state(s_return_home_switch, LV_STATE_CHECKED));
    s_last_activity_us = esp_timer_get_time();
    update_settings_summaries();
}

static const char *dim_seconds_text(uint16_t seconds)
{
    if (seconds == 1800) return "30 minutes";
    if (seconds == 3600) return "1 hour";
    if (seconds == 10800) return "3 hours";
    return "5 minutes";
}

static void update_settings_summaries(void)
{
    const app_settings_t *settings = app_settings_get();
    char text[8];
    snprintf(text, sizeof(text), "%u%%", settings->brightness_percent);
    lv_label_set_text(s_settings_brightness_value, text);
    lv_label_set_text(s_settings_auto_dim_value,
                      settings->auto_dim_enabled
                          ? dim_seconds_text(settings->auto_dim_seconds)
                          : "Off");
    lv_label_set_text(s_settings_return_home_value,
                      settings->return_home_on_idle ? "On" : "Off");

    const uint16_t selected = dim_seconds_selection(settings->auto_dim_seconds);
    for (uint16_t i = 0; i < 4; ++i) {
        lv_label_set_text(s_auto_dim_marks[i],
                          i == selected ? LV_SYMBOL_OK : "");
    }
}

static void setting_back_clicked(lv_event_t *event)
{
    (void)event;
    update_settings_summaries();
    lv_obj_clear_flag(s_page_indicator, LV_OBJ_FLAG_HIDDEN);
    update_page_indicator(3);
    lv_scr_load_anim(s_settings_page, LV_SCR_LOAD_ANIM_MOVE_RIGHT,
                     PAGE_ANIM_MS, 0, false);
}

static void settings_row_clicked(lv_event_t *event)
{
    if (s_navigation_gesture) return;
    lv_obj_t *page = lv_event_get_user_data(event);
    lv_obj_add_flag(s_page_indicator, LV_OBJ_FLAG_HIDDEN);
    lv_scr_load_anim(page, LV_SCR_LOAD_ANIM_MOVE_LEFT,
                     PAGE_ANIM_MS, 0, false);
}

static void auto_dim_option_clicked(lv_event_t *event)
{
    const uint16_t selected = (uint16_t)(uintptr_t)lv_event_get_user_data(event);
    app_settings_set_auto_dim(selected_dim_seconds(selected));
    board_display_set_brightness(app_settings_get()->brightness_percent);
    s_display_dimmed = false;
    s_last_activity_us = esp_timer_get_time();
    update_settings_summaries();
}

static void auto_dim_enabled_changed(lv_event_t *event)
{
    (void)event;
    const bool enabled = lv_obj_has_state(s_auto_dim_switch, LV_STATE_CHECKED);
    app_settings_set_auto_dim_enabled(enabled);
    if (!enabled && s_display_dimmed) {
        board_display_set_brightness(app_settings_get()->brightness_percent);
        s_display_dimmed = false;
    }
    s_last_activity_us = esp_timer_get_time();
    update_settings_summaries();
}

static void update_idle_display(void)
{
    const int64_t now = esp_timer_get_time();
    const int64_t last_touch = board_display_last_touch_us();
    if (last_touch > s_last_activity_us) {
        s_last_activity_us = last_touch;
        if (s_display_dimmed) {
            board_display_set_brightness(app_settings_get()->brightness_percent);
            s_display_dimmed = false;
        }
        return;
    }

    const app_settings_t *settings = app_settings_get();
    if (s_display_dimmed || !settings->auto_dim_enabled) return;
    if (now - s_last_activity_us <
        (int64_t)settings->auto_dim_seconds * 1000000) return;

    if (settings->return_home_on_idle) show_page(0);
    board_display_set_brightness(5);
    s_display_dimmed = true;
}

static void add_settings_header(lv_obj_t *page, const char *title)
{
    lv_obj_t *back = lv_btn_create(page);
    lv_obj_set_pos(back, 12, 10);
    lv_obj_set_size(back, 40, 34);
    lv_obj_set_style_bg_color(back, lv_color_hex(COLOR_CARD), 0);
    lv_obj_set_style_radius(back, 12, 0);
    lv_obj_set_style_shadow_width(back, 0, 0);
    lv_obj_add_event_cb(back, setting_back_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *arrow = make_label(back, LV_SYMBOL_LEFT,
                                 &lv_font_montserrat_16, COLOR_INK);
    lv_obj_center(arrow);
    lv_obj_t *heading = make_label(page, title, &lv_font_montserrat_24,
                                   COLOR_INK);
    lv_obj_set_pos(heading, 66, 8);
}

static lv_obj_t *add_settings_row(lv_obj_t *page, lv_coord_t y,
                                  const char *title, lv_obj_t **value,
                                  lv_obj_t *destination, bool alternate)
{
    lv_obj_t *row = make_box(page, alternate ? COLOR_CARD_ALT : COLOR_CARD, 16);
    lv_obj_set_pos(row, 12, y);
    lv_obj_set_size(row, 432, 54);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, interaction_pressed, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(row, settings_row_clicked, LV_EVENT_CLICKED, destination);
    lv_obj_t *label = make_label(row, title, &lv_font_montserrat_20, COLOR_INK);
    lv_obj_set_pos(label, 16, 15);
    *value = make_label(row, "--", &lv_font_montserrat_16, COLOR_MUTED);
    lv_obj_align(*value, LV_ALIGN_RIGHT_MID, -38, 0);
    lv_obj_t *chevron = make_label(row, LV_SYMBOL_RIGHT,
                                   &lv_font_montserrat_16, COLOR_MUTED);
    lv_obj_align(chevron, LV_ALIGN_RIGHT_MID, -15, 0);
    return row;
}

static void create_settings_pages(void)
{
    const app_settings_t *settings = app_settings_get();

    s_brightness_page = make_screen();
    add_settings_header(s_brightness_page, "Brightness");
    lv_obj_t *brightness = make_box(s_brightness_page, COLOR_CARD, 18);
    lv_obj_set_pos(brightness, 12, 68);
    lv_obj_set_size(brightness, 432, 142);
    lv_obj_t *brightness_label = make_label(brightness, "Display brightness",
                                            &lv_font_montserrat_20, COLOR_MUTED);
    lv_obj_set_pos(brightness_label, 16, 14);
    s_brightness_value = make_label(brightness, "65%",
                                    &lv_font_montserrat_32, COLOR_CODEX);
    lv_obj_align(s_brightness_value, LV_ALIGN_TOP_RIGHT, -16, 5);
    s_brightness_slider = lv_slider_create(brightness);
    lv_slider_set_range(s_brightness_slider, 10, 100);
    lv_slider_set_value(s_brightness_slider, settings->brightness_percent,
                        LV_ANIM_OFF);
    lv_obj_set_pos(s_brightness_slider, 16, 87);
    lv_obj_set_size(s_brightness_slider, 400, 16);
    lv_obj_set_style_bg_color(s_brightness_slider,
                              lv_color_hex(COLOR_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_brightness_slider,
                              lv_color_hex(COLOR_CODEX), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_brightness_slider,
                              lv_color_hex(COLOR_INK), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_brightness_slider, 6, LV_PART_KNOB);
    lv_obj_add_event_cb(s_brightness_slider, brightness_changed,
                        LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_brightness_slider, brightness_released,
                        LV_EVENT_RELEASED, NULL);
    lv_obj_clear_flag(s_brightness_slider, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_t *brightness_hint = make_label(s_brightness_page,
        "Swipe navigation is disabled on this screen",
        &lv_font_montserrat_14, COLOR_MUTED);
    lv_obj_align(brightness_hint, LV_ALIGN_BOTTOM_MID, 0, -28);

    s_auto_dim_page = make_screen();
    add_settings_header(s_auto_dim_page, "Auto dim");
    lv_obj_t *dim_content = lv_obj_create(s_auto_dim_page);
    lv_obj_remove_style_all(dim_content);
    lv_obj_set_pos(dim_content, 0, 54);
    lv_obj_set_size(dim_content, 456, 226);
    lv_obj_add_flag(dim_content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(dim_content, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(dim_content, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(dim_content, lv_color_hex(COLOR_CODEX),
                              LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(dim_content, LV_OPA_60, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(dim_content, 4, LV_PART_SCROLLBAR);

    lv_obj_t *dim_toggle = make_box(dim_content, COLOR_CARD, 14);
    lv_obj_set_pos(dim_toggle, 12, 4);
    lv_obj_set_size(dim_toggle, 432, 50);
    lv_obj_t *dim_toggle_label = make_label(dim_toggle, "Auto dim display",
                                            &lv_font_montserrat_20, COLOR_INK);
    lv_obj_set_pos(dim_toggle_label, 16, 13);
    s_auto_dim_switch = lv_switch_create(dim_toggle);
    lv_obj_set_pos(s_auto_dim_switch, 354, 8);
    lv_obj_set_size(s_auto_dim_switch, 60, 34);
    lv_obj_set_style_bg_color(s_auto_dim_switch,
                              lv_color_hex(COLOR_CODEX),
                              LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (settings->auto_dim_enabled) {
        lv_obj_add_state(s_auto_dim_switch, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(s_auto_dim_switch, auto_dim_enabled_changed,
                        LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_clear_flag(s_auto_dim_switch, LV_OBJ_FLAG_GESTURE_BUBBLE);

    static const char *const dim_names[4] = {
        "After 5 minutes", "After 30 minutes", "After 1 hour", "After 3 hours"
    };
    for (uint16_t i = 0; i < 4; ++i) {
        lv_obj_t *row = make_box(dim_content,
                                 i % 2 ? COLOR_CARD_ALT : COLOR_CARD, 14);
        lv_obj_set_pos(row, 12, 62 + i * 49);
        lv_obj_set_size(row, 432, 43);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, auto_dim_option_clicked, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);
        lv_obj_t *label = make_label(row, dim_names[i],
                                     &lv_font_montserrat_20, COLOR_INK);
        lv_obj_set_pos(label, 16, 10);
        s_auto_dim_marks[i] = make_label(row, "", &lv_font_montserrat_20,
                                         COLOR_CODEX);
        lv_obj_align(s_auto_dim_marks[i], LV_ALIGN_RIGHT_MID, -16, 0);
    }

    s_return_home_page = make_screen();
    add_settings_header(s_return_home_page, "Home when idle");
    lv_obj_t *idle = make_box(s_return_home_page, COLOR_CARD, 18);
    lv_obj_set_pos(idle, 12, 68);
    lv_obj_set_size(idle, 432, 104);
    lv_obj_t *idle_title = make_label(idle, "Return to Home",
                                      &lv_font_montserrat_20, COLOR_INK);
    lv_obj_set_pos(idle_title, 16, 16);
    lv_obj_t *idle_hint = make_label(idle,
        "Go home before the display dims",
        &lv_font_montserrat_14, COLOR_MUTED);
    lv_obj_set_pos(idle_hint, 16, 52);
    s_return_home_switch = lv_switch_create(idle);
    lv_obj_set_pos(s_return_home_switch, 350, 25);
    lv_obj_set_size(s_return_home_switch, 64, 36);
    lv_obj_set_style_bg_color(s_return_home_switch,
                              lv_color_hex(COLOR_CODEX),
                              LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (settings->return_home_on_idle) {
        lv_obj_add_state(s_return_home_switch, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(s_return_home_switch, return_home_changed,
                        LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_clear_flag(s_return_home_switch, LV_OBJ_FLAG_GESTURE_BUBBLE);

    s_settings_page = make_screen();
    lv_obj_t *eyebrow = make_label(s_settings_page, "DEVICE",
                                    &lv_font_montserrat_14, COLOR_MUTED);
    lv_obj_set_pos(eyebrow, 14, 8);
    lv_obj_t *title = make_label(s_settings_page, "Settings",
                                 &lv_font_montserrat_32, COLOR_INK);
    lv_obj_set_pos(title, 14, 22);

    add_settings_row(s_settings_page, 65, "Brightness",
                     &s_settings_brightness_value, s_brightness_page, false);
    add_settings_row(s_settings_page, 125, "Auto dim",
                     &s_settings_auto_dim_value, s_auto_dim_page, true);
    add_settings_row(s_settings_page, 185, "Home when idle",
                     &s_settings_return_home_value, s_return_home_page, false);
    brightness_changed(NULL);
    update_settings_summaries();
}

static lv_obj_t *page_screen(uint8_t page)
{
    if (page == 0) return s_home;
    if (page == 1) return s_codex_weekly.page;
    if (page == 2) return s_claude_weekly.page;
    return s_settings_page;
}

static void show_page(uint8_t page)
{
    if (page >= PAGE_COUNT) page = PAGE_COUNT - 1;
    lv_obj_t *selected = page_screen(page);
    if (lv_scr_act() == selected) {
        s_current_page = page;
        lv_obj_clear_flag(s_page_indicator, LV_OBJ_FLAG_HIDDEN);
        update_page_indicator(page);
        return;
    }
    const lv_scr_load_anim_t animation = page > s_current_page
        ? LV_SCR_LOAD_ANIM_MOVE_LEFT : LV_SCR_LOAD_ANIM_MOVE_RIGHT;
    s_current_page = page;
    lv_obj_clear_flag(s_page_indicator, LV_OBJ_FLAG_HIDDEN);
    update_page_indicator(page);
    lv_scr_load_anim(selected, animation, PAGE_ANIM_MS, 0, false);
}

static void page_gesture(lv_event_t *event)
{
    (void)event;
    lv_indev_t *indev = lv_indev_get_act();
    if (indev == NULL) return;
    const lv_dir_t direction = lv_indev_get_gesture_dir(indev);
    if (direction == LV_DIR_LEFT && s_current_page < PAGE_COUNT - 1) {
        s_navigation_gesture = true;
        lv_indev_wait_release(indev);
        show_page(s_current_page + 1);
    } else if (direction == LV_DIR_RIGHT && s_current_page > 0) {
        s_navigation_gesture = true;
        lv_indev_wait_release(indev);
        show_page(s_current_page - 1);
    }
}

static void detail_exit_gesture(lv_event_t *event)
{
    lv_indev_t *indev = lv_indev_get_act();
    if (indev == NULL || lv_indev_get_gesture_dir(indev) != LV_DIR_RIGHT) {
        return;
    }
    s_navigation_gesture = true;
    lv_indev_wait_release(indev);
    if (lv_event_get_current_target(event) == s_detail) {
        back_clicked(NULL);
    } else {
        setting_back_clicked(NULL);
    }
}

static void update_layout(void)
{
    const bool codex_visible = s_snapshot.codex.status != AIUD_STATUS_NOT_CONFIGURED;
    const bool claude_visible = s_snapshot.claude.status != AIUD_STATUS_NOT_CONFIGURED;
    if (codex_visible) lv_obj_clear_flag(s_codex.card, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_codex.card, LV_OBJ_FLAG_HIDDEN);
    if (claude_visible) lv_obj_clear_flag(s_claude.card, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_claude.card, LV_OBJ_FLAG_HIDDEN);

    const bool any = codex_visible || claude_visible;
    if (any) lv_obj_add_flag(s_waiting, LV_OBJ_FLAG_HIDDEN);
    else {
        lv_label_set_text(s_waiting_title, "No providers configured");
        lv_label_set_text(s_waiting_hint,
            "Sign in to Codex or Claude on this computer");
        lv_obj_clear_flag(s_waiting, LV_OBJ_FLAG_HIDDEN);
    }
    if (codex_visible != claude_visible) {
        provider_widgets_t *only = codex_visible ? &s_codex : &s_claude;
        lv_obj_set_pos(only->card, 12, 50);
        lv_obj_set_width(only->card, 432);
        lv_obj_set_width(only->bar, 320);
        lv_obj_align(only->percent, LV_ALIGN_TOP_RIGHT, -13, 78);
        lv_obj_align(only->session_reset, LV_ALIGN_TOP_RIGHT, -14, 132);
        lv_obj_align(only->weekly, LV_ALIGN_TOP_RIGHT, -14, 168);
    } else {
        lv_obj_set_pos(s_codex.card, 12, 50);
        lv_obj_set_pos(s_claude.card, 234, 50);
        lv_obj_set_width(s_codex.card, 210);
        lv_obj_set_width(s_claude.card, 210);
        lv_obj_set_width(s_codex.bar, 110);
        lv_obj_set_width(s_claude.bar, 110);
        lv_obj_align(s_codex.percent, LV_ALIGN_TOP_RIGHT, -13, 78);
        lv_obj_align(s_codex.session_reset, LV_ALIGN_TOP_RIGHT, -14, 132);
        lv_obj_align(s_codex.weekly, LV_ALIGN_TOP_RIGHT, -14, 168);
        lv_obj_align(s_claude.percent, LV_ALIGN_TOP_RIGHT, -13, 78);
        lv_obj_align(s_claude.session_reset, LV_ALIGN_TOP_RIGHT, -14, 132);
        lv_obj_align(s_claude.weekly, LV_ALIGN_TOP_RIGHT, -14, 168);
    }
}

static void update_snapshot(const aiud_snapshot_t *snapshot)
{
    s_snapshot = *snapshot;
    s_has_snapshot = true;
    s_received_monotonic_us = esp_timer_get_time();
    update_header();
    update_layout();
    update_card(&s_codex, &s_snapshot.codex);
    update_card(&s_claude, &s_snapshot.claude);
    update_detail();
    update_weekly_page(&s_codex_weekly);
    update_weekly_page(&s_claude_weekly);
}

static void ui_timer(lv_timer_t *timer)
{
    (void)timer;
    aiud_snapshot_t incoming;
    if (xQueueReceive(s_queue, &incoming, 0) == pdTRUE) {
        update_snapshot(&incoming);
    }
    update_idle_display();
    update_header();
    if (usage_usb_capture_requested()) capture_active_screen();
}

static void create_detail_page(lv_obj_t *root)
{
    (void)root;
    s_detail = make_screen();

    lv_obj_t *back = lv_btn_create(s_detail);
    lv_obj_set_pos(back, 12, 10);
    lv_obj_set_size(back, 40, 32);
    lv_obj_set_style_bg_color(back, lv_color_hex(COLOR_CARD), 0);
    lv_obj_set_style_radius(back, 12, 0);
    lv_obj_set_style_shadow_width(back, 0, 0);
    lv_obj_add_event_cb(back, back_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *arrow = make_label(back, LV_SYMBOL_LEFT, &lv_font_montserrat_16,
                                 COLOR_INK);
    lv_obj_center(arrow);

    s_detail_title = make_label(s_detail, "Codex", &lv_font_montserrat_32,
                                COLOR_INK);
    lv_obj_set_pos(s_detail_title, 66, 6);
    s_detail_status = make_label(s_detail, "LIVE", &lv_font_montserrat_16,
                                 COLOR_CODEX);
    lv_obj_align(s_detail_status, LV_ALIGN_TOP_RIGHT, -16, 17);

    lv_obj_t *session = make_box(s_detail, COLOR_CARD, 16);
    lv_obj_set_pos(session, 12, 55);
    lv_obj_set_size(session, 432, 94);
    lv_obj_t *session_label = make_label(session, "Session",
                                         &lv_font_montserrat_16, COLOR_MUTED);
    lv_obj_set_pos(session_label, 16, 10);
    s_detail_session_value = make_label(session, "--",
                                        &lv_font_montserrat_32, COLOR_INK);
    lv_obj_align(s_detail_session_value, LV_ALIGN_TOP_RIGHT, -16, 2);
    s_detail_session_bar = make_bar(session, COLOR_CODEX);
    lv_obj_set_pos(s_detail_session_bar, 16, 47);
    lv_obj_set_width(s_detail_session_bar, 400);
    s_detail_session_reset = make_label(session, "reset --",
                                        &lv_font_montserrat_16, COLOR_MUTED);
    lv_obj_set_pos(s_detail_session_reset, 16, 65);

    lv_obj_t *weekly = make_box(s_detail, COLOR_CARD_ALT, 16);
    lv_obj_set_pos(weekly, 12, 160);
    lv_obj_set_size(weekly, 432, 94);
    lv_obj_t *weekly_label = make_label(weekly, "Weekly",
                                        &lv_font_montserrat_16, COLOR_MUTED);
    lv_obj_set_pos(weekly_label, 16, 10);
    s_detail_weekly_value = make_label(weekly, "--",
                                       &lv_font_montserrat_32, COLOR_INK);
    lv_obj_align(s_detail_weekly_value, LV_ALIGN_TOP_RIGHT, -16, 2);
    s_detail_weekly_bar = make_bar(weekly, COLOR_CODEX);
    lv_obj_set_pos(s_detail_weekly_bar, 16, 49);
    lv_obj_set_width(s_detail_weekly_bar, 400);
}

void usage_ui_init(QueueHandle_t snapshot_queue)
{
    s_queue = snapshot_queue;
    lv_obj_t *root = lv_scr_act();
    lv_obj_remove_style_all(root);
    lv_obj_set_style_bg_color(root, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    s_home = root;
    lv_obj_t *brand = make_label(s_home, "AI USAGE",
                                 &lv_font_montserrat_20, COLOR_INK);
    lv_obj_set_pos(brand, 14, 10);
    s_header_time = make_label(s_home, "-- --, ----  --:--",
                               &lv_font_montserrat_20, COLOR_MUTED);
    lv_obj_align(s_header_time, LV_ALIGN_TOP_RIGHT, -30, 10);
    s_connection_dot = make_box(s_home, COLOR_MUTED, LV_RADIUS_CIRCLE);
    lv_obj_set_size(s_connection_dot, 9, 9);
    lv_obj_align(s_connection_dot, LV_ALIGN_TOP_RIGHT, -14, 20);

    s_codex = create_provider_card(s_home, "Codex", COLOR_CODEX, true);
    s_claude = create_provider_card(s_home, "Claude", COLOR_CLAUDE, false);

    s_waiting = make_box(s_home, COLOR_CARD, 18);
    lv_obj_set_pos(s_waiting, 45, 82);
    lv_obj_set_size(s_waiting, 366, 132);
    s_waiting_title = make_label(s_waiting, "Connect the desktop host",
                                 &lv_font_montserrat_24, COLOR_INK);
    lv_obj_align(s_waiting_title, LV_ALIGN_TOP_MID, 0, 20);
    s_waiting_hint = make_label(s_waiting,
        "Usage data only - no account tokens are stored",
        &lv_font_montserrat_16, COLOR_MUTED);
    lv_obj_align(s_waiting_hint, LV_ALIGN_TOP_MID, 0, 67);
    lv_obj_add_flag(s_codex.card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_claude.card, LV_OBJ_FLAG_HIDDEN);
    s_codex_weekly = create_weekly_page(root, "Codex", COLOR_CODEX, true, 1);
    s_claude_weekly = create_weekly_page(root, "Claude", COLOR_CLAUDE, false, 2);
    create_settings_pages();
    create_detail_page(root);
    lv_obj_add_event_cb(s_home, page_gesture, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(s_codex_weekly.page, page_gesture,
                        LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(s_claude_weekly.page, page_gesture,
                        LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(s_settings_page, page_gesture,
                        LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(s_detail, detail_exit_gesture,
                        LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(s_brightness_page, detail_exit_gesture,
                        LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(s_auto_dim_page, detail_exit_gesture,
                        LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(s_return_home_page, detail_exit_gesture,
                        LV_EVENT_GESTURE, NULL);
    create_page_indicator();
    s_last_activity_us = esp_timer_get_time();
    lv_timer_create(ui_timer, 250, NULL);
}
