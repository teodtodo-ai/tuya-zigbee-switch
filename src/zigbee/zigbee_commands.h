#ifndef _ZIGBEE_COMMANDS_H_
#define _ZIGBEE_COMMANDS_H_

#include "consts.h"
#include "hal/zigbee.h"
#include <stddef.h>

static inline hal_zigbee_cmd build_onoff_cmd(uint8_t endpoint,
                                             uint8_t onoff_cmd_id) {
    hal_zigbee_cmd c = {
        .endpoint            = endpoint,
        .profile_id          = ZCL_HA_PROFILE,
        .cluster_id          = ZCL_CLUSTER_ON_OFF,
        .command_id          = onoff_cmd_id,
        .cluster_specific    =                               1,
        .direction           = HAL_ZIGBEE_DIR_CLIENT_TO_SERVER,
        .disable_default_rsp =                               1,
        .manufacturer_code   =                               0,
        .payload             = NULL,
        .payload_len         =                               0,
    };

    return c;
}

static inline hal_zigbee_cmd
build_level_move_onoff_cmd(uint8_t endpoint, uint8_t dir, uint8_t rate) {
    static uint8_t buf[2];

    buf[0] = dir;
    buf[1] = rate;

    hal_zigbee_cmd c = {
        .endpoint            = endpoint,
        .profile_id          = ZCL_HA_PROFILE,
        .cluster_id          = ZCL_CLUSTER_LEVEL_CONTROL,
        .command_id          = ZCL_CMD_LEVEL_MOVE_WITH_ON_OFF,
        .cluster_specific    =                               1,
        .direction           = HAL_ZIGBEE_DIR_CLIENT_TO_SERVER,
        .disable_default_rsp =                               1,
        .manufacturer_code   =                               0,
        .payload             = buf,
        .payload_len         = sizeof(buf),
    };
    return c;
}

// Move to level with On/Off, instant (transition time 0).
static inline hal_zigbee_cmd
build_level_move_to_level_onoff_cmd(uint8_t endpoint, uint8_t level) {
    static uint8_t buf[3];

    buf[0] = level;
    buf[1] = 0; // transition time, LSB (1/10 s)
    buf[2] = 0; // transition time, MSB

    hal_zigbee_cmd c = {
        .endpoint            = endpoint,
        .profile_id          = ZCL_HA_PROFILE,
        .cluster_id          = ZCL_CLUSTER_LEVEL_CONTROL,
        .command_id          = ZCL_CMD_LEVEL_MOVE_TO_LEVEL_WITH_ON_OFF,
        .cluster_specific    =                                       1,
        .direction           = HAL_ZIGBEE_DIR_CLIENT_TO_SERVER,
        .disable_default_rsp =                                       1,
        .manufacturer_code   =                                       0,
        .payload             = buf,
        .payload_len         = sizeof(buf),
    };
    return c;
}

// Global Read Attributes for a single attribute, sent to bindings of
// cluster_id. Used to ask a bound lamp for its current state.
static inline hal_zigbee_cmd build_read_attr_cmd(uint8_t endpoint,
                                                 uint16_t cluster_id,
                                                 uint16_t attr_id) {
    // Separate buffers per cluster: both reads are built back-to-back and the
    // payload pointer must stay valid until each one is sent.
    static uint8_t buf_onoff[2];
    static uint8_t buf_level[2];
    static uint8_t buf_other[2];
    uint8_t *      buf = cluster_id == ZCL_CLUSTER_ON_OFF ? buf_onoff
                       : cluster_id == ZCL_CLUSTER_LEVEL_CONTROL ? buf_level
                       : buf_other;

    buf[0] = (uint8_t)(attr_id & 0xFF);
    buf[1] = (uint8_t)(attr_id >> 8);

    hal_zigbee_cmd c = {
        .endpoint            = endpoint,
        .profile_id          = ZCL_HA_PROFILE,
        .cluster_id          = cluster_id,
        .command_id          = ZCL_CMD_READ_ATTRIBUTES,
        .cluster_specific    =                               0,
        .direction           = HAL_ZIGBEE_DIR_CLIENT_TO_SERVER,
        .disable_default_rsp =                               1,
        .manufacturer_code   =                               0,
        .payload             = buf,
        .payload_len         =                               2,
    };
    return c;
}

static inline hal_zigbee_cmd build_level_stop_onoff_cmd(uint8_t endpoint) {
    hal_zigbee_cmd c = {
        .endpoint            = endpoint,
        .profile_id          = ZCL_HA_PROFILE,
        .cluster_id          = ZCL_CLUSTER_LEVEL_CONTROL,
        .command_id          = ZCL_CMD_LEVEL_STOP_WITH_ON_OFF,
        .cluster_specific    =                               1,
        .direction           = HAL_ZIGBEE_DIR_CLIENT_TO_SERVER,
        .disable_default_rsp =                               1,
        .manufacturer_code   =                               0,
        .payload             = NULL,
        .payload_len         =                               0,
    };

    return c;
}

static inline hal_zigbee_cmd build_window_covering_cmd(uint8_t endpoint,
                                                       uint8_t cmd_id) {
    hal_zigbee_cmd c = {
        .endpoint            = endpoint,
        .profile_id          = ZCL_HA_PROFILE,
        .cluster_id          = ZCL_CLUSTER_WINDOW_COVERING,
        .command_id          = cmd_id,
        .cluster_specific    =                               1,
        .direction           = HAL_ZIGBEE_DIR_CLIENT_TO_SERVER,
        .disable_default_rsp =                               1,
        .manufacturer_code   =                               0,
        .payload             = NULL,
        .payload_len         =                               0,
    };

    return c;
}

#endif
