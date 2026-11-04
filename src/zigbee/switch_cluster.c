#include "switch_cluster.h"
#include "base_components/relay.h"
#include "cluster_common.h"
#include "consts.h"
#include "device_config/nvm_items.h"
#include "hal/nvm.h"

#include "hal/printf_selector.h"
#include "hal/system.h"
#include "hal/tasks.h"
#include "hal/timer.h"
#include "relay_cluster.h"
#include "zigbee_commands.h"

const uint8_t  multistate_out_of_service = 0;
const uint8_t  multistate_flags          = 0;
const uint16_t multistate_num_of_states  = 3;

#define MULTISTATE_NOT_PRESSED     0
#define MULTISTATE_PRESS           1
#define MULTISTATE_LONG_PRESS      2
#define MULTISTATE_POSITION_ON     3
#define MULTISTATE_POSITION_OFF    4

extern zigbee_relay_cluster relay_clusters[];
extern uint8_t relay_clusters_cnt;
extern zigbee_switch_cluster switch_clusters[];
extern uint8_t switch_clusters_cnt;

// Upper limit for a long press loaded from NV (same as the Z2M converter)
#define LONG_PRESS_MAX_MS                  5000

// Smart long-press dimming (fork)
#define DIMMING_THRESHOLD_DEFAULT_PCT      60
#define DIMMING_THRESHOLD_MAX_PCT          100
// Pressing again within this window after a dim ended keeps its direction.
// 0 disables the rule (threshold always decides).
#define DIMMING_RESET_WINDOW_DEFAULT_MS    1500
#define DIMMING_RESET_WINDOW_MAX_MS        5000
#define LAMP_LEVEL_MIN                     1
#define LAMP_LEVEL_MAX                     254
// Lamp off -> on at this level. One above the minimum: a "with On/Off"
// command that ends at the minimum level switches the lamp off (ZCL).
#define LAMP_LEVEL_FROM_OFF                (LAMP_LEVEL_MIN + 1)
// Move up is sent this long after MoveToLevel, so it reaches the lamp second
#define DIM_UP_DELAY_MS                    250
#define LAMP_STATE_ONOFF_VALID             0x01
#define LAMP_STATE_LEVEL_VALID             0x02

typedef struct {
    uint8_t threshold_pct;
} zigbee_switch_dimming_config;

typedef struct {
    uint16_t reset_window_ms;
} zigbee_switch_dimming_window_config;

static void switch_cluster_load_dimming_from_nv(zigbee_switch_cluster *cluster);
static void switch_cluster_store_dimming_to_nv(zigbee_switch_cluster *cluster);
static void switch_cluster_store_dimming_window_to_nv(
    zigbee_switch_cluster *cluster);
static void switch_cluster_load_dimming_window_from_nv(
    zigbee_switch_cluster *cluster);

void switch_cluster_on_button_press(zigbee_switch_cluster *cluster);
void switch_cluster_on_button_release(zigbee_switch_cluster *cluster);
void switch_cluster_on_button_long_press(zigbee_switch_cluster *cluster);
static void switch_cluster_dim_up_task_handler(void *arg);
static bool switch_cluster_has_valid_relay(
    const zigbee_switch_cluster *cluster);

zigbee_switch_cluster *switch_cluster_by_endpoint[10];

static void sync_switch_indicator_led(zigbee_switch_cluster *cluster) {
    if (cluster->indicator_led == NULL) {
        return;
    }

    if (cluster->relay_mode != ZCL_ONOFF_CONFIGURATION_RELAY_MODE_DETACHED &&
        switch_cluster_has_valid_relay(cluster)) {
        return;
    }

    led_off(cluster->indicator_led);
}

void update_switch_clusters() {
    for (int i = 0; i < switch_clusters_cnt; i++) {
        sync_switch_indicator_led(&switch_clusters[i]);
    }
}

static bool switch_cluster_has_valid_relay(const zigbee_switch_cluster *cluster) {
    return cluster->relay_index > 0 && cluster->relay_index <= relay_clusters_cnt;
}

static void switch_cluster_flash_indicator(zigbee_switch_cluster *cluster) {
    if (cluster->indicator_led == NULL) {
        return;
    }
    // Skip flash when relay is attached — the relay toggle itself changes the
    // indicator, and the blink would race with sync_indicator_led.
    if (cluster->relay_mode != ZCL_ONOFF_CONFIGURATION_RELAY_MODE_DETACHED &&
        switch_cluster_has_valid_relay(cluster)) {
        return;
    }
    // Only flash when LED is idle (not in "not connected" forever-blink)
    if (cluster->indicator_led->blink_times_left == 0) {
        led_blink(cluster->indicator_led, 50, 50, 1);
    }
}

void switch_cluster_store_attrs_to_nv(zigbee_switch_cluster *cluster);
void switch_cluster_load_attrs_from_nv(zigbee_switch_cluster *cluster);
void switch_cluster_on_write_attr(zigbee_switch_cluster *cluster,
                                  uint16_t attribute_id);

void switch_cluster_report_action(zigbee_switch_cluster *cluster);

void switch_cluster_callback_attr_write_trampoline(uint8_t endpoint,
                                                   uint16_t attribute_id) {
    switch_cluster_on_write_attr(switch_cluster_by_endpoint[endpoint],
                                 attribute_id);
}

void switch_cluster_add_to_endpoint(zigbee_switch_cluster *cluster,
                                    hal_zigbee_endpoint *endpoint) {
    switch_cluster_by_endpoint[endpoint->endpoint] = cluster;
    cluster->endpoint = endpoint->endpoint;
    switch_cluster_load_attrs_from_nv(cluster);
    switch_cluster_load_dimming_from_nv(cluster);

    cluster->button->on_press =
        (ev_button_callback_t)switch_cluster_on_button_press;
    cluster->button->on_release =
        (ev_button_callback_t)switch_cluster_on_button_release;
    cluster->button->on_long_press =
        (ev_button_callback_t)switch_cluster_on_button_long_press;
    cluster->button->callback_param = cluster;

    cluster->dim_up_task.handler = switch_cluster_dim_up_task_handler;
    cluster->dim_up_task.arg     = cluster;
    hal_tasks_init(&cluster->dim_up_task);

    SETUP_ATTR(0, ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_TYPE, ZCL_DATA_TYPE_ENUM8,
               ATTR_READONLY, cluster->mode);
    SETUP_ATTR(1, ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_ACTIONS,
               ZCL_DATA_TYPE_ENUM8, ATTR_WRITABLE, cluster->action);
    SETUP_ATTR(2, ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_MODE, ZCL_DATA_TYPE_ENUM8,
               ATTR_WRITABLE, cluster->mode);
    SETUP_ATTR(3, ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_RELAY_MODE,
               ZCL_DATA_TYPE_ENUM8, ATTR_WRITABLE, cluster->relay_mode);
    SETUP_ATTR(4, ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_RELAY_INDEX,
               ZCL_DATA_TYPE_UINT8, ATTR_WRITABLE, cluster->relay_index);
    SETUP_ATTR(5, ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_LONG_PRESS_DUR,
               ZCL_DATA_TYPE_UINT16, ATTR_WRITABLE,
               cluster->button->long_press_duration_ms);
    SETUP_ATTR(6, ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_LEVEL_MOVE_RATE,
               ZCL_DATA_TYPE_UINT8, ATTR_WRITABLE, cluster->level_move_rate);
    SETUP_ATTR(7, ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_BINDING_MODE,
               ZCL_DATA_TYPE_ENUM8, ATTR_WRITABLE, cluster->binded_mode);
    SETUP_ATTR(8, ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_DIMMING_THRESHOLD,
               ZCL_DATA_TYPE_UINT8, ATTR_WRITABLE,
               cluster->dimming_threshold_pct);
    SETUP_ATTR(9, ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_DIMMING_RESET_WINDOW,
               ZCL_DATA_TYPE_UINT16, ATTR_WRITABLE,
               cluster->dimming_reset_window_ms);

    // Configuration
    endpoint->clusters[endpoint->cluster_count].cluster_id =
        ZCL_CLUSTER_ON_OFF_SWITCH_CONFIG;
    endpoint->clusters[endpoint->cluster_count].attribute_count = 10;
    endpoint->clusters[endpoint->cluster_count].attributes      = cluster->attr_infos;
    endpoint->clusters[endpoint->cluster_count].is_server       = 1;
    endpoint->cluster_count++;

    // Output ON OFF to bind to other devices
    endpoint->clusters[endpoint->cluster_count].cluster_id      = ZCL_CLUSTER_ON_OFF;
    endpoint->clusters[endpoint->cluster_count].attribute_count = 0;
    endpoint->clusters[endpoint->cluster_count].attributes      = NULL;
    endpoint->clusters[endpoint->cluster_count].is_server       = 0;
    endpoint->cluster_count++;

    SETUP_ATTR_FOR_TABLE(cluster->multistate_attr_infos, 0,
                         ZCL_ATTR_MULTISTATE_INPUT_NUMBER_OF_STATES,
                         ZCL_DATA_TYPE_UINT16, ATTR_READONLY,
                         multistate_num_of_states);
    SETUP_ATTR_FOR_TABLE(cluster->multistate_attr_infos, 1,
                         ZCL_ATTR_MULTISTATE_INPUT_OUT_OF_SERVICE,
                         ZCL_DATA_TYPE_BOOLEAN, ATTR_READONLY,
                         multistate_out_of_service);
    SETUP_ATTR_FOR_TABLE(cluster->multistate_attr_infos, 2,
                         ZCL_ATTR_MULTISTATE_INPUT_PRESENT_VALUE,
                         ZCL_DATA_TYPE_UINT16, ATTR_READONLY,
                         cluster->multistate_state);
    SETUP_ATTR_FOR_TABLE(cluster->multistate_attr_infos, 3,
                         ZCL_ATTR_MULTISTATE_INPUT_STATUS_FLAGS,
                         ZCL_DATA_TYPE_BITMAP8, ATTR_READONLY, multistate_flags);

    // Output
    endpoint->clusters[endpoint->cluster_count].cluster_id =
        ZCL_CLUSTER_MULTISTATE_INPUT_BASIC;
    endpoint->clusters[endpoint->cluster_count].attribute_count = 4;
    endpoint->clusters[endpoint->cluster_count].attributes      =
        cluster->multistate_attr_infos;
    endpoint->clusters[endpoint->cluster_count].is_server = 1;
    endpoint->cluster_count++;

    // Output Level for other devices
    endpoint->clusters[endpoint->cluster_count].cluster_id =
        ZCL_CLUSTER_LEVEL_CONTROL;
    endpoint->clusters[endpoint->cluster_count].attribute_count = 0;
    endpoint->clusters[endpoint->cluster_count].attributes      = NULL;
    endpoint->clusters[endpoint->cluster_count].is_server       = 0;
    endpoint->cluster_count++;
}

// Perform the relay action for ON position (position 1 in ZCL docs)
void switch_cluster_relay_action_on(zigbee_switch_cluster *cluster) {
    if (!switch_cluster_has_valid_relay(cluster))
        return;

    zigbee_relay_cluster *relay_cluster =
        &relay_clusters[cluster->relay_index - 1];

    switch (cluster->action) {
    case ZCL_ONOFF_CONFIGURATION_SWITCH_ACTION_ONOFF:
        relay_cluster_on(relay_cluster);
        break;
    case ZCL_ONOFF_CONFIGURATION_SWITCH_ACTION_OFFON:
        relay_cluster_off(relay_cluster);
        break;
    case ZCL_ONOFF_CONFIGURATION_SWITCH_ACTION_TOGGLE_SIMPLE:
        relay_cluster_toggle(relay_cluster);
        break;
    case ZCL_ONOFF_CONFIGURATION_SWITCH_ACTION_TOGGLE_SMART_SYNC:
        relay_cluster_toggle(relay_cluster);
        break;
    case ZCL_ONOFF_CONFIGURATION_SWITCH_ACTION_TOGGLE_SMART_OPPOSITE:
        relay_cluster_toggle(relay_cluster);
        break;
    }
}

// Perform the relay action for OFF position (position 2 in ZCL docs)
void switch_cluster_relay_action_off(zigbee_switch_cluster *cluster) {
    if (!switch_cluster_has_valid_relay(cluster))
        return;

    zigbee_relay_cluster *relay_cluster =
        &relay_clusters[cluster->relay_index - 1];

    switch (cluster->action) {
    case ZCL_ONOFF_CONFIGURATION_SWITCH_ACTION_ONOFF:
        relay_cluster_off(relay_cluster);
        break;
    case ZCL_ONOFF_CONFIGURATION_SWITCH_ACTION_OFFON:
        relay_cluster_on(relay_cluster);
        break;
    case ZCL_ONOFF_CONFIGURATION_SWITCH_ACTION_TOGGLE_SIMPLE:
        relay_cluster_toggle(relay_cluster);
        break;
    case ZCL_ONOFF_CONFIGURATION_SWITCH_ACTION_TOGGLE_SMART_SYNC:
        relay_cluster_toggle(relay_cluster);
        break;
    case ZCL_ONOFF_CONFIGURATION_SWITCH_ACTION_TOGGLE_SMART_OPPOSITE:
        relay_cluster_toggle(relay_cluster);
        break;
    }
}

// Send OnOff command to binded device based on ON position (position 1 in
// ZCL docs)
void switch_cluster_binding_action_on(zigbee_switch_cluster *cluster) {
    if (hal_zigbee_get_network_status() != HAL_ZIGBEE_NETWORK_JOINED) {
        return;
    }

    uint8_t cmd_id;

    switch (cluster->action) {
    case ZCL_ONOFF_CONFIGURATION_SWITCH_ACTION_ONOFF:
        cmd_id = ZCL_CMD_ONOFF_ON;
        break;

    case ZCL_ONOFF_CONFIGURATION_SWITCH_ACTION_OFFON:
        cmd_id = ZCL_CMD_ONOFF_OFF;
        break;

    case ZCL_ONOFF_CONFIGURATION_SWITCH_ACTION_TOGGLE_SIMPLE:
        cmd_id = ZCL_CMD_ONOFF_TOGGLE;
        break;

    case ZCL_ONOFF_CONFIGURATION_SWITCH_ACTION_TOGGLE_SMART_SYNC:
    case ZCL_ONOFF_CONFIGURATION_SWITCH_ACTION_TOGGLE_SMART_OPPOSITE:
        if (!switch_cluster_has_valid_relay(cluster)) {
            cmd_id = ZCL_CMD_ONOFF_TOGGLE;
        } else {
            zigbee_relay_cluster *relay_cluster =
                &relay_clusters[cluster->relay_index - 1];
            if (cluster->action ==
                ZCL_ONOFF_CONFIGURATION_SWITCH_ACTION_TOGGLE_SMART_SYNC)
                cmd_id = (relay_cluster->relay->on) ? ZCL_CMD_ONOFF_ON
                                                    : ZCL_CMD_ONOFF_OFF;
            else
                cmd_id = (relay_cluster->relay->on) ? ZCL_CMD_ONOFF_OFF
                                                    : ZCL_CMD_ONOFF_ON;
        }
        break;

    default:
        return;
    }

    hal_zigbee_cmd c = build_onoff_cmd(cluster->endpoint, cmd_id);
    hal_zigbee_send_cmd_to_bindings(&c);
}

// Send OnOff command to binded device based on OFF position (position 2 in
// ZCL docs)
void switch_cluster_binding_action_off(zigbee_switch_cluster *cluster) {
    if (hal_zigbee_get_network_status() != HAL_ZIGBEE_NETWORK_JOINED) {
        return;
    }

    uint8_t cmd_id;

    switch (cluster->action) {
    case ZCL_ONOFF_CONFIGURATION_SWITCH_ACTION_ONOFF:
        cmd_id = ZCL_CMD_ONOFF_OFF;
        break;

    case ZCL_ONOFF_CONFIGURATION_SWITCH_ACTION_OFFON:
        cmd_id = ZCL_CMD_ONOFF_ON;
        break;

    case ZCL_ONOFF_CONFIGURATION_SWITCH_ACTION_TOGGLE_SIMPLE:
        cmd_id = ZCL_CMD_ONOFF_TOGGLE;
        break;

    case ZCL_ONOFF_CONFIGURATION_SWITCH_ACTION_TOGGLE_SMART_SYNC:
    case ZCL_ONOFF_CONFIGURATION_SWITCH_ACTION_TOGGLE_SMART_OPPOSITE:
        if (!switch_cluster_has_valid_relay(cluster)) {
            cmd_id = ZCL_CMD_ONOFF_TOGGLE;
        } else {
            zigbee_relay_cluster *relay_cluster =
                &relay_clusters[cluster->relay_index - 1];
            if (cluster->action ==
                ZCL_ONOFF_CONFIGURATION_SWITCH_ACTION_TOGGLE_SMART_SYNC)
                cmd_id = (relay_cluster->relay->on) ? ZCL_CMD_ONOFF_ON
                                                    : ZCL_CMD_ONOFF_OFF;
            else
                cmd_id = (relay_cluster->relay->on) ? ZCL_CMD_ONOFF_OFF
                                                    : ZCL_CMD_ONOFF_ON;
        }
        break;

    default:
        return;
    }

    hal_zigbee_cmd c = build_onoff_cmd(cluster->endpoint, cmd_id);
    hal_zigbee_send_cmd_to_bindings(&c);
}

void switch_cluster_level_stop(zigbee_switch_cluster *cluster) {
    if (hal_zigbee_get_network_status() != HAL_ZIGBEE_NETWORK_JOINED) {
        return;
    }

    hal_zigbee_cmd c = build_level_stop_onoff_cmd(cluster->endpoint);
    hal_zigbee_send_cmd_to_bindings(&c);
}

// Ask bound lamp(s) for their current state. Answers arrive asynchronously in
// switch_cluster_on_read_rsp() and are only used for this same press.
static void switch_cluster_request_lamp_state(zigbee_switch_cluster *cluster) {
    cluster->lamp_state_valid = 0;

    if (hal_zigbee_get_network_status() != HAL_ZIGBEE_NETWORK_JOINED) {
        return;
    }

    hal_zigbee_cmd c = build_read_attr_cmd(cluster->endpoint, ZCL_CLUSTER_ON_OFF,
                                           ZCL_ATTR_ONOFF);
    hal_zigbee_send_cmd_to_bindings(&c);

    c = build_read_attr_cmd(cluster->endpoint, ZCL_CLUSTER_LEVEL_CONTROL,
                            ZCL_ATTR_LEVEL_CURRENT_LEVEL);
    hal_zigbee_send_cmd_to_bindings(&c);
}

void switch_cluster_on_read_rsp(uint8_t endpoint, uint16_t cluster_id,
                                uint16_t attribute_id, uint8_t status,
                                uint8_t data_type, const uint8_t *value) {
    if (endpoint >= sizeof(switch_cluster_by_endpoint) /
        sizeof(switch_cluster_by_endpoint[0])) {
        return;
    }
    zigbee_switch_cluster *cluster = switch_cluster_by_endpoint[endpoint];
    if (cluster == NULL) {
        return; // Not a switch endpoint (e.g. relay endpoint)
    }

    // Only store answers while a press is pending (button down, long press
    // not started yet). The direction is decided at long-press start and the
    // state is cleared on every new press, so later answers are never used.
    if (cluster->mode == ZCL_ONOFF_CONFIGURATION_SWITCH_TYPE_TOGGLE ||
        !cluster->button->pressed ||
        cluster->multistate_state != MULTISTATE_PRESS) {
        return;
    }
    if (status != ZCL_STATUS_SUCCESS || value == NULL) {
        return; // e.g. coordinator answering "unsupported attribute"
    }

    if (cluster_id == ZCL_CLUSTER_ON_OFF && attribute_id == ZCL_ATTR_ONOFF &&
        data_type == ZCL_DATA_TYPE_BOOLEAN) {
        // With several bound lamps, first valid answer wins
        if (!(cluster->lamp_state_valid & LAMP_STATE_ONOFF_VALID)) {
            cluster->lamp_on           = value[0] ? 1 : 0;
            cluster->lamp_state_valid |= LAMP_STATE_ONOFF_VALID;
        }
    } else if (cluster_id == ZCL_CLUSTER_LEVEL_CONTROL &&
               attribute_id == ZCL_ATTR_LEVEL_CURRENT_LEVEL &&
               data_type == ZCL_DATA_TYPE_UINT8) {
        if (!(cluster->lamp_state_valid & LAMP_STATE_LEVEL_VALID)) {
            cluster->lamp_level        = value[0];
            cluster->lamp_state_valid |= LAMP_STATE_LEVEL_VALID;
        }
    }
}

static void switch_cluster_send_level_move(zigbee_switch_cluster *cluster,
                                           uint8_t direction) {
    hal_zigbee_cmd c = build_level_move_onoff_cmd(cluster->endpoint, direction,
                                                  cluster->level_move_rate);

    hal_zigbee_send_cmd_to_bindings(&c);
    cluster->last_dim_direction = direction;
    // Keep legacy alternation consistent: next fallback goes the other way
    cluster->level_move_direction =
        direction == ZCL_LEVEL_MOVE_DOWN ? ZCL_LEVEL_MOVE_UP : ZCL_LEVEL_MOVE_DOWN;
}

static void switch_cluster_dim_up_task_handler(void *arg) {
    zigbee_switch_cluster *cluster = (zigbee_switch_cluster *)arg;

    // Released before the delay ran out: Stop is already sent, stay low
    if (!cluster->button->pressed ||
        cluster->multistate_state != MULTISTATE_LONG_PRESS) {
        return;
    }
    switch_cluster_send_level_move(cluster, ZCL_LEVEL_MOVE_UP);
}

void switch_cluster_level_control(zigbee_switch_cluster *cluster) {
    if (hal_zigbee_get_network_status() != HAL_ZIGBEE_NETWORK_JOINED) {
        return;
    }

    uint8_t valid = cluster->lamp_state_valid;

    if (!(valid & LAMP_STATE_ONOFF_VALID)) {
        // No answer from the lamp: legacy behaviour, alternate direction
        switch_cluster_send_level_move(cluster, cluster->level_move_direction);
        return;
    }

    if (!cluster->lamp_on) {
        // Lamp is off: switch it on just above minimum, then fade up. The
        // Move is delayed so it cannot overtake MoveToLevel on the way to
        // the lamp; if it is lost, the lamp stays on at the low level.
        hal_zigbee_cmd c = build_level_move_to_level_onoff_cmd(
            cluster->endpoint, LAMP_LEVEL_FROM_OFF);
        hal_zigbee_send_cmd_to_bindings(&c);
        cluster->last_dim_direction   = ZCL_LEVEL_MOVE_UP;
        cluster->level_move_direction = ZCL_LEVEL_MOVE_DOWN;
        hal_tasks_schedule(&cluster->dim_up_task, DIM_UP_DELAY_MS);
        return;
    }

    uint8_t direction;

    if (cluster->dim_regrab) {
        // Pressed again shortly after the last dim: keep going the same way
        direction = cluster->last_dim_direction;
    } else if (valid & LAMP_STATE_LEVEL_VALID) {
        // level <= threshold% -> up, otherwise down
        direction = ((uint16_t)cluster->lamp_level * 100 <=
                     (uint16_t)cluster->dimming_threshold_pct * LAMP_LEVEL_MAX)
                  ? ZCL_LEVEL_MOVE_UP
                  : ZCL_LEVEL_MOVE_DOWN;
    } else {
        // Lamp on, level unknown: dim down
        direction = ZCL_LEVEL_MOVE_DOWN;
    }

    // Never push against a limit we already sit at
    if (valid & LAMP_STATE_LEVEL_VALID) {
        if (direction == ZCL_LEVEL_MOVE_DOWN &&
            cluster->lamp_level <= LAMP_LEVEL_MIN) {
            direction = ZCL_LEVEL_MOVE_UP;
        } else if (direction == ZCL_LEVEL_MOVE_UP &&
                   cluster->lamp_level >= LAMP_LEVEL_MAX) {
            direction = ZCL_LEVEL_MOVE_DOWN;
        }
    }

    switch_cluster_send_level_move(cluster, direction);
}

void switch_cluster_on_button_press(zigbee_switch_cluster *cluster) {
    switch_cluster_flash_indicator(cluster);

    if (cluster->mode == ZCL_ONOFF_CONFIGURATION_SWITCH_TYPE_TOGGLE) {
        // Toggle does not support modes (RISE, SHORT, LONG)
        if (cluster->relay_mode != ZCL_ONOFF_CONFIGURATION_RELAY_MODE_DETACHED) {
            switch_cluster_relay_action_on(cluster);
        }
        switch_cluster_binding_action_on(cluster);
        cluster->multistate_state = MULTISTATE_POSITION_ON;
        hal_zigbee_notify_attribute_changed(
            cluster->endpoint, ZCL_CLUSTER_MULTISTATE_INPUT_BASIC,
            ZCL_ATTR_MULTISTATE_INPUT_PRESENT_VALUE);
        return;
    }

    if (cluster->relay_mode == ZCL_ONOFF_CONFIGURATION_RELAY_MODE_RISE) {
        switch_cluster_relay_action_on(cluster);
    }

    if (cluster->binded_mode == ZCL_ONOFF_CONFIGURATION_BINDED_MODE_RISE) {
        switch_cluster_binding_action_on(cluster);
    }

    cluster->multistate_state = MULTISTATE_PRESS;
    hal_zigbee_notify_attribute_changed(cluster->endpoint,
                                        ZCL_CLUSTER_MULTISTATE_INPUT_BASIC,
                                        ZCL_ATTR_MULTISTATE_INPUT_PRESENT_VALUE);

    // Re-grab is measured from the release of the last dim to this press,
    // so the 800 ms long-press delay does not eat into the window.
    cluster->dim_regrab = cluster->last_dim_valid &&
                          cluster->dimming_reset_window_ms > 0 &&
                          (hal_millis() - cluster->last_dim_end_ms) <=
                          cluster->dimming_reset_window_ms;

    // Must be after multistate_state = PRESS so answers are accepted
    switch_cluster_request_lamp_state(cluster);
}

void switch_cluster_on_button_release(zigbee_switch_cluster *cluster) {
    if (cluster->mode == ZCL_ONOFF_CONFIGURATION_SWITCH_TYPE_TOGGLE) {
        // Only flash on release for toggles,
        // for momentary flash on press only
        switch_cluster_flash_indicator(cluster);
    }

    if (cluster->mode == ZCL_ONOFF_CONFIGURATION_SWITCH_TYPE_TOGGLE) {
        // Toggle does not support modes (RISE, SHORT, LONG)
        if (cluster->relay_mode != ZCL_ONOFF_CONFIGURATION_RELAY_MODE_DETACHED) {
            switch_cluster_relay_action_off(cluster);
        }
        switch_cluster_binding_action_off(cluster);
        cluster->multistate_state = MULTISTATE_POSITION_OFF;
        hal_zigbee_notify_attribute_changed(
            cluster->endpoint, ZCL_CLUSTER_MULTISTATE_INPUT_BASIC,
            ZCL_ATTR_MULTISTATE_INPUT_PRESENT_VALUE);
        return;
    }

    // Release without a seen press: the button was already held at boot
    // (btn_init marks it pressed without calling on_press). Ignore it,
    // otherwise it is sent as a phantom short press.
    if (cluster->multistate_state == MULTISTATE_NOT_PRESSED) {
        return;
    }

    if (cluster->multistate_state != MULTISTATE_LONG_PRESS) {
        if (cluster->relay_mode == ZCL_ONOFF_CONFIGURATION_RELAY_MODE_SHORT) {
            switch_cluster_relay_action_on(cluster);
        }
        if (cluster->binded_mode == ZCL_ONOFF_CONFIGURATION_BINDED_MODE_SHORT) {
            switch_cluster_binding_action_on(cluster);
        }
    } else {
        // This is end of long press, send zcl_level stop
        hal_tasks_unschedule(&cluster->dim_up_task);
        switch_cluster_level_stop(cluster);
        cluster->last_dim_valid  = 1;
        cluster->last_dim_end_ms = hal_millis();
    }

    cluster->multistate_state = MULTISTATE_NOT_PRESSED;
    hal_zigbee_notify_attribute_changed(cluster->endpoint,
                                        ZCL_CLUSTER_MULTISTATE_INPUT_BASIC,
                                        ZCL_ATTR_MULTISTATE_INPUT_PRESENT_VALUE);
}

void switch_cluster_on_button_long_press(zigbee_switch_cluster *cluster) {
    if (cluster->mode == ZCL_ONOFF_CONFIGURATION_SWITCH_TYPE_TOGGLE) {
        // Toggle does not support modes (RISE, SHORT, LONG)
        return;
    }

    if (cluster->relay_mode == ZCL_ONOFF_CONFIGURATION_RELAY_MODE_LONG) {
        if (switch_cluster_has_valid_relay(cluster)) {
            relay_cluster_toggle(&relay_clusters[cluster->relay_index - 1]);
        }
    }

    if (cluster->binded_mode == ZCL_ONOFF_CONFIGURATION_BINDED_MODE_LONG) {
        switch_cluster_binding_action_on(cluster);
    }

    switch_cluster_level_control(cluster);

    cluster->multistate_state = MULTISTATE_LONG_PRESS;
    hal_zigbee_notify_attribute_changed(cluster->endpoint,
                                        ZCL_CLUSTER_MULTISTATE_INPUT_BASIC,
                                        ZCL_ATTR_MULTISTATE_INPUT_PRESENT_VALUE);
}

void synchronize_multistate_state(zigbee_switch_cluster *cluster) {
    if (cluster->mode == ZCL_ONOFF_CONFIGURATION_SWITCH_TYPE_TOGGLE) {
        if (cluster->button->pressed) {
            cluster->multistate_state = MULTISTATE_POSITION_ON;
        } else {
            cluster->multistate_state = MULTISTATE_POSITION_OFF;
        }
    } else {
        if (cluster->button->long_pressed) {
            cluster->multistate_state = MULTISTATE_LONG_PRESS;
        } else if (cluster->button->pressed) {
            cluster->multistate_state = MULTISTATE_PRESS;
        } else {
            cluster->multistate_state = MULTISTATE_NOT_PRESSED;
        }
    }
    hal_zigbee_notify_attribute_changed(cluster->endpoint,
                                        ZCL_CLUSTER_MULTISTATE_INPUT_BASIC,
                                        ZCL_ATTR_MULTISTATE_INPUT_PRESENT_VALUE);
}

void switch_cluster_on_write_attr(zigbee_switch_cluster *cluster,
                                  uint16_t attribute_id) {
    printf("Index at write attr: %d\r\n", cluster->switch_idx);
    if (attribute_id == ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_DIMMING_THRESHOLD) {
        if (cluster->dimming_threshold_pct > DIMMING_THRESHOLD_MAX_PCT) {
            cluster->dimming_threshold_pct = DIMMING_THRESHOLD_MAX_PCT;
        }
        // Stored in its own NV item; upstream switch config is untouched
        switch_cluster_store_dimming_to_nv(cluster);
        return;
    }
    if (attribute_id == ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_DIMMING_RESET_WINDOW) {
        if (cluster->dimming_reset_window_ms > DIMMING_RESET_WINDOW_MAX_MS) {
            cluster->dimming_reset_window_ms = DIMMING_RESET_WINDOW_MAX_MS;
        }
        switch_cluster_store_dimming_window_to_nv(cluster);
        return;
    }
    if (attribute_id == ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_RELAY_INDEX) {
        if (relay_clusters_cnt == 0) {
            cluster->relay_index = 0;
        } else if (cluster->relay_index < 1 || cluster->relay_index > relay_clusters_cnt) {
            cluster->relay_index = 1;
        }
    }
    if (attribute_id == ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_MODE) {
        synchronize_multistate_state(cluster);
        if (cluster->mode == ZCL_ONOFF_CONFIGURATION_SWITCH_TYPE_MOMENTARY_NC) {
            cluster->button->pressed_when_high = 1;
        } else {
            cluster->button->pressed_when_high = 0;
        }
    }
    switch_cluster_store_attrs_to_nv(cluster);
}

zigbee_switch_cluster_config nv_config_buffer;

void switch_cluster_store_attrs_to_nv(zigbee_switch_cluster *cluster) {
    nv_config_buffer.action      = cluster->action;
    nv_config_buffer.mode        = cluster->mode;
    nv_config_buffer.relay_index = cluster->relay_index;
    nv_config_buffer.relay_mode  = cluster->relay_mode;
    nv_config_buffer.button_long_press_duration =
        cluster->button->long_press_duration_ms;
    nv_config_buffer.level_move_rate = cluster->level_move_rate;
    nv_config_buffer.binded_mode     = cluster->binded_mode;
    hal_nvm_write(NV_ITEM_SWITCH_CLUSTER_DATA(cluster->switch_idx),
                  sizeof(zigbee_switch_cluster_config),
                  (uint8_t *)&nv_config_buffer);
}

void switch_cluster_load_attrs_from_nv(zigbee_switch_cluster *cluster) {
    hal_nvm_status_t st = hal_nvm_read(
        NV_ITEM_SWITCH_CLUSTER_DATA(cluster->switch_idx),
        sizeof(zigbee_switch_cluster_config), (uint8_t *)&nv_config_buffer);

    if (st != HAL_NVM_SUCCESS) {
        printf("No switch config in NV, using defaults\r\n");
        return;
    }
    // Each field is taken only if it is in range; otherwise the default set
    // by the config parser stays. The NV item itself is never rewritten here.
    if (nv_config_buffer.action <=
        ZCL_ONOFF_CONFIGURATION_SWITCH_ACTION_TOGGLE_SMART_OPPOSITE) {
        cluster->action = nv_config_buffer.action;
    } else {
        printf("Invalid action %d in NV, keeping default\r\n",
               nv_config_buffer.action);
    }
    if (nv_config_buffer.mode <=
        ZCL_ONOFF_CONFIGURATION_SWITCH_TYPE_MOMENTARY_NC) {
        cluster->mode = nv_config_buffer.mode;
    } else {
        printf("Invalid mode %d in NV, keeping default\r\n",
               nv_config_buffer.mode);
    }
    cluster->relay_index = nv_config_buffer.relay_index;
    if (nv_config_buffer.relay_mode <= ZCL_ONOFF_CONFIGURATION_RELAY_MODE_SHORT) {
        cluster->relay_mode = nv_config_buffer.relay_mode;
    } else {
        printf("Invalid relay_mode %d in NV, keeping default\r\n",
               nv_config_buffer.relay_mode);
    }
    if (nv_config_buffer.button_long_press_duration <=
        LONG_PRESS_MAX_MS) {
        cluster->button->long_press_duration_ms =
            nv_config_buffer.button_long_press_duration;
    } else {
        printf("Invalid long press %d in NV, keeping default\r\n",
               nv_config_buffer.button_long_press_duration);
    }
    if (nv_config_buffer.level_move_rate >= 1) {
        cluster->level_move_rate = nv_config_buffer.level_move_rate;
    } else {
        printf("Invalid level move rate 0 in NV, keeping default\r\n");
    }
    if (nv_config_buffer.binded_mode >= ZCL_ONOFF_CONFIGURATION_BINDED_MODE_RISE &&
        nv_config_buffer.binded_mode <= ZCL_ONOFF_CONFIGURATION_BINDED_MODE_SHORT) {
        cluster->binded_mode = nv_config_buffer.binded_mode;
    } else {
        printf("Invalid binded_mode %d in NV, keeping default\r\n",
               nv_config_buffer.binded_mode);
    }

    // Normally-closed buttons are inverted. The polarity was set only when the
    // mode was written from Z2M, so it was lost after a restart. btn_init()
    // has already run with the old polarity, so its state is re-derived
    // the same way btn_init() does it (a held button is not a press).
    if (cluster->mode == ZCL_ONOFF_CONFIGURATION_SWITCH_TYPE_MOMENTARY_NC &&
        !cluster->button->pressed_when_high) {
        cluster->button->pressed_when_high = 1;
        uint8_t held = cluster->button->debounce_last_state == 1;
        cluster->button->pressed      = held;
        cluster->button->long_pressed = held;
    }

    // Validate relay_index to prevent out-of-bounds access
    if (relay_clusters_cnt == 0) {
        cluster->relay_index = 0;
    } else if (cluster->relay_index < 1 || cluster->relay_index > relay_clusters_cnt) {
        printf("Invalid relay_index %d in NV, resetting to default\r\n",
               cluster->relay_index);
        cluster->relay_index = cluster->switch_idx + 1;
    }
}

// Smart dimming settings live in a separate NV item so the upstream
// switch config item keeps its exact size and is never invalidated.
static zigbee_switch_dimming_config dimming_nv_buffer;

static void switch_cluster_store_dimming_to_nv(zigbee_switch_cluster *cluster) {
    dimming_nv_buffer.threshold_pct = cluster->dimming_threshold_pct;
    hal_nvm_write(NV_ITEM_SWITCH_DIMMING_CONFIG(cluster->switch_idx),
                  sizeof(zigbee_switch_dimming_config),
                  (uint8_t *)&dimming_nv_buffer);
}

static void switch_cluster_load_dimming_from_nv(zigbee_switch_cluster *cluster) {
    switch_cluster_load_dimming_window_from_nv(cluster);
    cluster->dimming_threshold_pct = DIMMING_THRESHOLD_DEFAULT_PCT;

    hal_nvm_status_t st = hal_nvm_read(
        NV_ITEM_SWITCH_DIMMING_CONFIG(cluster->switch_idx),
        sizeof(zigbee_switch_dimming_config), (uint8_t *)&dimming_nv_buffer);

    if (st != HAL_NVM_SUCCESS) {
        return; // Not stored yet (first boot on this firmware): default
    }
    if (dimming_nv_buffer.threshold_pct > DIMMING_THRESHOLD_MAX_PCT) {
        printf("Invalid dimming threshold %d in NV, using default\r\n",
               dimming_nv_buffer.threshold_pct);
        return;
    }
    cluster->dimming_threshold_pct = dimming_nv_buffer.threshold_pct;
}

static zigbee_switch_dimming_window_config dimming_window_nv_buffer;

static void switch_cluster_store_dimming_window_to_nv(
    zigbee_switch_cluster *cluster) {
    dimming_window_nv_buffer.reset_window_ms = cluster->dimming_reset_window_ms;
    hal_nvm_write(NV_ITEM_SWITCH_DIMMING_WINDOW(cluster->switch_idx),
                  sizeof(zigbee_switch_dimming_window_config),
                  (uint8_t *)&dimming_window_nv_buffer);
}

static void switch_cluster_load_dimming_window_from_nv(
    zigbee_switch_cluster *cluster) {
    cluster->dimming_reset_window_ms = DIMMING_RESET_WINDOW_DEFAULT_MS;

    hal_nvm_status_t st = hal_nvm_read(
        NV_ITEM_SWITCH_DIMMING_WINDOW(cluster->switch_idx),
        sizeof(zigbee_switch_dimming_window_config),
        (uint8_t *)&dimming_window_nv_buffer);

    if (st != HAL_NVM_SUCCESS) {
        return; // Not stored yet: default
    }
    if (dimming_window_nv_buffer.reset_window_ms > DIMMING_RESET_WINDOW_MAX_MS) {
        printf("Invalid dimming reset window %d in NV, using default\r\n",
               dimming_window_nv_buffer.reset_window_ms);
        return;
    }
    cluster->dimming_reset_window_ms = dimming_window_nv_buffer.reset_window_ms;
}
