/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "interaction_ui.h"

#include <string.h>

#include "interaction_controller.h"
#include "lvgl.h"
#include "mosaico_interaction.h"
#include "mosaico_mag_classifier.h"
#include "mosaico_topology.h"

#define COLOR_BG       lv_color_hex(0x070a12)
#define COLOR_PANEL    lv_color_hex(0x151b2b)
#define COLOR_TEXT     lv_color_hex(0xf8fafc)
#define COLOR_MUTED    lv_color_hex(0x94a3b8)
#define COLOR_ACCENT   lv_color_hex(0x38bdf8)
#define COLOR_CONTACT  lv_color_hex(0x22c55e)
#define COLOR_WARN     lv_color_hex(0xf59e0b)

static lv_obj_t *s_status;
static lv_obj_t *s_detail;
static lv_obj_t *s_game;
static lv_obj_t *s_title;
static lv_obj_t *s_clear_button;
static lv_obj_t *s_rotate_button;
static lv_obj_t *s_reset_button;
static lv_obj_t *s_energy_orb;
static lv_obj_t *s_edge_buttons[4];
static uint32_t s_energy_animation_session;
static uint32_t s_energy_animation_event;
static mosaico_energy_phase_t s_energy_animation_phase;

typedef struct {
    int32_t start_x;
    int32_t start_y;
    int32_t end_x;
    int32_t end_y;
} energy_animation_path_t;

static energy_animation_path_t s_energy_animation_path;

typedef struct {
    mosaico_edge_t edge;
    const char *label;
} edge_button_data_t;

static edge_button_data_t s_edge_data[] = {
    {.edge = MOSAICO_EDGE_TOP, .label = "TOP"},
    {.edge = MOSAICO_EDGE_RIGHT, .label = "RIGHT"},
    {.edge = MOSAICO_EDGE_BOTTOM, .label = "BOTTOM"},
    {.edge = MOSAICO_EDGE_LEFT, .label = "LEFT"},
};

static void edge_button_cb(lv_event_t *event)
{
    const edge_button_data_t *data = lv_event_get_user_data(event);
    interaction_controller_set_mock_edge(data->edge);
}

static void clear_button_cb(lv_event_t *event)
{
    (void)event;
    interaction_controller_set_mock_edge(MOSAICO_EDGE_NONE);
}

static void rotate_button_cb(lv_event_t *event)
{
    (void)event;
    interaction_controller_rotate_mock();
}

static void reset_game_button_cb(lv_event_t *event)
{
    (void)event;
    interaction_controller_snapshot_t snapshot = {0};
    interaction_controller_get_snapshot(&snapshot);
    if (snapshot.hardware_source) {
        interaction_controller_recalibrate();
    } else {
        interaction_controller_reset_game();
    }
}

static lv_obj_t *make_button(
    lv_obj_t *parent,
    const char *text,
    int width,
    int height,
    lv_event_cb_t callback,
    void *user_data)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, width, height);
    lv_obj_set_style_bg_color(button, COLOR_PANEL, 0);
    lv_obj_set_style_bg_color(button, COLOR_ACCENT, LV_STATE_PRESSED);
    lv_obj_set_style_radius(button, 12, 0);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
    lv_obj_center(label);
    return button;
}

static void format_edge_mask(mosaico_edge_mask_t mask, char *buffer, size_t length)
{
    static const struct {
        mosaico_edge_mask_t bit;
        const char *name;
    } edges[] = {
        {MOSAICO_EDGE_MASK_TOP, "TOP"},
        {MOSAICO_EDGE_MASK_RIGHT, "RIGHT"},
        {MOSAICO_EDGE_MASK_BOTTOM, "BOTTOM"},
        {MOSAICO_EDGE_MASK_LEFT, "LEFT"},
    };
    if (!buffer || length == 0) {
        return;
    }
    buffer[0] = '\0';
    if ((mask & MOSAICO_EDGE_MASK_ALL) == 0) {
        strlcpy(buffer, "NONE", length);
        return;
    }
    for (size_t i = 0; i < sizeof(edges) / sizeof(edges[0]); ++i) {
        if ((mask & edges[i].bit) == 0) {
            continue;
        }
        if (buffer[0] != '\0') {
            strlcat(buffer, "+", length);
        }
        strlcat(buffer, edges[i].name, length);
    }
}

static void energy_animation_exec_cb(void *object, int32_t progress)
{
    const int32_t x = s_energy_animation_path.start_x +
        ((s_energy_animation_path.end_x - s_energy_animation_path.start_x) *
         progress) / 1000;
    const int32_t y = s_energy_animation_path.start_y +
        ((s_energy_animation_path.end_y - s_energy_animation_path.start_y) *
         progress) / 1000;
    lv_obj_set_pos(object, x - 28, y - 28);
}

static void get_energy_edge_position(
    mosaico_edge_t edge,
    int32_t *x,
    int32_t *y)
{
    int32_t edge_x = 240;
    int32_t edge_y = 220;
    switch (edge) {
    case MOSAICO_EDGE_TOP:    edge_y = 0; break;
    case MOSAICO_EDGE_RIGHT:  edge_x = 480; break;
    case MOSAICO_EDGE_BOTTOM: edge_y = 480; break;
    case MOSAICO_EDGE_LEFT:   edge_x = 0; break;
    default: break;
    }
    *x = edge_x;
    *y = edge_y;
}

static mosaico_edge_t physical_edge_to_display(
    mosaico_edge_t physical_edge,
    uint16_t display_rotation_delta)
{
    mosaico_edge_t display_edge = physical_edge;
    const uint16_t inverse_rotation =
        (uint16_t)((360U - display_rotation_delta) % 360U);
    if (mosaico_edge_rotate(physical_edge, inverse_rotation, &display_edge) != ESP_OK) {
        return physical_edge;
    }
    return display_edge;
}

static mosaico_edge_mask_t physical_mask_to_display(
    mosaico_edge_mask_t physical_mask,
    uint16_t display_rotation_delta)
{
    mosaico_edge_mask_t display_mask = 0;
    for (size_t i = 0; i < sizeof(s_edge_data) / sizeof(s_edge_data[0]); ++i) {
        const mosaico_edge_t physical_edge = s_edge_data[i].edge;
        if ((physical_mask & mosaico_edge_to_mask(physical_edge)) == 0) {
            continue;
        }
        const mosaico_edge_t display_edge = physical_edge_to_display(
            physical_edge, display_rotation_delta);
        display_mask |= mosaico_edge_to_mask(display_edge);
    }
    return display_mask;
}

static void start_energy_animation(
    const interaction_controller_snapshot_t *snapshot)
{
    int32_t edge_x;
    int32_t edge_y;
    const mosaico_edge_t display_edge = physical_edge_to_display(
        snapshot->energy_edge, snapshot->display_rotation_delta);
    get_energy_edge_position(display_edge, &edge_x, &edge_y);

    const int32_t center_x = 240;
    const int32_t center_y = 220;
    const bool receiving = snapshot->energy_phase == MOSAICO_ENERGY_RECEIVING;
    s_energy_animation_path.start_x = receiving ? edge_x : center_x;
    s_energy_animation_path.start_y = receiving ? edge_y : center_y;
    s_energy_animation_path.end_x = receiving ? center_x : edge_x;
    s_energy_animation_path.end_y = receiving ? center_y : edge_y;

    lv_anim_delete(s_energy_orb, energy_animation_exec_cb);
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, s_energy_orb);
    lv_anim_set_exec_cb(&animation, energy_animation_exec_cb);
    lv_anim_set_values(
        &animation, snapshot->energy_progress_permille, 1000);
    const uint32_t remaining_ms =
        (MOSAICO_ENERGY_TRANSFER_DEFAULT_DURATION_MS *
         (1000U - snapshot->energy_progress_permille)) / 1000U;
    lv_anim_set_duration(&animation, remaining_ms ? remaining_ms : 1U);
    lv_anim_set_path_cb(&animation, lv_anim_path_linear);
    lv_obj_clear_flag(s_energy_orb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_energy_orb);
    lv_anim_start(&animation);
    s_energy_animation_session = snapshot->energy_session_id;
    s_energy_animation_event = snapshot->energy_event_id;
    s_energy_animation_phase = snapshot->energy_phase;
}

static void update_energy_orb(const interaction_controller_snapshot_t *snapshot)
{
    if (snapshot->display_rotation_pending) {
        lv_anim_delete(s_energy_orb, energy_animation_exec_cb);
        lv_obj_add_flag(s_energy_orb, LV_OBJ_FLAG_HIDDEN);
        s_energy_animation_session = 0;
        s_energy_animation_event = 0;
        s_energy_animation_phase = MOSAICO_ENERGY_IDLE;
        return;
    }
    if (snapshot->energy_phase == MOSAICO_ENERGY_IDLE ||
        snapshot->energy_edge == MOSAICO_EDGE_NONE) {
        lv_anim_delete(s_energy_orb, energy_animation_exec_cb);
        lv_obj_add_flag(s_energy_orb, LV_OBJ_FLAG_HIDDEN);
        s_energy_animation_session = 0;
        s_energy_animation_event = 0;
        s_energy_animation_phase = MOSAICO_ENERGY_IDLE;
        return;
    }

    if ((snapshot->energy_phase == MOSAICO_ENERGY_SENDING ||
         snapshot->energy_phase == MOSAICO_ENERGY_RECEIVING) &&
        (s_energy_animation_session != snapshot->energy_session_id ||
         s_energy_animation_event != snapshot->energy_event_id ||
         s_energy_animation_phase != snapshot->energy_phase)) {
        start_energy_animation(snapshot);
        return;
    }

    if (snapshot->energy_phase == MOSAICO_ENERGY_SENT) {
        lv_anim_delete(s_energy_orb, energy_animation_exec_cb);
        lv_obj_add_flag(s_energy_orb, LV_OBJ_FLAG_HIDDEN);
    } else if (snapshot->energy_phase == MOSAICO_ENERGY_WAIT_HANDOFF) {
        lv_anim_delete(s_energy_orb, energy_animation_exec_cb);
        lv_obj_add_flag(s_energy_orb, LV_OBJ_FLAG_HIDDEN);
    } else if (snapshot->energy_phase == MOSAICO_ENERGY_WAIT_COMPLETE) {
        int32_t edge_x = 0;
        int32_t edge_y = 0;
        const mosaico_edge_t display_edge = physical_edge_to_display(
            snapshot->energy_edge, snapshot->display_rotation_delta);
        get_energy_edge_position(display_edge, &edge_x, &edge_y);
        lv_anim_delete(s_energy_orb, energy_animation_exec_cb);
        lv_obj_set_pos(s_energy_orb, edge_x - 28, edge_y - 28);
        lv_obj_clear_flag(s_energy_orb, LV_OBJ_FLAG_HIDDEN);
    } else if (snapshot->energy_phase == MOSAICO_ENERGY_RECEIVED) {
        lv_anim_delete(s_energy_orb, energy_animation_exec_cb);
        lv_obj_set_pos(s_energy_orb, 240 - 28, 220 - 28);
        lv_obj_clear_flag(s_energy_orb, LV_OBJ_FLAG_HIDDEN);
    }
}

static void refresh_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    interaction_controller_snapshot_t snapshot = {0};
    interaction_controller_get_snapshot(&snapshot);
    const mosaico_edge_mask_t display_detected_mask = physical_mask_to_display(
        snapshot.detected_mask, snapshot.display_rotation_delta);

    char detected[32];
    format_edge_mask(snapshot.detected_mask, detected, sizeof(detected));

    if (snapshot.hardware_source) {
        lv_label_set_text(s_title, "MOSAICO ENERGY RELAY");
        const bool calibration_ready =
            snapshot.calibration_state == MOSAICO_MAG_CALIBRATION_READY;
        const char *state;
        if (!calibration_ready) {
            state = snapshot.calibration_state == MOSAICO_MAG_CALIBRATION_FAILED ?
                "CAL FAILED / SEPARATE" : "CALIBRATING / SEPARATE";
        } else if (snapshot.display_rotation_pending) {
            state = "ALIGNING SCREENS";
        } else if (snapshot.energy_phase == MOSAICO_ENERGY_SENDING) {
            state = "MOVING TO SEAM";
        } else if (snapshot.energy_phase == MOSAICO_ENERGY_WAIT_COMPLETE) {
            state = "CROSSING SCREENS";
        } else if (snapshot.energy_phase == MOSAICO_ENERGY_WAIT_HANDOFF) {
            state = "WAITING AT SEAM";
        } else if (snapshot.energy_phase == MOSAICO_ENERGY_RECEIVING) {
            state = "ENTERING THIS SCREEN";
        } else if (snapshot.energy_phase == MOSAICO_ENERGY_SENT) {
            state = "ENERGY SENT";
        } else if (snapshot.energy_phase == MOSAICO_ENERGY_RECEIVED) {
            state = "ENERGY RECEIVED";
        } else {
            state = snapshot.detected_mask ? "NEGOTIATING" : "CONNECT AN EDGE";
        }
        lv_label_set_text_fmt(s_status, "%s  %s", state, detected);
        lv_obj_set_style_text_color(
            s_status, !calibration_ready ? COLOR_WARN :
                (snapshot.detected_mask ? COLOR_CONTACT : COLOR_MUTED), 0);
        lv_label_set_text_fmt(
            s_detail,
            "sensor %s  filter %u/%u  baseline %ld  saturation %s\n"
            "event %s  radio %s  link %s  peer %04llX  display %u\n"
            "mesh %u nodes  root %04llX  topology v%lu%s",
            snapshot.sensors_valid ? "VALID" : "WARMUP/INVALID",
            snapshot.filtered_samples,
            MOSAICO_MAG_FILTER_MAX_SAMPLES,
            (long)snapshot.baseline_right_y,
            snapshot.saturated ? "YES" : "NO",
            snapshot.last_event,
            snapshot.radio_ready ? "READY" : "LOCAL",
            snapshot.peer_connected ? "COMMITTED" :
                (snapshot.detected_mask ? "SEARCHING" : "IDLE"),
            (unsigned long long)(snapshot.last_peer_id & 0xffff),
            snapshot.display_rotation,
            snapshot.mesh_node_count,
            (unsigned long long)(snapshot.mesh_root_id & 0xffff),
            (unsigned long)snapshot.topology_version,
            snapshot.mesh_orientation_conflict ? "  CONFLICT" : "");
        if (snapshot.energy_phase == MOSAICO_ENERGY_IDLE) {
            lv_label_set_text(s_game, "BUILD A MESH TO ROUTE ENERGY");
        } else {
            lv_label_set_text_fmt(
                s_game, "EVENT %08lX  HOP %lu  |  %s %u%%",
                (unsigned long)snapshot.energy_event_id,
                (unsigned long)snapshot.energy_hop,
                mosaico_energy_phase_to_string(snapshot.energy_phase),
                snapshot.energy_progress_permille / 10);
        }
        update_energy_orb(&snapshot);
        for (size_t i = 0; i < 4; ++i) {
            lv_obj_set_style_bg_color(
                s_edge_buttons[i],
                (display_detected_mask & mosaico_edge_to_mask(s_edge_data[i].edge)) ?
                    COLOR_CONTACT : COLOR_PANEL,
                0);
        }
        return;
    }

    const char *state = "IDLE";
    lv_color_t state_color = COLOR_MUTED;
    if (snapshot.state == MOSAICO_INTERACTION_STATE_APPROACH) {
        state = "APPROACH";
        state_color = COLOR_WARN;
    } else if (snapshot.state == MOSAICO_INTERACTION_STATE_ATTACHED) {
        state = "ATTACHED";
        state_color = COLOR_CONTACT;
    }
    lv_label_set_text_fmt(s_status, "%s  %s  %u deg",
                          state,
                          mosaico_edge_to_string(snapshot.attached_edge),
                          snapshot.attached_rotation);
    lv_obj_set_style_text_color(s_status, state_color, 0);

    lv_label_set_text_fmt(
        s_detail,
        "input %s/%u  event %s\n"
        "orbit CW %lu  CCW %lu  topo v%lu\n"
        "radio %s  peer %04llX",
        mosaico_edge_to_string(snapshot.simulated_edge),
        snapshot.simulated_rotation,
        snapshot.last_event,
        (unsigned long)snapshot.orbit_cw_count,
        (unsigned long)snapshot.orbit_ccw_count,
        (unsigned long)snapshot.topology_version,
        snapshot.radio_ready ? "READY" : "LOCAL",
        (unsigned long long)(snapshot.last_peer_id & 0xffff));

    char tokens[20] = "-- -- -- --";
    for (size_t i = 0; i < snapshot.idiom_count && i < 4; ++i) {
        const char *edge = mosaico_edge_to_string((mosaico_edge_t)snapshot.idiom_tokens[i]);
        tokens[i * 3] = edge[0];
        tokens[i * 3 + 1] = edge[1] ? edge[1] : '-';
    }
    lv_label_set_text_fmt(s_game, "PAIR %s    CHAIN %s%s",
                          snapshot.pair_match ? "MATCH" : "--",
                          tokens,
                          snapshot.idiom_complete ? "  COMPLETE" : "");

    for (size_t i = 0; i < 4; ++i) {
        const mosaico_edge_t display_edge = physical_edge_to_display(
            snapshot.simulated_edge, snapshot.display_rotation_delta);
        lv_obj_set_style_bg_color(
            s_edge_buttons[i],
            display_edge == s_edge_data[i].edge ? COLOR_ACCENT : COLOR_PANEL,
            0);
    }
}

void interaction_ui_create(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, COLOR_BG, 0);

    s_title = lv_label_create(screen);
    lv_label_set_text(s_title, "MAGNETIC INTERACTION / MOCK");
    lv_obj_set_style_text_color(s_title, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_20, 0);
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 18);

    s_status = lv_label_create(screen);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_20, 0);
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 58);

    s_edge_buttons[0] = make_button(
        screen, s_edge_data[0].label, 110, 52,
        edge_button_cb, &s_edge_data[0]);
    lv_obj_align(s_edge_buttons[0], LV_ALIGN_TOP_MID, 0, 96);

    s_edge_buttons[1] = make_button(
        screen, s_edge_data[1].label, 110, 52,
        edge_button_cb, &s_edge_data[1]);
    lv_obj_align(s_edge_buttons[1], LV_ALIGN_RIGHT_MID, -22, -42);

    s_edge_buttons[2] = make_button(
        screen, s_edge_data[2].label, 110, 52,
        edge_button_cb, &s_edge_data[2]);
    lv_obj_align(s_edge_buttons[2], LV_ALIGN_BOTTOM_MID, 0, -98);

    s_edge_buttons[3] = make_button(
        screen, s_edge_data[3].label, 110, 52,
        edge_button_cb, &s_edge_data[3]);
    lv_obj_align(s_edge_buttons[3], LV_ALIGN_LEFT_MID, 22, -42);

    s_clear_button = make_button(screen, "CLEAR", 100, 52, clear_button_cb, NULL);
    lv_obj_align(s_clear_button, LV_ALIGN_CENTER, -56, -42);
    s_rotate_button = make_button(screen, "ROTATE", 100, 52, rotate_button_cb, NULL);
    lv_obj_align(s_rotate_button, LV_ALIGN_CENTER, 56, -42);

    s_energy_orb = lv_obj_create(screen);
    lv_obj_set_size(s_energy_orb, 56, 56);
    lv_obj_set_style_radius(s_energy_orb, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_energy_orb, COLOR_ACCENT, 0);
    lv_obj_set_style_border_width(s_energy_orb, 0, 0);
    lv_obj_set_style_shadow_color(s_energy_orb, COLOR_ACCENT, 0);
    lv_obj_set_style_shadow_width(s_energy_orb, 24, 0);
    lv_obj_set_style_shadow_opa(s_energy_orb, LV_OPA_60, 0);
    lv_obj_remove_flag(s_energy_orb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_energy_orb, LV_OBJ_FLAG_HIDDEN);

    s_detail = lv_label_create(screen);
    lv_obj_set_width(s_detail, 420);
    lv_obj_set_style_text_align(s_detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_detail, COLOR_MUTED, 0);
    lv_obj_set_style_text_font(s_detail, &lv_font_montserrat_14, 0);
    lv_obj_align(s_detail, LV_ALIGN_CENTER, 0, 58);

    s_game = lv_label_create(screen);
    lv_obj_set_width(s_game, 440);
    lv_obj_set_style_text_align(s_game, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_game, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(s_game, &lv_font_montserrat_16, 0);
    lv_obj_align(s_game, LV_ALIGN_BOTTOM_MID, 0, -58);

    s_reset_button = make_button(
        screen, "RESET GAME", 140, 42, reset_game_button_cb, NULL);
    lv_obj_align(s_reset_button, LV_ALIGN_BOTTOM_MID, 0, -10);

    interaction_controller_snapshot_t snapshot = {0};
    interaction_controller_get_snapshot(&snapshot);
    if (snapshot.hardware_source) {
        lv_obj_add_flag(s_clear_button, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_rotate_button, LV_OBJ_FLAG_HIDDEN);
        lv_obj_t *label = lv_obj_get_child(s_reset_button, 0);
        if (label) {
            lv_label_set_text(label, "RECALIBRATE");
        }
    }

    lv_timer_create(refresh_timer_cb, 100, NULL);
    refresh_timer_cb(NULL);
}
