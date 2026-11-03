"""Switch settings loaded from NV (fork fix for review bug #10 + NC polarity).

1. Every field of the switch config NV item is range-checked on load. An
   invalid field keeps the default from the config string; valid fields from
   the same item are still used. The NV item itself is not rewritten.
2. momentary_nc inverts the button. The polarity used to be set only when the
   mode was written from Z2M, so it was lost after a restart.
"""

import os
import struct

from tests.client import StubProc
from tests.conftest import Device, RelayButtonPair
from tests.zcl_consts import (
    ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_ACTIONS,
    ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_BINDING_MODE,
    ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_LEVEL_MOVE_RATE,
    ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_LONG_PRESS_DUR,
    ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_MODE,
    ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_RELAY_MODE,
    ZCL_CLUSTER_ON_OFF,
    ZCL_CLUSTER_ON_OFF_SWITCH_CONFIG,
    ZCL_CMD_ONOFF_TOGGLE,
    ZCL_ONOFF_CONFIGURATION_BINDED_MODE_RISE,
    ZCL_ONOFF_CONFIGURATION_BINDED_MODE_SHORT,
    ZCL_ONOFF_CONFIGURATION_RELAY_MODE_RISE,
    ZCL_ONOFF_CONFIGURATION_SWITCH_ACTION_ONOFF,
    ZCL_ONOFF_CONFIGURATION_SWITCH_ACTION_TOGGLE_SIMPLE,
    ZCL_ONOFF_CONFIGURATION_SWITCH_TYPE_MOMENTARY,
    ZCL_ONOFF_CONFIGURATION_SWITCH_TYPE_MOMENTARY_NC,
    ZCL_ONOFF_CONFIGURATION_SWITCH_TYPE_TOGGLE,
)

NVM_DIR = "./stub_nvm_data"
# NV_ITEM_SWITCH_CLUSTER_DATA(0) = NV_ITEM_BASIC_CLUSTER_DATA (3) + 1 + 0
SWITCH0_ITEM = os.path.join(NVM_DIR, "item_04.bin")
# zigbee_switch_cluster_config: mode, action, relay_mode, relay_index (u8),
# button_long_press_duration (u16), level_move_rate, binded_mode (u8)
NV_FMT = "<BBBBHBB"

DEFAULT_LONG_PRESS_MS = 800
DEFAULT_MOVE_RATE = 50


def pin_number(pin: str) -> int:
    return (ord(pin[0].upper()) - ord("A")) * 16 + int(pin[1:])


def read_attr(dev: Device, ep: int, attr: int) -> int:
    return int(dev.read_zigbee_attr(ep, ZCL_CLUSTER_ON_OFF_SWITCH_CONFIG, attr))


def write_attr(dev: Device, ep: int, attr: int, value: int) -> None:
    dev.write_zigbee_attr(ep, ZCL_CLUSTER_ON_OFF_SWITCH_CONFIG, attr, value)


def read_nv() -> tuple[int, ...]:
    with open(SWITCH0_ITEM, "rb") as f:
        return struct.unpack(NV_FMT, f.read())


def write_nv(fields: tuple[int, ...]) -> None:
    with open(SWITCH0_ITEM, "wb") as f:
        f.write(struct.pack(NV_FMT, *fields))


def configure_valid(device_config: str, ep: int) -> None:
    """Store a fully valid, non-default config in NV."""
    with StubProc(device_config=device_config) as proc:
        dev = Device(proc)
        dev.zcl_switch_mode_set(ep, ZCL_ONOFF_CONFIGURATION_SWITCH_TYPE_MOMENTARY)
        dev.zcl_switch_actions_set(ep, ZCL_ONOFF_CONFIGURATION_SWITCH_ACTION_ONOFF)
        dev.zcl_switch_relay_mode_set(ep, ZCL_ONOFF_CONFIGURATION_RELAY_MODE_RISE)
        dev.zcl_switch_binding_mode_set(ep, ZCL_ONOFF_CONFIGURATION_BINDED_MODE_RISE)
        write_attr(dev, ep, ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_LONG_PRESS_DUR, 1234)
        write_attr(dev, ep, ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_LEVEL_MOVE_RATE, 77)
    assert os.path.exists(SWITCH0_ITEM)


def test_valid_nv_config_is_loaded(
    device_config: str, relay_button_pair: RelayButtonPair
):
    ep = relay_button_pair.switch_endpoint
    configure_valid(device_config, ep)

    with StubProc(device_config=device_config) as proc:
        dev = Device(proc)
        assert (
            read_attr(dev, ep, ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_MODE)
            == ZCL_ONOFF_CONFIGURATION_SWITCH_TYPE_MOMENTARY
        )
        assert (
            read_attr(dev, ep, ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_ACTIONS)
            == ZCL_ONOFF_CONFIGURATION_SWITCH_ACTION_ONOFF
        )
        assert (
            read_attr(dev, ep, ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_RELAY_MODE)
            == ZCL_ONOFF_CONFIGURATION_RELAY_MODE_RISE
        )
        assert (
            read_attr(dev, ep, ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_BINDING_MODE)
            == ZCL_ONOFF_CONFIGURATION_BINDED_MODE_RISE
        )
        assert read_attr(dev, ep, ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_LONG_PRESS_DUR) == 1234
        assert read_attr(dev, ep, ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_LEVEL_MOVE_RATE) == 77


def test_invalid_nv_fields_fall_back_to_defaults(
    device_config: str, relay_button_pair: RelayButtonPair
):
    ep = relay_button_pair.switch_endpoint
    configure_valid(device_config, ep)

    _, _, _, relay_index, _, _, _ = read_nv()
    broken = (7, 9, 8, relay_index, 60000, 0, 0)
    write_nv(broken)

    with StubProc(device_config=device_config) as proc:
        dev = Device(proc)
        # Defaults from the config string (no 'M' -> toggle)
        assert (
            read_attr(dev, ep, ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_MODE)
            == ZCL_ONOFF_CONFIGURATION_SWITCH_TYPE_TOGGLE
        )
        assert (
            read_attr(dev, ep, ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_ACTIONS)
            == ZCL_ONOFF_CONFIGURATION_SWITCH_ACTION_TOGGLE_SIMPLE
        )
        assert (
            read_attr(dev, ep, ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_BINDING_MODE)
            == ZCL_ONOFF_CONFIGURATION_BINDED_MODE_SHORT
        )
        assert (
            read_attr(dev, ep, ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_LONG_PRESS_DUR)
            == DEFAULT_LONG_PRESS_MS
        )
        assert (
            read_attr(dev, ep, ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_LEVEL_MOVE_RATE)
            == DEFAULT_MOVE_RATE
        )
        relay_mode = read_attr(dev, ep, ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_RELAY_MODE)
        assert relay_mode <= 3

    # The NV item itself is untouched by the load.
    assert read_nv() == broken


def test_one_invalid_field_keeps_the_valid_ones(
    device_config: str, relay_button_pair: RelayButtonPair
):
    ep = relay_button_pair.switch_endpoint
    configure_valid(device_config, ep)

    fields = list(read_nv())
    fields[0] = 0xFF  # mode
    write_nv(tuple(fields))

    with StubProc(device_config=device_config) as proc:
        dev = Device(proc)
        assert (
            read_attr(dev, ep, ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_MODE)
            == ZCL_ONOFF_CONFIGURATION_SWITCH_TYPE_TOGGLE
        )
        assert (
            read_attr(dev, ep, ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_ACTIONS)
            == ZCL_ONOFF_CONFIGURATION_SWITCH_ACTION_ONOFF
        )
        assert read_attr(dev, ep, ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_LONG_PRESS_DUR) == 1234
        assert read_attr(dev, ep, ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_LEVEL_MOVE_RATE) == 77


def test_momentary_nc_polarity_survives_restart(
    device_config: str, relay_button_pair: RelayButtonPair
):
    ep = relay_button_pair.switch_endpoint
    pin = relay_button_pair.button_pin

    with StubProc(device_config=device_config) as proc:
        dev = Device(proc)
        dev.zcl_switch_mode_set(ep, ZCL_ONOFF_CONFIGURATION_SWITCH_TYPE_MOMENTARY_NC)
        dev.zcl_switch_binding_mode_set(ep, ZCL_ONOFF_CONFIGURATION_BINDED_MODE_SHORT)
        dev.zcl_switch_actions_set(
            ep, ZCL_ONOFF_CONFIGURATION_SWITCH_ACTION_TOGGLE_SIMPLE
        )

    # NC contact is closed at rest: the pin is low at boot and goes high
    # while the button is pressed.
    with StubProc(device_config=device_config, low_at_boot=[pin_number(pin)]) as proc:
        dev = Device(proc)
        dev.step_time(200)
        dev.status()
        assert dev.zcl_list_cmds(endpoint=ep) == []

        dev.set_gpio(pin, 1)  # press
        dev.step_time(100)
        dev.status()
        assert dev.zcl_list_cmds(endpoint=ep, cluster=ZCL_CLUSTER_ON_OFF) == []

        dev.set_gpio(pin, 0)  # release
        dev.step_time(100)
        dev.wait_for_cmd_send(ep, ZCL_CLUSTER_ON_OFF, ZCL_CMD_ONOFF_TOGGLE)
