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

#define MULTISTATE_NOT_PRESSED        0
#define MULTISTATE_PRESS              1
#define MULTISTATE_LONG_PRESS         2
#define MULTISTATE_POSITION_ON        3
#define MULTISTATE_POSITION_OFF       4
// Fork: confirmed multi-press gestures (Layer 1 reports)
#define MULTISTATE_SINGLE_CLICK       5
#define MULTISTATE_DOUBLE_CLICK       6
#define MULTISTATE_TRIPLE_CLICK       7
#define MULTISTATE_QUADRUPLE_CLICK    8

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
// switch_lamp_state_t.flags
#define LAMP_ONOFF_VALID                   0x01
#define LAMP_IS_ON                         0x02
#define LAMP_LEVEL_VALID                   0x04
// Lamp answers are collected this long after the press edge
#define LAMP_ANSWER_WINDOW_MS              200
// Lamps count as on when at least this % of the answering lamps are on
#define LAMP_ON_QUORUM_PCT                 60

// Multi-press gestures (fork). Active only for a momentary switch whose
// relay is detached and whose binded mode is "short press".
#define MULTI_PRESS_WINDOW_DEFAULT_MS     250
#define MULTI_PRESS_WINDOW_MIN_MS         100
#define MULTI_PRESS_WINDOW_MAX_MS         400
#define VERY_LONG_HOLD_MS                 15000
#define RESET_CONFIRM_WINDOW_MS           3000
#define SCENE_COUNT_MAX                   10
#define SCENE_SET2_FIRST_ID               10
#define SCENE_LOCKOUT_DEFAULT_S           10
#define SCENE_LOCKOUT_MIN_S               5
#define SCENE_LOCKOUT_MAX_S               15
// Scene mode LED: N short blinks, pause, repeated
#define SCENE_LED_PERIOD_MS               1500
#define SCENE_LED_ON_MS                   120
#define SCENE_LED_OFF_MS                  180
// "Scene set off" LED: SOS once (on times; 150 ms off between marks)
#define SOS_SHORT_MS                      150
#define SOS_LONG_MS                       450
#define SOS_GAP_MS                        150
#define SOS_MARKS                         9
// Light on Quadruple
#define QUADRUPLE_LEVEL_DEFAULT_PCT       100
#define QUADRUPLE_COLOR_TEMP_DEFAULT_K    2700
#define QUADRUPLE_COLOR_TEMP_MIN_K        2000
#define QUADRUPLE_COLOR_TEMP_MAX_K        7000
// Colour goes after the level: a lamp that is off may ignore it
#define QUADRUPLE_COLOR_DELAY_MS          250
// Reset armed LED: fast blinking
#define RESET_LED_ON_MS                   80
#define RESET_LED_OFF_MS                  80

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

// Multi-press gestures (fork)
static bool switch_cluster_gestures_active(const zigbee_switch_cluster *cluster);
static void switch_cluster_load_gestures_from_nv(zigbee_switch_cluster *cluster);
static void switch_cluster_gesture_task_handler(void *arg);
static void switch_cluster_very_long_task_handler(void *arg);
static void switch_cluster_mode_task_handler(void *arg);
static void switch_cluster_led_task_handler(void *arg);
static void switch_cluster_quadruple_color_task_handler(void *arg);
static void switch_cluster_play_sos(zigbee_switch_cluster *cluster);
static void switch_cluster_light_on_quadruple(zigbee_switch_cluster *cluster);
static void switch_cluster_multi_press_trampoline(void *arg, uint8_t count);
static void switch_cluster_leave_op_mode(zigbee_switch_cluster *cluster);
static void switch_cluster_set_onoff_flag(zigbee_switch_cluster *cluster,
                                          uint8_t value);
static void switch_cluster_update_flag_after_dim(zigbee_switch_cluster *cluster);
static void switch_cluster_report_gesture(zigbee_switch_cluster *cluster,
                                          uint16_t value);
static bool switch_cluster_on_write_gesture_attr(zigbee_switch_cluster *cluster,
                                                 uint16_t attribute_id);

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
    switch_cluster_load_gestures_from_nv(cluster);

    cluster->button->on_press =
        (ev_button_callback_t)switch_cluster_on_button_press;
    cluster->button->on_release =
        (ev_button_callback_t)switch_cluster_on_button_release;
    cluster->button->on_long_press =
        (ev_button_callback_t)switch_cluster_on_button_long_press;
    cluster->button->callback_param = cluster;
    // The "N quick presses = factory reset" counter would fire while cycling
    // scenes, so it is replaced by the 15 s hold when gestures are active.
    cluster->orig_on_multi_press    = cluster->button->on_multi_press;
    cluster->button->on_multi_press = switch_cluster_multi_press_trampoline;

    cluster->dim_up_task.handler = switch_cluster_dim_up_task_handler;
    cluster->dim_up_task.arg     = cluster;
    hal_tasks_init(&cluster->dim_up_task);
    cluster->gesture_task.handler = switch_cluster_gesture_task_handler;
    cluster->gesture_task.arg     = cluster;
    hal_tasks_init(&cluster->gesture_task);
    cluster->very_long_task.handler = switch_cluster_very_long_task_handler;
    cluster->very_long_task.arg     = cluster;
    hal_tasks_init(&cluster->very_long_task);
    cluster->mode_task.handler = switch_cluster_mode_task_handler;
    cluster->mode_task.arg     = cluster;
    hal_tasks_init(&cluster->mode_task);
    cluster->led_task.handler = switch_cluster_led_task_handler;
    cluster->led_task.arg     = cluster;
    hal_tasks_init(&cluster->led_task);
    cluster->quadruple_color_task.handler =
        switch_cluster_quadruple_color_task_handler;
    cluster->quadruple_color_task.arg = cluster;
    hal_tasks_init(&cluster->quadruple_color_task);
    cluster->op_mode = SWITCH_OP_MODE_NORMAL;

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
    SETUP_ATTR(10, ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_DIMMING_SYNC,
               ZCL_DATA_TYPE_BOOLEAN, ATTR_WRITABLE, cluster->dimming_sync);
    SETUP_ATTR(11, ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_MULTI_PRESS_WINDOW,
               ZCL_DATA_TYPE_UINT16, ATTR_WRITABLE,
               cluster->multi_press_window_ms);
    SETUP_ATTR(12, ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_SCENE_COUNT_SET1,
               ZCL_DATA_TYPE_UINT8, ATTR_WRITABLE, cluster->scene_count[0]);
    SETUP_ATTR(13, ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_SCENE_GROUP_ID_SET1,
               ZCL_DATA_TYPE_UINT16, ATTR_WRITABLE, cluster->scene_group_id[0]);
    SETUP_ATTR(14, ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_SCENE_LOCKOUT_SET1,
               ZCL_DATA_TYPE_UINT8, ATTR_WRITABLE, cluster->scene_lockout_s[0]);
    SETUP_ATTR(15, ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_SCENE_COUNT_SET2,
               ZCL_DATA_TYPE_UINT8, ATTR_WRITABLE, cluster->scene_count[1]);
    SETUP_ATTR(16, ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_SCENE_GROUP_ID_SET2,
               ZCL_DATA_TYPE_UINT16, ATTR_WRITABLE, cluster->scene_group_id[1]);
    SETUP_ATTR(17, ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_SCENE_LOCKOUT_SET2,
               ZCL_DATA_TYPE_UINT8, ATTR_WRITABLE, cluster->scene_lockout_s[1]);
    SETUP_ATTR(18, ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_QUADRUPLE_LEVEL,
               ZCL_DATA_TYPE_UINT8, ATTR_WRITABLE, cluster->quadruple_level_pct);
    SETUP_ATTR(19, ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_QUADRUPLE_COLOR_TEMP,
               ZCL_DATA_TYPE_UINT16, ATTR_WRITABLE,
               cluster->quadruple_color_temp_k);

    // Configuration
    endpoint->clusters[endpoint->cluster_count].cluster_id =
        ZCL_CLUSTER_ON_OFF_SWITCH_CONFIG;
    endpoint->clusters[endpoint->cluster_count].attribute_count = 20;
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

    // Output Scenes (fork): RecallScene to bound lamps or groups
    endpoint->clusters[endpoint->cluster_count].cluster_id      = ZCL_CLUSTER_SCENES;
    endpoint->clusters[endpoint->cluster_count].attribute_count = 0;
    endpoint->clusters[endpoint->cluster_count].attributes      = NULL;
    endpoint->clusters[endpoint->cluster_count].is_server       = 0;
    endpoint->cluster_count++;

    // Output Color Control (fork): colour temperature for Light on Quadruple
    endpoint->clusters[endpoint->cluster_count].cluster_id =
        ZCL_CLUSTER_COLOR_CONTROL;
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
    cluster->lamps_cnt   = 0;
    cluster->lamp_req_ms = hal_millis();

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

// Entry of the lamp with this address; a new one if it is not known yet.
// NULL when the table is full (extra lamps are not counted).
static switch_lamp_state_t *switch_cluster_lamp_slot(
    zigbee_switch_cluster *cluster, uint16_t addr) {
    for (uint8_t i = 0; i < cluster->lamps_cnt; i++) {
        if (cluster->lamps[i].addr == addr) {
            return &cluster->lamps[i];
        }
    }
    if (cluster->lamps_cnt >= SWITCH_MAX_LAMPS) {
        return NULL;
    }
    switch_lamp_state_t *slot = &cluster->lamps[cluster->lamps_cnt++];
    slot->addr  = addr;
    slot->level = 0;
    slot->flags = 0;
    return slot;
}

void switch_cluster_on_read_rsp(uint16_t src_addr, uint8_t endpoint,
                                uint16_t cluster_id, uint16_t attribute_id,
                                uint8_t status, uint8_t data_type,
                                const uint8_t *value) {
    if (endpoint >= sizeof(switch_cluster_by_endpoint) /
        sizeof(switch_cluster_by_endpoint[0])) {
        return;
    }
    zigbee_switch_cluster *cluster = switch_cluster_by_endpoint[endpoint];
    if (cluster == NULL) {
        return; // Not a switch endpoint (e.g. relay endpoint)
    }

    // Only store answers while a press is pending (button down, long press
    // not started yet) and within the answer window after the press edge.
    // The state is cleared on every new press, so later answers are never
    // used.
    if (cluster->mode == ZCL_ONOFF_CONFIGURATION_SWITCH_TYPE_TOGGLE ||
        !cluster->button->pressed || !cluster->lamp_answers_open ||
        (hal_millis() - cluster->lamp_req_ms) > LAMP_ANSWER_WINDOW_MS) {
        return;
    }
    if (status != ZCL_STATUS_SUCCESS || value == NULL) {
        return; // e.g. coordinator answering "unsupported attribute"
    }

    if (cluster_id == ZCL_CLUSTER_ON_OFF && attribute_id == ZCL_ATTR_ONOFF &&
        data_type == ZCL_DATA_TYPE_BOOLEAN) {
        switch_lamp_state_t *lamp = switch_cluster_lamp_slot(cluster, src_addr);
        // A lamp answering twice: its first valid answer wins
        if (lamp != NULL && !(lamp->flags & LAMP_ONOFF_VALID)) {
            lamp->flags |= LAMP_ONOFF_VALID | (value[0] ? LAMP_IS_ON : 0);
        }
    } else if (cluster_id == ZCL_CLUSTER_LEVEL_CONTROL &&
               attribute_id == ZCL_ATTR_LEVEL_CURRENT_LEVEL &&
               data_type == ZCL_DATA_TYPE_UINT8) {
        switch_lamp_state_t *lamp = switch_cluster_lamp_slot(cluster, src_addr);
        if (lamp != NULL && !(lamp->flags & LAMP_LEVEL_VALID)) {
            lamp->level  = value[0];
            lamp->flags |= LAMP_LEVEL_VALID;
        }
    }
}

typedef struct {
    uint8_t answered;    // lamps that sent their OnOff
    uint8_t group_on;    // at least LAMP_ON_QUORUM_PCT of them are on
    uint8_t level_valid; // at least one lit lamp sent its level
    uint8_t level;       // average level of the lit lamps
} lamp_summary_t;

static void switch_cluster_lamp_summary(const zigbee_switch_cluster *cluster,
                                        lamp_summary_t *s) {
    uint16_t level_sum = 0;
    uint8_t  level_cnt = 0;
    uint8_t  on_cnt    = 0;

    s->answered = 0;
    for (uint8_t i = 0; i < cluster->lamps_cnt; i++) {
        const switch_lamp_state_t *lamp = &cluster->lamps[i];
        if (!(lamp->flags & LAMP_ONOFF_VALID)) {
            continue;
        }
        s->answered++;
        if (lamp->flags & LAMP_IS_ON) {
            on_cnt++;
            if (lamp->flags & LAMP_LEVEL_VALID) {
                level_sum += lamp->level;
                level_cnt++;
            }
        }
    }
    s->group_on = s->answered > 0 &&
                  (uint16_t)on_cnt * 100 >=
                  (uint16_t)LAMP_ON_QUORUM_PCT * s->answered;
    s->level_valid = level_cnt > 0;
    s->level       = level_cnt > 0
                   ? (uint8_t)((level_sum + level_cnt / 2) / level_cnt)
                   : 0;
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
    cluster->dim_active   = 1;
    cluster->dim_dir      = direction;
    cluster->dim_start_ms = hal_millis();
}

static void switch_cluster_dim_up_task_handler(void *arg) {
    zigbee_switch_cluster *cluster = (zigbee_switch_cluster *)arg;

    // Released before the delay ran out: Stop is already sent, stay there
    if (!cluster->button->pressed ||
        cluster->multistate_state != MULTISTATE_LONG_PRESS) {
        return;
    }
    switch_cluster_send_level_move(cluster, cluster->delayed_move_dir);
}

// Send MoveToLevelWithOnOff(level) now and the Move in `direction` a bit
// later, so the Move cannot overtake it on the way to the lamp.
static void switch_cluster_move_to_level_then_move(
    zigbee_switch_cluster *cluster, uint8_t level, uint8_t direction) {
    hal_zigbee_cmd c = build_level_move_to_level_onoff_cmd(cluster->endpoint,
                                                           level);

    hal_zigbee_send_cmd_to_bindings(&c);
    cluster->last_dim_direction   = direction;
    cluster->level_move_direction =
        direction == ZCL_LEVEL_MOVE_DOWN ? ZCL_LEVEL_MOVE_UP : ZCL_LEVEL_MOVE_DOWN;
    cluster->delayed_move_dir = direction;
    hal_tasks_schedule(&cluster->dim_up_task, DIM_UP_DELAY_MS);
}

void switch_cluster_level_control(zigbee_switch_cluster *cluster) {
    cluster->dim_active     = 0;
    cluster->dim_sync_level = 0;

    if (hal_zigbee_get_network_status() != HAL_ZIGBEE_NETWORK_JOINED) {
        return;
    }

    lamp_summary_t s;

    switch_cluster_lamp_summary(cluster, &s);

    if (s.answered == 0) {
        // No answer from any lamp: legacy behaviour, alternate direction
        switch_cluster_send_level_move(cluster, cluster->level_move_direction);
        return;
    }

    // Lamps are (or are about to be) on: this also corrects a flag that went
    // stale because the lamps were switched from elsewhere
    if (switch_cluster_gestures_active(cluster)) {
        switch_cluster_set_onoff_flag(cluster, 1);
    }

    if (!s.group_on) {
        // Lamps are off: switch them on just above minimum, then fade up. If
        // the Move is lost, the lamps stay on at the low level.
        switch_cluster_move_to_level_then_move(cluster, LAMP_LEVEL_FROM_OFF,
                                               ZCL_LEVEL_MOVE_UP);
        return;
    }

    uint8_t direction;

    if (cluster->dim_regrab) {
        // Pressed again shortly after the last dim: keep going the same way
        direction = cluster->last_dim_direction;
    } else if (s.level_valid) {
        // level <= threshold% -> up, otherwise down
        direction = ((uint16_t)s.level * 100 <=
                     (uint16_t)cluster->dimming_threshold_pct * LAMP_LEVEL_MAX)
                  ? ZCL_LEVEL_MOVE_UP
                  : ZCL_LEVEL_MOVE_DOWN;
    } else {
        // Lamps on, level unknown: dim down
        direction = ZCL_LEVEL_MOVE_DOWN;
    }

    // Never push against a limit we already sit at
    if (s.level_valid) {
        if (direction == ZCL_LEVEL_MOVE_DOWN && s.level <= LAMP_LEVEL_MIN) {
            direction = ZCL_LEVEL_MOVE_UP;
        } else if (direction == ZCL_LEVEL_MOVE_UP &&
                   s.level >= LAMP_LEVEL_MAX) {
            direction = ZCL_LEVEL_MOVE_DOWN;
        }
    }

    if (cluster->dimming_sync && s.level_valid) {
        // All lamps jump to the average level first, then dim together
        uint8_t level = s.level < LAMP_LEVEL_FROM_OFF ? LAMP_LEVEL_FROM_OFF
                                                      : s.level;
        cluster->dim_sync_level = level;
        switch_cluster_move_to_level_then_move(cluster, level, direction);
        return;
    }

    switch_cluster_send_level_move(cluster, direction);
}

// End of a dimming hold: Stop, remember it for the re-grab window and update
// the on/off flag.
static void switch_cluster_end_dimming(zigbee_switch_cluster *cluster) {
    hal_tasks_unschedule(&cluster->dim_up_task);
    switch_cluster_level_stop(cluster);
    cluster->dim_stop_ms     = hal_millis();
    cluster->last_dim_valid  = 1;
    cluster->last_dim_end_ms = cluster->dim_stop_ms;
    if (switch_cluster_gestures_active(cluster)) {
        switch_cluster_update_flag_after_dim(cluster);
    }
    cluster->dim_active = 0;
}

// Estimate whether the lamps are still on after a dim (no Read): a lamp that
// was dimmed down for at least its starting level reached the minimum and
// switched off. Flag = 0 if fewer than LAMP_ON_QUORUM_PCT stay on.
static void switch_cluster_update_flag_after_dim(zigbee_switch_cluster *cluster) {
    if (!cluster->dim_active) {
        return; // No Move was sent: the flag was set when dimming started
    }
    if (cluster->dim_dir == ZCL_LEVEL_MOVE_UP) {
        switch_cluster_set_onoff_flag(cluster, 1);
        return;
    }

    uint32_t moved = (cluster->dim_stop_ms - cluster->dim_start_ms) *
                     cluster->level_move_rate / 1000;

    if (cluster->dim_sync_level != 0) {
        switch_cluster_set_onoff_flag(cluster,
                                      moved >= cluster->dim_sync_level ? 0 : 1);
        return;
    }

    uint8_t answered = 0;
    uint8_t still_on = 0;

    for (uint8_t i = 0; i < cluster->lamps_cnt; i++) {
        const switch_lamp_state_t *lamp = &cluster->lamps[i];
        if (!(lamp->flags & LAMP_ONOFF_VALID)) {
            continue;
        }
        answered++;
        if ((lamp->flags & LAMP_IS_ON) &&
            (!(lamp->flags & LAMP_LEVEL_VALID) || moved < lamp->level)) {
            still_on++;
        }
    }
    if (answered == 0) {
        return; // Legacy dim without answers: nothing to estimate from
    }
    switch_cluster_set_onoff_flag(
        cluster, (uint16_t)still_on * 100 >=
        (uint16_t)LAMP_ON_QUORUM_PCT * answered);
}

// ---------------------------------------------------------------------------
// Multi-press gestures (fork)
//
// Layer 1 turns presses into gestures: Short click, Long click, Double click,
// Triple click and Very long hold (15 s). Layer 2 turns gestures into
// actions depending on the operating mode. Only for a momentary switch whose
// relay is detached and whose binded mode is "short press"; every other
// configuration keeps the original behaviour.
// ---------------------------------------------------------------------------

static bool switch_cluster_gestures_active(const zigbee_switch_cluster *cluster) {
    return cluster->mode != ZCL_ONOFF_CONFIGURATION_SWITCH_TYPE_TOGGLE &&
           cluster->relay_mode == ZCL_ONOFF_CONFIGURATION_RELAY_MODE_DETACHED &&
           cluster->binded_mode == ZCL_ONOFF_CONFIGURATION_BINDED_MODE_SHORT;
}

static void switch_cluster_multi_press_trampoline(void *arg, uint8_t count) {
    zigbee_switch_cluster *cluster = (zigbee_switch_cluster *)arg;

    if (switch_cluster_gestures_active(cluster)) {
        return; // Factory reset is the 15 s hold instead
    }
    if (cluster->orig_on_multi_press != NULL) {
        cluster->orig_on_multi_press(arg, count);
    }
}

// Send the current multistate value as an explicit attribute report. The
// periodic reporting only sees a value that is still set when it runs, so a
// short event (a gesture) must be sent explicitly.
static void switch_cluster_send_multistate_report(zigbee_switch_cluster *cluster) {
    hal_zigbee_send_report_attr(cluster->endpoint,
                                ZCL_CLUSTER_MULTISTATE_INPUT_BASIC,
                                ZCL_ATTR_MULTISTATE_INPUT_PRESENT_VALUE,
                                ZCL_DATA_TYPE_UINT16, &cluster->multistate_state,
                                sizeof(cluster->multistate_state));
}

// Report a confirmed gesture to Z2M, then "released", so the same gesture
// twice in a row is two changes for Home Assistant.
static void switch_cluster_report_gesture(zigbee_switch_cluster *cluster,
                                          uint16_t value) {
    cluster->multistate_state = value;
    switch_cluster_send_multistate_report(cluster);
    cluster->multistate_state = MULTISTATE_NOT_PRESSED;
    switch_cluster_send_multistate_report(cluster);
}

static void switch_cluster_store_u8(uint8_t item_id, uint8_t value) {
    static uint8_t buf;

    buf = value;
    hal_nvm_write(item_id, sizeof(buf), &buf);
}

static void switch_cluster_store_u16(uint8_t item_id, uint16_t value) {
    static uint16_t buf;

    buf = value;
    hal_nvm_write(item_id, sizeof(buf), (uint8_t *)&buf);
}

// Stored value if it is present and in range, otherwise the default
static uint8_t switch_cluster_load_u8(uint8_t item_id, uint8_t def,
                                      uint8_t min, uint8_t max) {
    uint8_t value;

    if (hal_nvm_read(item_id, sizeof(value), &value) != HAL_NVM_SUCCESS) {
        return def;
    }
    if (value < min || value > max) {
        printf("Invalid value %d in NV item %02x, using default\r\n", value,
               item_id);
        return def;
    }
    return value;
}

static uint16_t switch_cluster_load_u16(uint8_t item_id, uint16_t def,
                                        uint16_t min, uint16_t max) {
    uint16_t value;

    if (hal_nvm_read(item_id, sizeof(value), (uint8_t *)&value) !=
        HAL_NVM_SUCCESS) {
        return def;
    }
    if (value < min || value > max) {
        printf("Invalid value %d in NV item %02x, using default\r\n", value,
               item_id);
        return def;
    }
    return value;
}

static void switch_cluster_load_gestures_from_nv(zigbee_switch_cluster *cluster) {
    uint8_t idx = cluster->switch_idx;

    cluster->dimming_sync =
        switch_cluster_load_u8(NV_ITEM_SWITCH_DIMMING_SYNC(idx), 0, 0, 1);
    cluster->multi_press_window_ms = switch_cluster_load_u16(
        NV_ITEM_SWITCH_MULTI_PRESS_WINDOW(idx), MULTI_PRESS_WINDOW_DEFAULT_MS,
        MULTI_PRESS_WINDOW_MIN_MS, MULTI_PRESS_WINDOW_MAX_MS);
    cluster->onoff_flag =
        switch_cluster_load_u8(NV_ITEM_SWITCH_ONOFF_FLAG(idx), 0, 0, 1);

    cluster->scene_count[0] = switch_cluster_load_u8(
        NV_ITEM_SWITCH_SCENE_COUNT_SET1(idx), 0, 0, SCENE_COUNT_MAX);
    cluster->scene_group_id[0] = switch_cluster_load_u16(
        NV_ITEM_SWITCH_SCENE_GROUP_ID_SET1(idx), 0, 0, 0xFFFF);
    cluster->scene_lockout_s[0] = switch_cluster_load_u8(
        NV_ITEM_SWITCH_SCENE_LOCKOUT_SET1(idx), SCENE_LOCKOUT_DEFAULT_S,
        SCENE_LOCKOUT_MIN_S, SCENE_LOCKOUT_MAX_S);
    cluster->last_scene[0] = switch_cluster_load_u8(
        NV_ITEM_SWITCH_LAST_SCENE_SET1(idx), 0, 0, SCENE_COUNT_MAX - 1);

    cluster->scene_count[1] = switch_cluster_load_u8(
        NV_ITEM_SWITCH_SCENE_COUNT_SET2(idx), 0, 0, SCENE_COUNT_MAX);
    cluster->scene_group_id[1] = switch_cluster_load_u16(
        NV_ITEM_SWITCH_SCENE_GROUP_ID_SET2(idx), 0, 0, 0xFFFF);
    cluster->scene_lockout_s[1] = switch_cluster_load_u8(
        NV_ITEM_SWITCH_SCENE_LOCKOUT_SET2(idx), SCENE_LOCKOUT_DEFAULT_S,
        SCENE_LOCKOUT_MIN_S, SCENE_LOCKOUT_MAX_S);
    cluster->last_scene[1] = switch_cluster_load_u8(
        NV_ITEM_SWITCH_LAST_SCENE_SET2(idx), SCENE_SET2_FIRST_ID,
        SCENE_SET2_FIRST_ID, SCENE_SET2_FIRST_ID + SCENE_COUNT_MAX - 1);

    cluster->current_scene[0] = cluster->last_scene[0];
    cluster->current_scene[1] = cluster->last_scene[1];

    cluster->quadruple_level_pct = switch_cluster_load_u8(
        NV_ITEM_SWITCH_QUADRUPLE_LEVEL(idx), QUADRUPLE_LEVEL_DEFAULT_PCT, 0, 100);
    cluster->quadruple_color_temp_k = switch_cluster_load_u16(
        NV_ITEM_SWITCH_QUADRUPLE_COLOR_TEMP(idx), QUADRUPLE_COLOR_TEMP_DEFAULT_K,
        QUADRUPLE_COLOR_TEMP_MIN_K, QUADRUPLE_COLOR_TEMP_MAX_K);
}

// The on/off flag is written to NV only when it changes
static void switch_cluster_set_onoff_flag(zigbee_switch_cluster *cluster,
                                          uint8_t value) {
    value = value ? 1 : 0;
    if (cluster->onoff_flag == value) {
        return;
    }
    cluster->onoff_flag = value;
    switch_cluster_store_u8(NV_ITEM_SWITCH_ONOFF_FLAG(cluster->switch_idx),
                            value);
}

// Indicator LED back to what it shows outside the special modes
static void switch_cluster_restore_indicator(zigbee_switch_cluster *cluster) {
    if (cluster->indicator_led == NULL) {
        return;
    }
    if (hal_zigbee_get_network_status() == HAL_ZIGBEE_NETWORK_JOINED) {
        led_off(cluster->indicator_led);
        sync_switch_indicator_led(cluster);
        // The LED may also show a relay (e.g. L-only: one LED for both)
        update_relay_clusters();
    } else {
        // Same pattern as the "not connected" network indicator
        led_blink(cluster->indicator_led, 500, 500, LED_BLINK_FOREVER);
    }
}

static uint8_t switch_cluster_scene_first_id(uint8_t set) {
    return set == 0 ? 0 : SCENE_SET2_FIRST_ID;
}

static void switch_cluster_recall_scene(zigbee_switch_cluster *cluster,
                                        uint8_t set) {
    if (hal_zigbee_get_network_status() != HAL_ZIGBEE_NETWORK_JOINED) {
        return;
    }
    hal_zigbee_cmd c = build_recall_scene_cmd(cluster->endpoint,
                                              cluster->scene_group_id[set],
                                              cluster->current_scene[set]);
    hal_zigbee_send_cmd_to_bindings(&c);
}

static void switch_cluster_restart_lockout(zigbee_switch_cluster *cluster,
                                           uint8_t set) {
    hal_tasks_unschedule(&cluster->mode_task);
    hal_tasks_schedule(&cluster->mode_task,
                       (uint32_t)cluster->scene_lockout_s[set] * 1000);
}

// Double click (set 0) or Triple click (set 1) in normal mode
static void switch_cluster_enter_scenes(zigbee_switch_cluster *cluster,
                                        uint8_t set) {
    uint8_t count = cluster->scene_count[set];

    if (count == 0) {
        switch_cluster_play_sos(cluster); // Set is off
        return;
    }
    if (hal_zigbee_get_network_status() != HAL_ZIGBEE_NETWORK_JOINED) {
        return;
    }

    uint8_t first = switch_cluster_scene_first_id(set);
    uint8_t last  = cluster->last_scene[set];

    cluster->current_scene[set] =
        (last >= first && last < first + count) ? last : first;
    cluster->op_mode = set == 0 ? SWITCH_OP_MODE_SCENES_SET1
                                : SWITCH_OP_MODE_SCENES_SET2;
    switch_cluster_recall_scene(cluster, set);
    switch_cluster_set_onoff_flag(cluster, 1);
    switch_cluster_restart_lockout(cluster, set);
    hal_tasks_unschedule(&cluster->led_task);
    cluster->sos_step = 0;
    hal_tasks_schedule(&cluster->led_task, 0);
}

// "Scene set off": SOS once on the LED, only if it is not blinking for
// something else (e.g. not connected)
static void switch_cluster_play_sos(zigbee_switch_cluster *cluster) {
    if (cluster->indicator_led == NULL ||
        cluster->indicator_led->blink_times_left != 0 ||
        cluster->op_mode != SWITCH_OP_MODE_NORMAL) {
        return;
    }
    hal_tasks_unschedule(&cluster->led_task);
    cluster->sos_step = 1;
    hal_tasks_schedule(&cluster->led_task, 0);
}

// Light on Quadruple: level now, colour temperature a bit later (a lamp
// that is off may ignore a colour command). One-shot, no mode.
static void switch_cluster_light_on_quadruple(zigbee_switch_cluster *cluster) {
    if (hal_zigbee_get_network_status() != HAL_ZIGBEE_NETWORK_JOINED) {
        return;
    }
    uint8_t level = (uint8_t)(((uint16_t)cluster->quadruple_level_pct *
                               LAMP_LEVEL_MAX + 50) / 100);
    hal_zigbee_cmd c = build_level_move_to_level_onoff_cmd(cluster->endpoint,
                                                           level);
    hal_zigbee_send_cmd_to_bindings(&c);

    hal_tasks_unschedule(&cluster->quadruple_color_task);
    if (level == 0) {
        switch_cluster_set_onoff_flag(cluster, 0); // Level 0 % = off
        return;
    }
    switch_cluster_set_onoff_flag(cluster, 1);
    hal_tasks_schedule(&cluster->quadruple_color_task,
                       QUADRUPLE_COLOR_DELAY_MS);
}

static void switch_cluster_quadruple_color_task_handler(void *arg) {
    zigbee_switch_cluster *cluster = (zigbee_switch_cluster *)arg;

    if (hal_zigbee_get_network_status() != HAL_ZIGBEE_NETWORK_JOINED) {
        return;
    }
    uint32_t       kelvin = cluster->quadruple_color_temp_k;
    uint16_t       mireds = (uint16_t)((1000000UL + kelvin / 2) / kelvin);
    hal_zigbee_cmd c      = build_move_to_color_temp_cmd(cluster->endpoint, mireds);
    hal_zigbee_send_cmd_to_bindings(&c);
}

// Short click in scene mode: next scene of the set, wrapping
static void switch_cluster_next_scene(zigbee_switch_cluster *cluster,
                                      uint8_t set) {
    uint8_t first = switch_cluster_scene_first_id(set);
    uint8_t count = cluster->scene_count[set];

    if (count == 0) {
        switch_cluster_leave_op_mode(cluster);
        return;
    }
    uint8_t idx = (uint8_t)(cluster->current_scene[set] - first);
    cluster->current_scene[set] = first + (uint8_t)((idx + 1) % count);
    switch_cluster_recall_scene(cluster, set);
    switch_cluster_restart_lockout(cluster, set);
}

// Back to normal mode. Leaving a scene set stores its last scene, only if
// it changed.
static void switch_cluster_leave_op_mode(zigbee_switch_cluster *cluster) {
    uint8_t prev = cluster->op_mode;

    cluster->op_mode            = SWITCH_OP_MODE_NORMAL;
    cluster->reset_wait_release = 0;
    hal_tasks_unschedule(&cluster->mode_task);
    hal_tasks_unschedule(&cluster->led_task);
    cluster->sos_step = 0;

    if (prev == SWITCH_OP_MODE_SCENES_SET1 ||
        prev == SWITCH_OP_MODE_SCENES_SET2) {
        uint8_t set = prev == SWITCH_OP_MODE_SCENES_SET1 ? 0 : 1;
        if (cluster->current_scene[set] != cluster->last_scene[set]) {
            cluster->last_scene[set] = cluster->current_scene[set];
            switch_cluster_store_u8(
                set == 0 ? NV_ITEM_SWITCH_LAST_SCENE_SET1(cluster->switch_idx)
                         : NV_ITEM_SWITCH_LAST_SCENE_SET2(cluster->switch_idx),
                cluster->last_scene[set]);
        }
    }
    if (prev != SWITCH_OP_MODE_NORMAL) {
        switch_cluster_restore_indicator(cluster);
    }
}

static void switch_cluster_led_task_handler(void *arg) {
    zigbee_switch_cluster *cluster = (zigbee_switch_cluster *)arg;

    if (cluster->sos_step != 0) {
        // SOS: step 1, 3, 5.. = mark on; 2, 4, 6.. = gap off
        uint8_t step = cluster->sos_step - 1;
        uint8_t mark = step / 2;
        if (mark >= SOS_MARKS || cluster->indicator_led == NULL) {
            cluster->sos_step = 0;
            switch_cluster_restore_indicator(cluster);
            return;
        }
        uint16_t delay;
        if ((step & 1) == 0) {
            led_on(cluster->indicator_led);
            delay = (mark >= 3 && mark < 6) ? SOS_LONG_MS : SOS_SHORT_MS;
        } else {
            led_off(cluster->indicator_led);
            delay = SOS_GAP_MS;
        }
        cluster->sos_step++;
        hal_tasks_schedule(&cluster->led_task, delay);
        return;
    }

    if (cluster->op_mode != SWITCH_OP_MODE_SCENES_SET1 &&
        cluster->op_mode != SWITCH_OP_MODE_SCENES_SET2) {
        return;
    }
    if (cluster->indicator_led != NULL) {
        led_blink(cluster->indicator_led, SCENE_LED_ON_MS, SCENE_LED_OFF_MS,
                  cluster->op_mode == SWITCH_OP_MODE_SCENES_SET1 ? 2 : 3);
    }
    hal_tasks_schedule(&cluster->led_task, SCENE_LED_PERIOD_MS);
}

// Lockout expired (scene mode) or reset confirmation window over
static void switch_cluster_mode_task_handler(void *arg) {
    zigbee_switch_cluster *cluster = (zigbee_switch_cluster *)arg;
    bool in_click = cluster->button->pressed ||
                    cluster->gesture_pending;

    if (cluster->op_mode == SWITCH_OP_MODE_SCENES_SET1 ||
        cluster->op_mode == SWITCH_OP_MODE_SCENES_SET2) {
        if (in_click) {
            // A click is under way: let it finish in scene mode
            hal_tasks_schedule(&cluster->mode_task, 100);
            return;
        }
        switch_cluster_leave_op_mode(cluster);
    } else if (cluster->op_mode == SWITCH_OP_MODE_RESET_ARMED) {
        if (cluster->reset_wait_release || in_click) {
            return; // The gesture in progress decides
        }
        switch_cluster_leave_op_mode(cluster);
    }
}

// Layer 2: a gesture confirmed after the multi-press window
static void switch_cluster_on_click_gesture(zigbee_switch_cluster *cluster,
                                            uint8_t count) {
    switch (cluster->op_mode) {
    case SWITCH_OP_MODE_NORMAL:
        if (count == 1) {
            // Short click: On/Off from the flag, no Read, no level
            if (hal_zigbee_get_network_status() == HAL_ZIGBEE_NETWORK_JOINED) {
                uint8_t cmd_id = cluster->onoff_flag ? ZCL_CMD_ONOFF_OFF
                                                            : ZCL_CMD_ONOFF_ON;
                hal_zigbee_cmd c = build_onoff_cmd(cluster->endpoint, cmd_id);
                hal_zigbee_send_cmd_to_bindings(&c);
                switch_cluster_set_onoff_flag(cluster, !cluster->onoff_flag);
            }
        } else if (count == 2) {
            switch_cluster_enter_scenes(cluster, 0);
        } else if (count == 3) {
            switch_cluster_enter_scenes(cluster, 1);
        } else if (count == 4) {
            switch_cluster_light_on_quadruple(cluster);
        }
        break;

    case SWITCH_OP_MODE_SCENES_SET1:
    case SWITCH_OP_MODE_SCENES_SET2:
        if (count == 1) {
            switch_cluster_next_scene(
                cluster, cluster->op_mode == SWITCH_OP_MODE_SCENES_SET1 ? 0 : 1);
        }
        // Double / Triple / Quadruple click: no effect
        break;

    case SWITCH_OP_MODE_RESET_ARMED:
        if (count == 1 && (cluster->click_press_ms - cluster->reset_release_ms) <=
            RESET_CONFIRM_WINDOW_MS) {
            printf("Factory reset confirmed\r\n");
            hal_factory_reset();
        }
        switch_cluster_leave_op_mode(cluster);
        break;
    }
}

// Layer 1: multi-press window expired after the last release
static void switch_cluster_gesture_task_handler(void *arg) {
    zigbee_switch_cluster *cluster = (zigbee_switch_cluster *)arg;

    if (!cluster->gesture_pending) {
        return;
    }
    cluster->gesture_pending = 0;

    uint8_t count = cluster->gesture_count;
    cluster->gesture_count = 0;

    if (count == 1) {
        switch_cluster_report_gesture(cluster, MULTISTATE_SINGLE_CLICK);
    } else if (count == 2) {
        switch_cluster_report_gesture(cluster, MULTISTATE_DOUBLE_CLICK);
    } else if (count == 3) {
        switch_cluster_report_gesture(cluster, MULTISTATE_TRIPLE_CLICK);
    } else if (count == 4) {
        switch_cluster_report_gesture(cluster, MULTISTATE_QUADRUPLE_CLICK);
    } else if (cluster->op_mode != SWITCH_OP_MODE_RESET_ARMED) {
        return; // 5+ presses: ignored
    }
    switch_cluster_on_click_gesture(cluster, count);
}

// Layer 1: Very long hold, 15 s from the press edge (continues a Long click)
static void switch_cluster_very_long_task_handler(void *arg) {
    zigbee_switch_cluster *cluster = (zigbee_switch_cluster *)arg;

    if (!cluster->button->pressed || !cluster->gesture_long ||
        cluster->press_op_mode != SWITCH_OP_MODE_NORMAL ||
        cluster->op_mode != SWITCH_OP_MODE_NORMAL) {
        return; // Factory reset only from normal mode
    }
    printf("Very long hold: factory reset armed\r\n");
    if (cluster->dim_hold) {
        switch_cluster_end_dimming(cluster);
        cluster->dim_hold = 0;
    }
    cluster->op_mode            = SWITCH_OP_MODE_RESET_ARMED;
    cluster->reset_wait_release = 1;
    hal_tasks_unschedule(&cluster->led_task);
    cluster->sos_step = 0;
    if (cluster->indicator_led != NULL) {
        led_blink(cluster->indicator_led, RESET_LED_ON_MS, RESET_LED_OFF_MS,
                  LED_BLINK_FOREVER);
    }
}

static void switch_cluster_gesture_on_press(zigbee_switch_cluster *cluster) {
    uint32_t held = hal_millis() - cluster->button->pressed_at_ms;

    if (cluster->gesture_pending) {
        // Within the multi-press window of the previous release
        hal_tasks_unschedule(&cluster->gesture_task);
        cluster->gesture_pending = 0;
        if (cluster->gesture_count < 255) {
            cluster->gesture_count++;
        }
    } else {
        cluster->gesture_count = 1;
    }
    cluster->gesture_long      = 0;
    cluster->press_op_mode     = cluster->op_mode;
    cluster->click_press_ms    = cluster->button->pressed_at_ms;
    cluster->press_seen        = 1;
    cluster->lamp_answers_open = 0;
    // The multistate value is not touched: a press alone is not reported

    hal_tasks_unschedule(&cluster->very_long_task);
    hal_tasks_schedule(&cluster->very_long_task,
                       held < VERY_LONG_HOLD_MS ? VERY_LONG_HOLD_MS - held : 0);

    // Lamp state is only needed for dimming, i.e. a Long click in normal
    // mode, which can only start a new sequence
    if (cluster->op_mode == SWITCH_OP_MODE_NORMAL &&
        cluster->gesture_count == 1) {
        cluster->dim_regrab = cluster->last_dim_valid &&
                              cluster->dimming_reset_window_ms > 0 &&
                              (hal_millis() - cluster->last_dim_end_ms) <=
                              cluster->dimming_reset_window_ms;
        cluster->lamp_answers_open = 1;
        switch_cluster_request_lamp_state(cluster);
    } else {
        cluster->lamps_cnt = 0;
    }
}

static void switch_cluster_gesture_on_long_press(zigbee_switch_cluster *cluster) {
    // A hold always becomes a Long click; presses before it are dropped
    cluster->gesture_long  = 1;
    cluster->gesture_count = 0;
    cluster->dim_active    = 0;

    if (cluster->op_mode == SWITCH_OP_MODE_NORMAL) {
        switch_cluster_level_control(cluster);
        cluster->dim_hold = 1;
    } else {
        // Scene mode: exit without dimming. Reset armed: cancel.
        switch_cluster_leave_op_mode(cluster);
        cluster->dim_hold = 0;
    }
    cluster->lamp_answers_open = 0;

    cluster->multistate_state = MULTISTATE_LONG_PRESS;
    hal_zigbee_notify_attribute_changed(cluster->endpoint,
                                        ZCL_CLUSTER_MULTISTATE_INPUT_BASIC,
                                        ZCL_ATTR_MULTISTATE_INPUT_PRESENT_VALUE);
}

static void switch_cluster_gesture_on_release(zigbee_switch_cluster *cluster) {
    // Release without a seen press: the button was held at boot. Ignore it.
    if (!cluster->press_seen) {
        return;
    }
    cluster->press_seen        = 0;
    cluster->lamp_answers_open = 0;
    hal_tasks_unschedule(&cluster->very_long_task);

    if (cluster->gesture_long) {
        cluster->gesture_long = 0;
        if (cluster->op_mode == SWITCH_OP_MODE_RESET_ARMED &&
            cluster->reset_wait_release) {
            // Armed at 15 s: the confirmation window starts at this release
            uint32_t since = hal_millis() - cluster->button->released_at_ms;
            cluster->reset_wait_release = 0;
            cluster->reset_release_ms   = cluster->button->released_at_ms;
            hal_tasks_unschedule(&cluster->mode_task);
            hal_tasks_schedule(&cluster->mode_task,
                               since < RESET_CONFIRM_WINDOW_MS
                               ? RESET_CONFIRM_WINDOW_MS - since
                               : 0);
        } else if (cluster->dim_hold) {
            switch_cluster_end_dimming(cluster);
        }
        cluster->dim_hold         = 0;
        cluster->multistate_state = MULTISTATE_NOT_PRESSED;
        hal_zigbee_notify_attribute_changed(
            cluster->endpoint, ZCL_CLUSTER_MULTISTATE_INPUT_BASIC,
            ZCL_ATTR_MULTISTATE_INPUT_PRESENT_VALUE);
        return;
    }

    // Short press released: wait for another press within the window,
    // measured from the release edge
    uint32_t since = hal_millis() - cluster->button->released_at_ms;

    cluster->gesture_pending = 1;
    hal_tasks_unschedule(&cluster->gesture_task);
    hal_tasks_schedule(&cluster->gesture_task,
                       since < cluster->multi_press_window_ms
                       ? cluster->multi_press_window_ms - since
                       : 0);
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

    if (switch_cluster_gestures_active(cluster)) {
        switch_cluster_gesture_on_press(cluster);
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

    cluster->lamp_answers_open = 1;
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

    if (switch_cluster_gestures_active(cluster)) {
        switch_cluster_gesture_on_release(cluster);
        return;
    }

    // Release without a seen press: the button was already held at boot
    // (btn_init marks it pressed without calling on_press). Ignore it,
    // otherwise it is sent as a phantom short press.
    if (cluster->multistate_state == MULTISTATE_NOT_PRESSED) {
        return;
    }
    cluster->lamp_answers_open = 0;

    if (cluster->multistate_state != MULTISTATE_LONG_PRESS) {
        if (cluster->relay_mode == ZCL_ONOFF_CONFIGURATION_RELAY_MODE_SHORT) {
            switch_cluster_relay_action_on(cluster);
        }
        if (cluster->binded_mode == ZCL_ONOFF_CONFIGURATION_BINDED_MODE_SHORT) {
            switch_cluster_binding_action_on(cluster);
        }
    } else {
        // This is end of long press, send zcl_level stop
        switch_cluster_end_dimming(cluster);
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

    if (switch_cluster_gestures_active(cluster)) {
        switch_cluster_gesture_on_long_press(cluster);
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
    cluster->lamp_answers_open = 0;

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
    if (switch_cluster_on_write_gesture_attr(cluster, attribute_id)) {
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
    if (!switch_cluster_gestures_active(cluster)) {
        // Gestures switched off (mode, relay mode or binded mode changed)
        hal_tasks_unschedule(&cluster->gesture_task);
        hal_tasks_unschedule(&cluster->very_long_task);
        cluster->gesture_pending = 0;
        cluster->gesture_count   = 0;
        cluster->gesture_long    = 0;
        if (cluster->op_mode != SWITCH_OP_MODE_NORMAL) {
            switch_cluster_leave_op_mode(cluster);
        }
    }
    switch_cluster_store_attrs_to_nv(cluster);
}

// Multi-press settings: each one in its own NV item. Returns true if the
// attribute was one of them.
static bool switch_cluster_on_write_gesture_attr(zigbee_switch_cluster *cluster,
                                                 uint16_t attribute_id) {
    uint8_t idx = cluster->switch_idx;

    switch (attribute_id) {
    case ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_DIMMING_SYNC:
        cluster->dimming_sync = cluster->dimming_sync ? 1 : 0;
        switch_cluster_store_u8(NV_ITEM_SWITCH_DIMMING_SYNC(idx),
                                cluster->dimming_sync);
        return true;

    case ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_MULTI_PRESS_WINDOW:
        if (cluster->multi_press_window_ms < MULTI_PRESS_WINDOW_MIN_MS) {
            cluster->multi_press_window_ms = MULTI_PRESS_WINDOW_MIN_MS;
        } else if (cluster->multi_press_window_ms > MULTI_PRESS_WINDOW_MAX_MS) {
            cluster->multi_press_window_ms = MULTI_PRESS_WINDOW_MAX_MS;
        }
        switch_cluster_store_u16(NV_ITEM_SWITCH_MULTI_PRESS_WINDOW(idx),
                                 cluster->multi_press_window_ms);
        return true;

    case ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_SCENE_COUNT_SET1:
    case ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_SCENE_COUNT_SET2: {
        uint8_t set =
            attribute_id == ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_SCENE_COUNT_SET1
            ? 0 : 1;
        if (cluster->scene_count[set] > SCENE_COUNT_MAX) {
            cluster->scene_count[set] = SCENE_COUNT_MAX;
        }
        // Leave the set if it is in use, so it restarts with the new count
        if (cluster->op_mode == (set == 0 ? SWITCH_OP_MODE_SCENES_SET1
                                          : SWITCH_OP_MODE_SCENES_SET2)) {
            switch_cluster_leave_op_mode(cluster);
        }
        switch_cluster_store_u8(set == 0 ? NV_ITEM_SWITCH_SCENE_COUNT_SET1(idx)
                                         : NV_ITEM_SWITCH_SCENE_COUNT_SET2(idx),
                                cluster->scene_count[set]);
        return true;
    }

    case ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_SCENE_GROUP_ID_SET1:
        switch_cluster_store_u16(NV_ITEM_SWITCH_SCENE_GROUP_ID_SET1(idx),
                                 cluster->scene_group_id[0]);
        return true;

    case ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_SCENE_GROUP_ID_SET2:
        switch_cluster_store_u16(NV_ITEM_SWITCH_SCENE_GROUP_ID_SET2(idx),
                                 cluster->scene_group_id[1]);
        return true;

    case ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_SCENE_LOCKOUT_SET1:
    case ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_SCENE_LOCKOUT_SET2: {
        uint8_t set =
            attribute_id == ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_SCENE_LOCKOUT_SET1
            ? 0 : 1;
        if (cluster->scene_lockout_s[set] < SCENE_LOCKOUT_MIN_S) {
            cluster->scene_lockout_s[set] = SCENE_LOCKOUT_MIN_S;
        } else if (cluster->scene_lockout_s[set] > SCENE_LOCKOUT_MAX_S) {
            cluster->scene_lockout_s[set] = SCENE_LOCKOUT_MAX_S;
        }
        switch_cluster_store_u8(set == 0 ? NV_ITEM_SWITCH_SCENE_LOCKOUT_SET1(idx)
                                         : NV_ITEM_SWITCH_SCENE_LOCKOUT_SET2(idx),
                                cluster->scene_lockout_s[set]);
        return true;
    }

    case ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_QUADRUPLE_LEVEL:
        if (cluster->quadruple_level_pct > 100) {
            cluster->quadruple_level_pct = 100;
        }
        switch_cluster_store_u8(NV_ITEM_SWITCH_QUADRUPLE_LEVEL(idx),
                                cluster->quadruple_level_pct);
        return true;

    case ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_QUADRUPLE_COLOR_TEMP:
        if (cluster->quadruple_color_temp_k < QUADRUPLE_COLOR_TEMP_MIN_K) {
            cluster->quadruple_color_temp_k = QUADRUPLE_COLOR_TEMP_MIN_K;
        } else if (cluster->quadruple_color_temp_k > QUADRUPLE_COLOR_TEMP_MAX_K) {
            cluster->quadruple_color_temp_k = QUADRUPLE_COLOR_TEMP_MAX_K;
        }
        switch_cluster_store_u16(NV_ITEM_SWITCH_QUADRUPLE_COLOR_TEMP(idx),
                                 cluster->quadruple_color_temp_k);
        return true;
    }
    return false;
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
