#ifndef _SWITCH_CLUSTER_H_
#define _SWITCH_CLUSTER_H_

#include "base_components/button.h"
#include "base_components/led.h"
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

typedef struct {
    uint8_t              switch_idx;
    uint8_t              endpoint;
    uint8_t              mode;
    uint8_t              action;
    uint8_t              relay_mode;
    uint8_t              relay_index;
    uint8_t              binded_mode;
    button_t *           button;
    hal_zigbee_attribute attr_infos[10];
    uint16_t             multistate_state;
    hal_zigbee_attribute multistate_attr_infos[4];
    uint8_t              level_move_rate;
    uint8_t              level_move_direction;
    led_t *              indicator_led;

    // Smart long-press dimming (fork). State of the bound lamp is read on
    // every press and only trusted for that same press.
    uint8_t              dimming_threshold_pct;   // persisted, 0..100
    uint16_t             dimming_reset_window_ms; // persisted, 0..5000
    uint8_t              dim_regrab;              // this press is a re-grab
    uint8_t              lamp_state_valid;        // bitmask, see .c
    uint8_t              lamp_on;
    uint8_t              lamp_level;
    uint8_t              last_dim_valid;
    uint8_t              last_dim_direction;
    uint32_t             last_dim_end_ms;
} zigbee_switch_cluster;

void switch_cluster_add_to_endpoint(zigbee_switch_cluster *cluster,
                                    hal_zigbee_endpoint *endpoint);

void switch_cluster_callback_attr_write_trampoline(uint8_t endpoint,
                                                   uint16_t attribute_id);

void update_switch_clusters(void);

// Handler for Read Attributes Responses (bound lamp state).
void switch_cluster_on_read_rsp(uint8_t endpoint, uint16_t cluster_id,
                                uint16_t attribute_id, uint8_t status,
                                uint8_t data_type, const uint8_t *value);

#endif
