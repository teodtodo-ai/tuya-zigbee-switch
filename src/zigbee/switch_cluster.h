#ifndef _SWITCH_CLUSTER_H_
#define _SWITCH_CLUSTER_H_

#include "base_components/button.h"
#include "base_components/led.h"
#include "hal/tasks.h"
#include "hal/zigbee.h"
#include <stdint.h>

typedef struct {
    uint8_t  mode;
    uint8_t  action;
    uint8_t  relay_mode;
    uint8_t  relay_index;
    uint16_t button_long_press_duration;
    uint8_t  level_move_rate;
    uint8_t  binded_mode;
} zigbee_switch_cluster_config;

// Answers from bound lamps kept for one press (fork, smart dimming)
#define SWITCH_MAX_LAMPS    10

typedef struct {
    uint16_t addr;   // short address of the lamp
    uint8_t  level;  // CurrentLevel, valid with LAMP_LEVEL_VALID
    uint8_t  flags;  // LAMP_ONOFF_VALID | LAMP_IS_ON | LAMP_LEVEL_VALID
} switch_lamp_state_t;

// Layer 2 operating modes (fork, multi-press)
#define SWITCH_OP_MODE_NORMAL         0
#define SWITCH_OP_MODE_SCENES_SET1    1
#define SWITCH_OP_MODE_SCENES_SET2    2
#define SWITCH_OP_MODE_RESET_ARMED    3

typedef struct {
    uint8_t                          switch_idx;
    uint8_t                          endpoint;
    uint8_t                          mode;
    uint8_t                          action;
    uint8_t                          relay_mode;
    uint8_t                          relay_index;
    uint8_t                          binded_mode;
    button_t *                       button;
    hal_zigbee_attribute             attr_infos[20];
    uint16_t                         multistate_state;
    hal_zigbee_attribute             multistate_attr_infos[4];
    uint8_t                          level_move_rate;
    uint8_t                          level_move_direction;
    led_t *                          indicator_led;

    // Smart long-press dimming (fork). State of the bound lamp is read on
    // every press and only trusted for that same press.
    uint8_t                          dimming_threshold_pct;   // persisted, 0..100
    uint16_t                         dimming_reset_window_ms; // persisted, 0..5000
    uint8_t                          dim_regrab;              // this press is a re-grab
    uint8_t                          last_dim_valid;
    uint8_t                          last_dim_direction;
    uint32_t                         last_dim_end_ms;
    hal_task_t                       dim_up_task;      // delayed Move after MoveToLevel
    uint8_t                          delayed_move_dir; // direction of that Move

    // Answers of all bound lamps for the current press, collected for
    // LAMP_ANSWER_WINDOW_MS after the press edge
    switch_lamp_state_t              lamps[SWITCH_MAX_LAMPS];
    uint8_t                          lamps_cnt;
    uint32_t                         lamp_req_ms;

    // Dimming of the current hold, used to estimate the on/off flag
    uint8_t                          dimming_sync;   // persisted, 0/1
    uint8_t                          dim_active;     // a Move was started
    uint8_t                          dim_hold;       // this hold dims (gestures)
    uint8_t                          dim_dir;
    uint8_t                          dim_sync_level; // all lamps start here (sync)
    uint32_t                         dim_start_ms;
    uint32_t                         dim_stop_ms;

    // Layer 1: gestures (momentary, relay detached, binded mode short)
    uint16_t                         multi_press_window_ms; // persisted, 100..400
    uint8_t                          gesture_count;         // presses in this sequence
    uint8_t                          gesture_pending;       // waiting for the window
    uint8_t                          gesture_long;          // this press is a Long click
    uint8_t                          press_op_mode;         // op_mode at the press edge
    uint8_t                          press_seen;            // press edge seen (gestures)
    uint8_t                          lamp_answers_open;     // lamp answers accepted now
    uint32_t                         click_press_ms;        // press edge of last click
    hal_task_t                       gesture_task;          // multi-press window
    hal_task_t                       very_long_task;        // 15 s hold

    // Layer 2: actions
    uint8_t                          op_mode;                // SWITCH_OP_MODE_*
    uint8_t                          onoff_flag;             // persisted, 0/1
    uint8_t                          scene_count[2];         // persisted, 0..10
    uint16_t                         scene_group_id[2];      // persisted
    uint8_t                          scene_lockout_s[2];     // persisted, 5..15
    uint8_t                          last_scene[2];          // persisted, scene id
    uint8_t                          current_scene[2];
    uint8_t                          reset_wait_release;     // armed, button still held
    uint32_t                         reset_release_ms;
    hal_task_t                       mode_task;              // lockout / reset window
    hal_task_t                       led_task;               // scene LED pattern
    uint8_t                          sos_step;               // SOS LED step, 0 = idle
    // Light on Quadruple (persisted)
    uint8_t                          quadruple_level_pct;    // 0..100
    uint16_t                         quadruple_color_temp_k; // 2000..7000
    hal_task_t                       quadruple_color_task;   // colour after level
    ev_button_multi_press_callback_t orig_on_multi_press;
} zigbee_switch_cluster;

void switch_cluster_add_to_endpoint(zigbee_switch_cluster *cluster,
                                    hal_zigbee_endpoint *endpoint);

void switch_cluster_callback_attr_write_trampoline(uint8_t endpoint,
                                                   uint16_t attribute_id);

void update_switch_clusters(void);

// Handler for Read Attributes Responses (bound lamp state).
void switch_cluster_on_read_rsp(uint16_t src_addr, uint8_t endpoint,
                                uint16_t cluster_id,
                                uint16_t attribute_id, uint8_t status,
                                uint8_t data_type, const uint8_t *value);

#endif
