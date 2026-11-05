#ifndef DEVICE_CONFIG_NVM_ITEMS_H_
#define DEVICE_CONFIG_NVM_ITEMS_H_

#define MAX_RELAYS                       5
#define MAX_SWITCHES                     5
#define MAX_COVER_SWITCHES               3
#define MAX_COVERS                       3

#define NV_ITEM_CURRENT_VERSION_IN_NV    1
#define NV_ITEM_DEVICE_CONFIG            2
#define NV_ITEM_BASIC_CLUSTER_DATA       3
// switch_idx and relay_idx below are zero indexes, e.g. first switch has
// switch_idx = 0
#define NV_ITEM_SWITCH_CLUSTER_DATA(switch_idx) \
        (NV_ITEM_BASIC_CLUSTER_DATA + 1 + switch_idx)
#define NV_ITEM_RELAY_CLUSTER_DATA(relay_idx) \
        (NV_ITEM_BASIC_CLUSTER_DATA + MAX_SWITCHES + 1 + relay_idx)
#define NV_ITEM_COVER_SWITCH_CONFIG(cover_switch_idx) \
        (NV_ITEM_BASIC_CLUSTER_DATA + MAX_SWITCHES + MAX_RELAYS + 1 + cover_switch_idx)
#define NV_ITEM_COVER_CONFIG(cover_idx)                                                    \
        (NV_ITEM_BASIC_CLUSTER_DATA + MAX_SWITCHES + MAX_RELAYS + MAX_COVER_SWITCHES + 1 + \
         cover_idx)

// 3 + 5 (switches) + 5 (relays) + 3 (cover switches) + 3 (covers) = 19
// Adding room for future items, so starting from 32
#define NV_ITEM_DEVICE_TYPE                32

#define NV_ITEM_MULTI_PRESS_RESET_COUNT    33
#define NV_ITEM_POLL_CONTROL_CONFIG        34

// Custom (fork) items, kept well away from upstream's range so an upstream
// update can never reuse them. One item per switch.
#define NV_ITEM_SWITCH_DIMMING_CONFIG(switch_idx)         (0x40 + (switch_idx))
// Separate item so adding it did not change the size of the item above
#define NV_ITEM_SWITCH_DIMMING_WINDOW(switch_idx)         (0x48 + (switch_idx))
// Multi-press gestures and scenes: one item per setting and switch, so no
// item ever changes size (see bug #11)
#define NV_ITEM_SWITCH_DIMMING_SYNC(switch_idx)           (0x50 + (switch_idx))
#define NV_ITEM_SWITCH_MULTI_PRESS_WINDOW(switch_idx)     (0x58 + (switch_idx))
#define NV_ITEM_SWITCH_ONOFF_FLAG(switch_idx)             (0x60 + (switch_idx))
#define NV_ITEM_SWITCH_SCENE_COUNT_SET1(switch_idx)       (0x68 + (switch_idx))
#define NV_ITEM_SWITCH_SCENE_GROUP_ID_SET1(switch_idx)    (0x70 + (switch_idx))
#define NV_ITEM_SWITCH_SCENE_LOCKOUT_SET1(switch_idx)     (0x78 + (switch_idx))
#define NV_ITEM_SWITCH_SCENE_COUNT_SET2(switch_idx)       (0x80 + (switch_idx))
#define NV_ITEM_SWITCH_SCENE_GROUP_ID_SET2(switch_idx)    (0x88 + (switch_idx))
#define NV_ITEM_SWITCH_SCENE_LOCKOUT_SET2(switch_idx)     (0x90 + (switch_idx))
#define NV_ITEM_SWITCH_LAST_SCENE_SET1(switch_idx)        (0x98 + (switch_idx))
#define NV_ITEM_SWITCH_LAST_SCENE_SET2(switch_idx)        (0xA0 + (switch_idx))

#endif /* DEVICE_CONFIG_NVM_ITEMS_H_ */
