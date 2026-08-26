/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: CC0-1.0
 */

#include "pong_ui.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#define VIEW_W                 480
#define VIEW_H                 480
#define GRID_STEP              48
#define GRID_VERTICAL_COUNT    9
#define GRID_HORIZONTAL_COUNT  9
#define TRAIL_COUNT            8
#define PARTICLE_COUNT         8
#define SEAM_SEGMENT_COUNT     6
#define HUD_UPDATE_TICKS       30U
#define PARTICLE_LIFETIME_TICKS 36U
#define PARTICLE_TRAVEL_STEPS   15U
#define PADDLE_DIRTY_MARGIN     4
#define BALL_DIRTY_MARGIN       48

#define COLOR_BG       lv_color_hex(0x070A12)
#define COLOR_GRID     lv_color_hex(0x172033)
#define COLOR_TEXT     lv_color_hex(0xF1F5F9)
#define COLOR_MUTED    lv_color_hex(0x8290A8)
#define COLOR_CYAN     lv_color_hex(0x3DDCFF)
#define COLOR_BLUE     lv_color_hex(0x4878FF)
#define COLOR_MAGENTA  lv_color_hex(0xE65CFF)
#define COLOR_GREEN    lv_color_hex(0x52F2A8)
#define COLOR_AMBER    lv_color_hex(0xFFBC57)
#define COLOR_DANGER   lv_color_hex(0xFF5E78)
#define COLOR_PANEL    lv_color_hex(0x111827)

static const char *TAG = "pong_ui";

typedef struct {
    bool created;
    pong_role_t viewport;
    lv_obj_t *root;
    lv_obj_t *grid_vertical[GRID_VERTICAL_COUNT];
    lv_obj_t *grid_horizontal[GRID_HORIZONTAL_COUNT];
    lv_obj_t *seam_segments[SEAM_SEGMENT_COUNT];
    lv_obj_t *seam_core;
    lv_obj_t *paddles[2];
    lv_obj_t *ball_glow_outer;
    lv_obj_t *ball_glow_inner;
    lv_obj_t *ball;
    lv_obj_t *trail[TRAIL_COUNT];
    lv_obj_t *particles[PARTICLE_COUNT];
    lv_obj_t *score;
    lv_obj_t *side;
    lv_obj_t *link;
    lv_obj_t *dock;
    lv_obj_t *status;
    lv_obj_t *seam_label;
    lv_obj_t *overlay;
    lv_obj_t *overlay_kicker;
    lv_obj_t *overlay_title;
    lv_obj_t *overlay_detail;
    uint32_t particle_event_id;
    uint32_t particle_start_tick;
    int16_t particle_origin_x;
    int16_t particle_origin_y;
    uint8_t seam_opacity;
    pong_dock_state_t seam_style_state;
    pong_role_t seam_label_viewport;
    pong_role_t paddle_style_role;
    bool seam_style_initialized;
    bool paddle_style_initialized;
    bool paddle_position_initialized[2];
    bool paddle_visible[2];
    bool ball_position_initialized;
    bool ball_visible;
    bool particle_bounds_valid;
    int32_t paddle_y[2];
    int32_t ball_x;
    int32_t ball_y;
    lv_area_t particle_bounds;
    bool hud_initialized;
    uint32_t last_hud_tick;
} pong_ui_state_t;

static pong_ui_state_t s_ui;

static const int8_t s_particle_dx[PARTICLE_COUNT] = { 6, 4, 1, -3, -6, -4, 2, 5 };
static const int8_t s_particle_dy[PARTICLE_COUNT] = { -2, -5, -6, -4, 1, 5, 6, 3 };

static lv_obj_t *make_box(lv_obj_t *parent, int32_t width, int32_t height,
                          lv_color_t color, lv_opa_t opacity, int32_t radius)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(obj, width, height);
    lv_obj_set_style_bg_color(obj, color, 0);
    lv_obj_set_style_bg_opa(obj, opacity, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    return obj;
}

static lv_obj_t *make_label(lv_obj_t *parent, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_font(label, LV_FONT_DEFAULT, 0);
    return label;
}

static void set_hidden(lv_obj_t *obj, bool hidden)
{
    const bool currently_hidden = lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
    if (hidden == currently_hidden) {
        return;
    }
    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static void set_pos_if_changed(lv_obj_t *obj, int32_t x, int32_t y)
{
    if (lv_obj_get_x(obj) != x || lv_obj_get_y(obj) != y) {
        lv_obj_set_pos(obj, x, y);
    }
}

static void set_label_text_if_changed(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

static void set_bg_opa_if_changed(lv_obj_t *obj, lv_opa_t opacity)
{
    if (lv_obj_get_style_bg_opa(obj, LV_PART_MAIN) != opacity) {
        lv_obj_set_style_bg_opa(obj, opacity, 0);
    }
}

static int32_t world_to_view_x(float world_x)
{
    const float origin = s_ui.viewport == PONG_ROLE_RIGHT ? PONG_VIEWPORT_WIDTH : 0.0f;
    return (int32_t)(world_x - origin);
}

static int32_t clamp_i32(int32_t value, int32_t minimum, int32_t maximum)
{
    if (value < minimum) {
        return minimum;
    }
    if (value > maximum) {
        return maximum;
    }
    return value;
}

static void invalidate_view_area(int32_t x1, int32_t y1,
                                 int32_t x2, int32_t y2)
{
    lv_area_t area = {
        .x1 = clamp_i32(x1, 0, VIEW_W - 1),
        .y1 = clamp_i32(y1, 0, VIEW_H - 1),
        .x2 = clamp_i32(x2, 0, VIEW_W - 1),
        .y2 = clamp_i32(y2, 0, VIEW_H - 1),
    };
    if (area.x1 <= area.x2 && area.y1 <= area.y2) {
        lv_obj_invalidate_area(s_ui.root, &area);
    }
}

static void include_bounds(lv_area_t *bounds, bool *valid,
                           int32_t x1, int32_t y1,
                           int32_t x2, int32_t y2)
{
    if (!*valid) {
        *bounds = (lv_area_t) {
            .x1 = x1, .y1 = y1, .x2 = x2, .y2 = y2,
        };
        *valid = true;
        return;
    }
    if (x1 < bounds->x1) {
        bounds->x1 = x1;
    }
    if (y1 < bounds->y1) {
        bounds->y1 = y1;
    }
    if (x2 > bounds->x2) {
        bounds->x2 = x2;
    }
    if (y2 > bounds->y2) {
        bounds->y2 = y2;
    }
}

static const char *dock_text(pong_dock_state_t state)
{
    switch (state) {
    case PONG_DOCK_WIRELESS:  return "WIRELESS PORTAL";
    case PONG_DOCK_ATTACHING: return "MAG SNAP...";
    case PONG_DOCK_SEAMLESS:  return "SEAMLESS";
    case PONG_DOCK_REVERSED:  return "REVERSE ALIGN";
    default:                  return "MAG SEARCH";
    }
}

static lv_color_t dock_color(pong_dock_state_t state)
{
    switch (state) {
    case PONG_DOCK_SEAMLESS:  return COLOR_GREEN;
    case PONG_DOCK_ATTACHING: return COLOR_AMBER;
    case PONG_DOCK_REVERSED:  return COLOR_DANGER;
    case PONG_DOCK_WIRELESS:  return COLOR_CYAN;
    default:                  return COLOR_MUTED;
    }
}

static void update_seam(const pong_render_snapshot_t *snapshot)
{
    const bool seamless = snapshot->dock_state == PONG_DOCK_SEAMLESS;
    const bool active = snapshot->dock_state == PONG_DOCK_WIRELESS ||
                        snapshot->dock_state == PONG_DOCK_ATTACHING ||
                        seamless;
    const int32_t seam_x = s_ui.viewport == PONG_ROLE_LEFT ? VIEW_W - 3 : 0;
    const lv_color_t color = dock_color(snapshot->dock_state);
    const uint8_t target_opacity = seamless ? 24U : (active ? 180U : 40U);
    const uint8_t previous_opacity = s_ui.seam_opacity;
    if (s_ui.seam_opacity < target_opacity) {
        const uint16_t next = s_ui.seam_opacity + 12U;
        s_ui.seam_opacity = next > target_opacity ? target_opacity : (uint8_t)next;
    } else if (s_ui.seam_opacity > target_opacity) {
        s_ui.seam_opacity = s_ui.seam_opacity - target_opacity > 12U ?
                            s_ui.seam_opacity - 12U : target_opacity;
    }

    const bool style_changed = !s_ui.seam_style_initialized ||
                               s_ui.seam_style_state != snapshot->dock_state;
    lv_display_t *display = lv_obj_get_display(s_ui.root);
    const bool invalidation_enabled = lv_display_is_invalidation_enabled(display);
    if (invalidation_enabled) {
        lv_display_enable_invalidation(display, false);
    }

    set_pos_if_changed(s_ui.seam_core, seam_x, 0);
    set_bg_opa_if_changed(s_ui.seam_core, s_ui.seam_opacity);
    if (style_changed) {
        s_ui.seam_style_initialized = true;
        s_ui.seam_style_state = snapshot->dock_state;
        lv_obj_set_style_bg_color(s_ui.seam_core, color, 0);
        for (size_t i = 0; i < SEAM_SEGMENT_COUNT; ++i) {
            lv_obj_set_style_bg_color(s_ui.seam_segments[i], color, 0);
        }
    }

    for (size_t i = 0; i < SEAM_SEGMENT_COUNT; ++i) {
        const int32_t animated_y =
            42 + ((int32_t)i * 64 + (int32_t)(snapshot->world.tick / 2U)) % 384;
        set_pos_if_changed(s_ui.seam_segments[i], seam_x - 2, animated_y);
        set_bg_opa_if_changed(s_ui.seam_segments[i], s_ui.seam_opacity);
        set_hidden(s_ui.seam_segments[i], seamless || !active);
    }

    if (invalidation_enabled) {
        lv_display_enable_invalidation(display, true);
        if (style_changed || previous_opacity != s_ui.seam_opacity ||
            (active && !seamless)) {
            invalidate_view_area(seam_x - 5, 0, seam_x + 8, VIEW_H - 1);
        }
    }

    set_label_text_if_changed(s_ui.seam_label, dock_text(snapshot->dock_state));
    if (style_changed) {
        lv_obj_set_style_text_color(s_ui.seam_label, color, 0);
    }
    if (s_ui.seam_label_viewport != s_ui.viewport) {
        s_ui.seam_label_viewport = s_ui.viewport;
        if (s_ui.viewport == PONG_ROLE_LEFT) {
            lv_obj_align(s_ui.seam_label, LV_ALIGN_RIGHT_MID, -12, 0);
        } else {
            lv_obj_align(s_ui.seam_label, LV_ALIGN_LEFT_MID, 12, 0);
        }
    }
}

static void update_paddles(const pong_render_snapshot_t *snapshot)
{
    static const float paddle_x[2] = {
        PONG_PADDLE_LEFT_X,
        PONG_PADDLE_RIGHT_X,
    };
    for (size_t i = 0; i < 2; ++i) {
        const int32_t x = world_to_view_x(paddle_x[i]) - (int32_t)(PONG_PADDLE_WIDTH / 2.0f);
        const int32_t y = clamp_i32(
            (int32_t)(snapshot->world.paddles[i].y - PONG_PADDLE_HEIGHT / 2.0f),
            4, VIEW_H - (int32_t)PONG_PADDLE_HEIGHT - 4);
        const bool visible = x > -(int32_t)PONG_PADDLE_WIDTH && x < VIEW_W;
        set_pos_if_changed(s_ui.paddles[i], x, y);
        set_hidden(s_ui.paddles[i], !visible);
        if (visible && (!s_ui.paddle_position_initialized[i] ||
                        s_ui.paddle_y[i] != y ||
                        s_ui.paddle_visible[i] != visible)) {
            /* One narrow flush restores the complete old paddle path. */
            invalidate_view_area(
                x - PADDLE_DIRTY_MARGIN, 4,
                x + (int32_t)PONG_PADDLE_WIDTH + PADDLE_DIRTY_MARGIN - 1,
                VIEW_H - 5);
        }
        s_ui.paddle_position_initialized[i] = true;
        s_ui.paddle_visible[i] = visible;
        s_ui.paddle_y[i] = y;
    }
    if (!s_ui.paddle_style_initialized ||
        s_ui.paddle_style_role != snapshot->local_role) {
        s_ui.paddle_style_initialized = true;
        s_ui.paddle_style_role = snapshot->local_role;
        for (size_t i = 0; i < 2; ++i) {
            lv_obj_set_style_border_opa(
                s_ui.paddles[i],
                i == (size_t)snapshot->local_role ? LV_OPA_60 : LV_OPA_20, 0);
        }
    }
}

static void update_ball(const pong_render_snapshot_t *snapshot)
{
    const int32_t x = world_to_view_x(snapshot->world.ball.position.x);
    const int32_t y = (int32_t)snapshot->world.ball.position.y;
    const bool visible = x >= -(int32_t)PONG_BALL_RADIUS * 3 &&
                         x <= VIEW_W + (int32_t)PONG_BALL_RADIUS * 3;

    const bool trail_visible = visible &&
                               snapshot->world.phase == PONG_PHASE_PLAYING;
    const bool dirty = (visible || s_ui.ball_visible) &&
                       (!s_ui.ball_position_initialized || s_ui.ball_x != x ||
                        s_ui.ball_y != y || s_ui.ball_visible != visible ||
                        trail_visible);
    int32_t dirty_x1 = x - BALL_DIRTY_MARGIN;
    int32_t dirty_y1 = y - BALL_DIRTY_MARGIN;
    int32_t dirty_x2 = x + BALL_DIRTY_MARGIN;
    int32_t dirty_y2 = y + BALL_DIRTY_MARGIN;
    if (dirty) {
        if (s_ui.ball_position_initialized) {
            if (s_ui.ball_x - BALL_DIRTY_MARGIN < dirty_x1) {
                dirty_x1 = s_ui.ball_x - BALL_DIRTY_MARGIN;
            }
            if (s_ui.ball_y - BALL_DIRTY_MARGIN < dirty_y1) {
                dirty_y1 = s_ui.ball_y - BALL_DIRTY_MARGIN;
            }
            if (s_ui.ball_x + BALL_DIRTY_MARGIN > dirty_x2) {
                dirty_x2 = s_ui.ball_x + BALL_DIRTY_MARGIN;
            }
            if (s_ui.ball_y + BALL_DIRTY_MARGIN > dirty_y2) {
                dirty_y2 = s_ui.ball_y + BALL_DIRTY_MARGIN;
            }
        }
    }

    lv_display_t *display = lv_obj_get_display(s_ui.root);
    const bool invalidation_enabled = lv_display_is_invalidation_enabled(display);
    if (invalidation_enabled) {
        lv_display_enable_invalidation(display, false);
    }

    set_pos_if_changed(s_ui.ball_glow_outer, x - 25, y - 25);
    set_pos_if_changed(s_ui.ball_glow_inner, x - 18, y - 18);
    set_pos_if_changed(s_ui.ball, x - (int32_t)PONG_BALL_RADIUS,
                       y - (int32_t)PONG_BALL_RADIUS);
    set_hidden(s_ui.ball_glow_outer, !visible);
    set_hidden(s_ui.ball_glow_inner, !visible);
    set_hidden(s_ui.ball, !visible);

    float speed_scale = snapshot->world.ball.velocity.x;
    if (speed_scale < 0.0f) {
        speed_scale = -speed_scale;
    }
    float abs_y = snapshot->world.ball.velocity.y;
    if (abs_y < 0.0f) {
        abs_y = -abs_y;
    }
    if (abs_y > speed_scale) {
        speed_scale = abs_y;
    }
    if (speed_scale < 1.0f) {
        speed_scale = 1.0f;
    }

    for (size_t i = 0; i < TRAIL_COUNT; ++i) {
        const float distance = 5.0f + (float)i * 5.0f;
        const int32_t tx = x - (int32_t)(snapshot->world.ball.velocity.x * distance / speed_scale);
        const int32_t ty = y - (int32_t)(snapshot->world.ball.velocity.y * distance / speed_scale);
        const int32_t size = 14 - (int32_t)i;
        set_pos_if_changed(s_ui.trail[i], tx - size / 2, ty - size / 2);
        set_hidden(s_ui.trail[i], !trail_visible);
    }

    if (invalidation_enabled) {
        lv_display_enable_invalidation(display, true);
        if (dirty) {
            /* One invalid area replaces more than twenty object invalidations. */
            invalidate_view_area(dirty_x1, dirty_y1, dirty_x2, dirty_y2);
        }
    }
    s_ui.ball_position_initialized = true;
    s_ui.ball_visible = visible;
    s_ui.ball_x = x;
    s_ui.ball_y = y;
}

static void update_particles(const pong_render_snapshot_t *snapshot)
{
    if (snapshot->world.event_id != s_ui.particle_event_id &&
        snapshot->world.event != PONG_EVENT_NONE) {
        s_ui.particle_event_id = snapshot->world.event_id;
        s_ui.particle_start_tick = snapshot->world.tick;
        s_ui.particle_origin_x = (int16_t)world_to_view_x(snapshot->world.ball.position.x);
        s_ui.particle_origin_y = (int16_t)snapshot->world.ball.position.y;
        ESP_LOGD(TAG, "Particle effect start: event=%u id=%lu origin=(%d,%d)",
                 (unsigned)snapshot->world.event,
                 (unsigned long)snapshot->world.event_id,
                 (int)s_ui.particle_origin_x, (int)s_ui.particle_origin_y);
    }

    const uint32_t age = snapshot->world.tick - s_ui.particle_start_tick;
    const bool active = s_ui.particle_event_id != 0U &&
                        age < PARTICLE_LIFETIME_TICKS;
    const int32_t travel = active ?
        (int32_t)(age * PARTICLE_TRAVEL_STEPS / PARTICLE_LIFETIME_TICKS) : 0;

    int32_t particle_x[PARTICLE_COUNT];
    int32_t particle_y[PARTICLE_COUNT];
    bool particle_visible[PARTICLE_COUNT];
    lv_area_t current_bounds = {0};
    bool current_bounds_valid = false;
    for (size_t i = 0; i < PARTICLE_COUNT; ++i) {
        particle_x[i] = s_ui.particle_origin_x + s_particle_dx[i] * travel;
        particle_y[i] = s_ui.particle_origin_y + s_particle_dy[i] * travel;
        particle_visible[i] = active && particle_x[i] >= -8 &&
                              particle_x[i] <= VIEW_W + 8 &&
                              particle_y[i] >= -8 &&
                              particle_y[i] <= VIEW_H + 8;
        if (particle_visible[i]) {
            include_bounds(&current_bounds, &current_bounds_valid,
                           particle_x[i] - 4, particle_y[i] - 4,
                           particle_x[i] + 4, particle_y[i] + 4);
        }
    }

    lv_area_t dirty_bounds = current_bounds;
    bool dirty_bounds_valid = current_bounds_valid;
    if (s_ui.particle_bounds_valid) {
        include_bounds(&dirty_bounds, &dirty_bounds_valid,
                       s_ui.particle_bounds.x1, s_ui.particle_bounds.y1,
                       s_ui.particle_bounds.x2, s_ui.particle_bounds.y2);
    }

    lv_display_t *display = lv_obj_get_display(s_ui.root);
    const bool invalidation_enabled = lv_display_is_invalidation_enabled(display);
    if (invalidation_enabled) {
        lv_display_enable_invalidation(display, false);
    }
    for (size_t i = 0; i < PARTICLE_COUNT; ++i) {
        set_pos_if_changed(s_ui.particles[i], particle_x[i] - 3,
                           particle_y[i] - 3);
        set_bg_opa_if_changed(
            s_ui.particles[i],
            (lv_opa_t)(active ? 210U -
                (age * 180U / PARTICLE_LIFETIME_TICKS) : 0U));
        set_hidden(s_ui.particles[i], !particle_visible[i]);
    }
    if (invalidation_enabled) {
        lv_display_enable_invalidation(display, true);
        if (dirty_bounds_valid) {
            invalidate_view_area(dirty_bounds.x1, dirty_bounds.y1,
                                 dirty_bounds.x2, dirty_bounds.y2);
        }
    }
    s_ui.particle_bounds = current_bounds;
    s_ui.particle_bounds_valid = current_bounds_valid;
}

static void update_hud(const pong_render_snapshot_t *snapshot)
{
    if (s_ui.hud_initialized &&
        snapshot->world.tick - s_ui.last_hud_tick < HUD_UPDATE_TICKS) {
        return;
    }
    s_ui.hud_initialized = true;
    s_ui.last_hud_tick = snapshot->world.tick;

    lv_label_set_text_fmt(s_ui.score, "%u   :   %u",
                          (unsigned)snapshot->world.score[PONG_ROLE_LEFT],
                          (unsigned)snapshot->world.score[PONG_ROLE_RIGHT]);
    lv_label_set_text(s_ui.side,
                      s_ui.viewport == PONG_ROLE_LEFT ? "LEFT VIEW" : "RIGHT VIEW");

    if (snapshot->peer_present) {
        lv_label_set_text_fmt(s_ui.link, "LINK  %ums  %d dBm  loss %u%%",
                              (unsigned)snapshot->latency_ms, (int)snapshot->rssi,
                              (unsigned)snapshot->packet_loss_percent);
        lv_obj_set_style_text_color(
            s_ui.link, snapshot->latency_ms > 90U ? COLOR_AMBER : COLOR_GREEN, 0);
    } else {
        lv_label_set_text(s_ui.link, "LINK  WAITING FOR PEER");
        lv_obj_set_style_text_color(s_ui.link, COLOR_DANGER, 0);
    }

    lv_label_set_text_fmt(s_ui.dock, "%s  |  %s",
                          dock_text(snapshot->dock_state),
                          snapshot->joystick_ready ? "CONTROL OK" : "CONTROL --");
    lv_obj_set_style_text_color(s_ui.dock, dock_color(snapshot->dock_state), 0);

    if (snapshot->status[0] != '\0') {
        lv_label_set_text(s_ui.status, snapshot->status);
    } else if (snapshot->peer_label[0] != '\0') {
        lv_label_set_text_fmt(s_ui.status, "PEER  %s", snapshot->peer_label);
    } else {
        lv_label_set_text(s_ui.status, "DUAL SCREEN PONG");
    }
}

static void update_overlay(const pong_render_snapshot_t *snapshot)
{
    const pong_phase_t phase = snapshot->world.phase;
    const bool show = phase != PONG_PHASE_PLAYING;
    set_hidden(s_ui.overlay, !show);
    if (!show) {
        return;
    }

    const char *kicker = "SYSTEM";
    const char *title = "STARTING";
    char detail[96] = {0};
    lv_color_t accent = COLOR_CYAN;

    switch (phase) {
    case PONG_PHASE_CALIBRATING:
        kicker = "JOYSTICK CALIBRATION";
        title = "ROTATE FULL RANGE";
        snprintf(detail, sizeof(detail), "Then release the stick at center and hold still");
        accent = COLOR_AMBER;
        break;
    case PONG_PHASE_LOBBY:
        kicker = snapshot->is_host ? "LOBBY / HOST" : "LOBBY / GUEST";
        title = (snapshot->local_ready && snapshot->peer_ready) ? "BOTH READY" :
                (snapshot->local_ready ? "YOU ARE READY" : "PRESS 1 TO READY");
        snprintf(detail, sizeof(detail), "Local %s   Peer %s",
                 snapshot->local_ready ? "READY" : "WAIT",
                 snapshot->peer_ready ? "READY" : "WAIT");
        accent = snapshot->local_ready ? COLOR_GREEN : COLOR_CYAN;
        break;
    case PONG_PHASE_COUNTDOWN:
        kicker = "MATCH START";
        snprintf(detail, sizeof(detail), "Serve: %s side",
                 snapshot->world.serving_role == PONG_ROLE_LEFT ? "LEFT" : "RIGHT");
        if (snapshot->world.countdown_ms == 0U) {
            title = "GO";
        } else if (snapshot->world.countdown_ms <= 1000U) {
            title = "1";
        } else if (snapshot->world.countdown_ms <= 2000U) {
            title = "2";
        } else {
            title = "3";
        }
        accent = COLOR_GREEN;
        break;
    case PONG_PHASE_PAUSED:
        kicker = "MATCH PAUSED";
        title = snapshot->peer_present ? "HOLD POSITION" : "PEER LOST";
        snprintf(detail, sizeof(detail), "Reconnect grace %u seconds",
                 (unsigned)(PONG_RECONNECT_GRACE_MS / 1000U));
        accent = snapshot->peer_present ? COLOR_AMBER : COLOR_DANGER;
        break;
    case PONG_PHASE_ROUND_OVER:
        kicker = "POINT";
        title = snapshot->world.score[PONG_ROLE_LEFT] >
                        snapshot->world.score[PONG_ROLE_RIGHT] ? "LEFT LEADS" : "RIGHT LEADS";
        snprintf(detail, sizeof(detail), "Next serve in position");
        accent = COLOR_MAGENTA;
        break;
    case PONG_PHASE_MATCH_OVER: {
        const pong_role_t winner = snapshot->world.score[PONG_ROLE_LEFT] >
                                           snapshot->world.score[PONG_ROLE_RIGHT] ?
                                       PONG_ROLE_LEFT : PONG_ROLE_RIGHT;
        kicker = "MATCH COMPLETE";
        title = winner == snapshot->local_role ? "VICTORY" : "GOOD GAME";
        snprintf(detail, sizeof(detail), "Final score  %u : %u",
                 (unsigned)snapshot->world.score[PONG_ROLE_LEFT],
                 (unsigned)snapshot->world.score[PONG_ROLE_RIGHT]);
        accent = winner == snapshot->local_role ? COLOR_GREEN : COLOR_MAGENTA;
        break;
    }
    case PONG_PHASE_BOOT:
    default:
        snprintf(detail, sizeof(detail), "Preparing display, control and peer link");
        break;
    }

    set_label_text_if_changed(s_ui.overlay_kicker, kicker);
    set_label_text_if_changed(s_ui.overlay_title, title);
    set_label_text_if_changed(s_ui.overlay_detail, detail);
    lv_obj_set_style_text_color(s_ui.overlay_kicker, accent, 0);
    lv_obj_set_style_border_color(s_ui.overlay, accent, 0);
}

esp_err_t pong_ui_create(lv_obj_t *parent, pong_role_t viewport)
{
    if (s_ui.created) {
        return ESP_ERR_INVALID_STATE;
    }
    if (viewport != PONG_ROLE_LEFT && viewport != PONG_ROLE_RIGHT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!parent) {
        parent = lv_screen_active();
    }
    if (!parent) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_ui, 0, sizeof(s_ui));
    s_ui.viewport = viewport;
    s_ui.seam_opacity = 180U;
    s_ui.root = make_box(parent, VIEW_W, VIEW_H, COLOR_BG, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad_color(s_ui.root, lv_color_hex(0x11142A), 0);
    lv_obj_set_style_bg_grad_dir(s_ui.root, LV_GRAD_DIR_VER, 0);
    lv_obj_set_pos(s_ui.root, 0, 0);

    for (size_t i = 0; i < GRID_VERTICAL_COUNT; ++i) {
        s_ui.grid_vertical[i] = make_box(s_ui.root, 1, VIEW_H, COLOR_GRID, LV_OPA_COVER, 0);
        lv_obj_set_pos(s_ui.grid_vertical[i], GRID_STEP * ((int32_t)i + 1), 0);
    }
    for (size_t i = 0; i < GRID_HORIZONTAL_COUNT; ++i) {
        s_ui.grid_horizontal[i] = make_box(s_ui.root, VIEW_W, 1, COLOR_GRID, LV_OPA_COVER, 0);
        lv_obj_set_pos(s_ui.grid_horizontal[i], 0, GRID_STEP * ((int32_t)i + 1));
    }

    s_ui.seam_core = make_box(s_ui.root, 3, VIEW_H, COLOR_MUTED, LV_OPA_20, 0);
    for (size_t i = 0; i < SEAM_SEGMENT_COUNT; ++i) {
        s_ui.seam_segments[i] = make_box(s_ui.root, 7, 32, COLOR_CYAN, LV_OPA_70, 4);
    }

    for (size_t i = 0; i < TRAIL_COUNT; ++i) {
        const int32_t size = 14 - (int32_t)i;
        s_ui.trail[i] = make_box(s_ui.root, size, size, COLOR_BLUE,
                                 (lv_opa_t)(105U - i * 10U), LV_RADIUS_CIRCLE);
    }
    s_ui.ball_glow_outer = make_box(s_ui.root, 50, 50, COLOR_CYAN, LV_OPA_20, LV_RADIUS_CIRCLE);
    s_ui.ball_glow_inner = make_box(s_ui.root, 36, 36, COLOR_CYAN, LV_OPA_40, LV_RADIUS_CIRCLE);
    s_ui.ball = make_box(s_ui.root, (int32_t)PONG_BALL_RADIUS * 2,
                         (int32_t)PONG_BALL_RADIUS * 2, COLOR_TEXT,
                         LV_OPA_COVER, LV_RADIUS_CIRCLE);

    for (size_t i = 0; i < 2; ++i) {
        s_ui.paddles[i] = make_box(s_ui.root, (int32_t)PONG_PADDLE_WIDTH,
                                   (int32_t)PONG_PADDLE_HEIGHT,
                                   i == 0 ? COLOR_CYAN : COLOR_MAGENTA,
                                   LV_OPA_COVER, 8);
        lv_obj_set_style_border_width(s_ui.paddles[i], 2, 0);
        lv_obj_set_style_border_color(s_ui.paddles[i], COLOR_TEXT, 0);
    }
    for (size_t i = 0; i < PARTICLE_COUNT; ++i) {
        s_ui.particles[i] = make_box(s_ui.root, 6, 6,
                                     (i & 1U) ? COLOR_MAGENTA : COLOR_CYAN,
                                     LV_OPA_COVER, LV_RADIUS_CIRCLE);
        set_hidden(s_ui.particles[i], true);
    }

    s_ui.score = make_label(s_ui.root, COLOR_TEXT);
    lv_obj_set_style_text_font(s_ui.score, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_letter_space(s_ui.score, 5, 0);
    lv_obj_align(s_ui.score, LV_ALIGN_TOP_MID, 0, 14);
    s_ui.side = make_label(s_ui.root, COLOR_MUTED);
    lv_obj_set_style_text_font(s_ui.side, &lv_font_montserrat_14, 0);
    lv_obj_align(s_ui.side, LV_ALIGN_TOP_LEFT, 16, 14);
    s_ui.link = make_label(s_ui.root, COLOR_MUTED);
    lv_obj_set_style_text_font(s_ui.link, &lv_font_montserrat_14, 0);
    lv_obj_align(s_ui.link, LV_ALIGN_TOP_RIGHT, -16, 14);
    s_ui.dock = make_label(s_ui.root, COLOR_MUTED);
    lv_obj_set_style_text_font(s_ui.dock, &lv_font_montserrat_14, 0);
    lv_obj_align(s_ui.dock, LV_ALIGN_BOTTOM_LEFT, 16, -14);
    s_ui.status = make_label(s_ui.root, COLOR_MUTED);
    lv_obj_set_style_text_font(s_ui.status, &lv_font_montserrat_14, 0);
    lv_obj_set_width(s_ui.status, 220);
    lv_obj_set_style_text_align(s_ui.status, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_ui.status, LV_ALIGN_BOTTOM_RIGHT, -16, -14);
    s_ui.seam_label = make_label(s_ui.root, COLOR_CYAN);
    lv_obj_set_style_text_font(s_ui.seam_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(s_ui.seam_label, 1, 0);
    s_ui.seam_label_viewport = viewport == PONG_ROLE_LEFT ?
                               PONG_ROLE_RIGHT : PONG_ROLE_LEFT;

    s_ui.overlay = make_box(s_ui.root, 344, 172, COLOR_PANEL, LV_OPA_COVER, 18);
    lv_obj_set_style_border_width(s_ui.overlay, 1, 0);
    lv_obj_set_style_border_opa(s_ui.overlay, LV_OPA_70, 0);
    lv_obj_center(s_ui.overlay);
    s_ui.overlay_kicker = make_label(s_ui.overlay, COLOR_CYAN);
    lv_obj_set_style_text_letter_space(s_ui.overlay_kicker, 2, 0);
    lv_obj_align(s_ui.overlay_kicker, LV_ALIGN_TOP_MID, 0, 22);
    s_ui.overlay_title = make_label(s_ui.overlay, COLOR_TEXT);
    lv_obj_set_style_text_font(s_ui.overlay_title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_letter_space(s_ui.overlay_title, 3, 0);
    lv_obj_align(s_ui.overlay_title, LV_ALIGN_CENTER, 0, -2);
    s_ui.overlay_detail = make_label(s_ui.overlay, COLOR_MUTED);
    lv_obj_set_width(s_ui.overlay_detail, 300);
    lv_obj_set_style_text_align(s_ui.overlay_detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_ui.overlay_detail, LV_ALIGN_BOTTOM_MID, 0, -22);

    s_ui.created = true;
    ESP_LOGI(TAG, "UI created for %s viewport with fixed reusable objects",
             viewport == PONG_ROLE_LEFT ? "left" : "right");
    return ESP_OK;
}

void pong_ui_set_viewport(pong_role_t viewport)
{
    if (!s_ui.created ||
        (viewport != PONG_ROLE_LEFT && viewport != PONG_ROLE_RIGHT)) {
        return;
    }
    s_ui.viewport = viewport;
}

void pong_ui_update(const pong_render_snapshot_t *snapshot)
{
    if (!s_ui.created || !snapshot) {
        return;
    }

    update_seam(snapshot);
    update_paddles(snapshot);
    update_ball(snapshot);
    update_particles(snapshot);
    update_hud(snapshot);
    update_overlay(snapshot);
}

bool pong_ui_is_created(void)
{
    return s_ui.created;
}
