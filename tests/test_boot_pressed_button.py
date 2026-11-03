"""Button held during power-on (fork fix for review bug #9).

btn_init() marks a button that is already held at boot as pressed without
calling on_press. Before the fix, its release went through the normal
short-press path in momentary mode and sent a phantom command to the bound
lamp. Now a release without a seen press is ignored; the next real press
works as usual.
"""

import pytest

from tests.client import StubProc
from tests.conftest import Device, RelayButtonPair
from tests.zcl_consts import (
    ZCL_CLUSTER_ON_OFF,
    ZCL_CMD_ONOFF_TOGGLE,
    ZCL_ONOFF_CONFIGURATION_BINDED_MODE_SHORT,
    ZCL_ONOFF_CONFIGURATION_SWITCH_ACTION_TOGGLE_SIMPLE,
    ZCL_ONOFF_CONFIGURATION_SWITCH_TYPE_MOMENTARY,
)


def pin_number(pin: str) -> int:
    return (ord(pin[0].upper()) - ord("A")) * 16 + int(pin[1:])


@pytest.mark.parametrize("held_ms", [100, 2000], ids=["short_hold", "long_hold"])
def test_momentary_release_of_button_held_at_boot_is_ignored(
    device_config: str, relay_button_pair: RelayButtonPair, held_ms: int
):
    ep = relay_button_pair.switch_endpoint
    pin = relay_button_pair.button_pin

    # Configure like the L-only test switch; settings persist in NVM.
    with StubProc(device_config=device_config) as proc:
        dev = Device(proc)
        dev.zcl_switch_mode_set(ep, ZCL_ONOFF_CONFIGURATION_SWITCH_TYPE_MOMENTARY)
        dev.zcl_switch_binding_mode_set(ep, ZCL_ONOFF_CONFIGURATION_BINDED_MODE_SHORT)
        dev.zcl_switch_actions_set(
            ep, ZCL_ONOFF_CONFIGURATION_SWITCH_ACTION_TOGGLE_SIMPLE
        )

    # Power-on with the button already held.
    with StubProc(device_config=device_config, low_at_boot=[pin_number(pin)]) as proc:
        dev = Device(proc)
        dev.step_time(held_ms)
        dev.release_button(pin)
        dev.step_time(500)
        dev.status()  # sync with the stub before checking events
        assert dev.zcl_list_cmds(endpoint=ep, include_global=True) == []

        # The next real press still works.
        dev.click_button(pin)
        dev.wait_for_cmd_send(ep, ZCL_CLUSTER_ON_OFF, ZCL_CMD_ONOFF_TOGGLE)
