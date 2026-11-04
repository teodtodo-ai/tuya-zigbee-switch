"""Smart long-press dimming (fork feature).

On every press the switch reads OnOff + CurrentLevel from the bound lamp.
On long press it picks the direction from that answer:
  - lamp off           -> MoveToLevelWithOnOff(min + 1), Move up 250 ms later
  - lamp on, <= X%     -> up
  - lamp on, >  X%     -> down
  - re-press within the reset window (default 1500 ms, 0 = off)
                       -> same direction as the previous long press
  - at a limit         -> never push against it
  - no/invalid answer  -> legacy behaviour (alternate direction)
"""

import pytest

from tests.client import StubProc
from tests.conftest import Device, RelayButtonPair
from tests.zcl_consts import (
    ZCL_CLUSTER_LEVEL_CONTROL,
    ZCL_CLUSTER_ON_OFF,
    ZCL_CLUSTER_ON_OFF_SWITCH_CONFIG,
    ZCL_CMD_LEVEL_MOVE_TO_LEVEL_WITH_ON_OFF,
    ZCL_CMD_LEVEL_MOVE_WITH_ON_OFF,
    ZCL_CMD_LEVEL_STOP_WITH_ON_OFF,
    ZCL_LEVEL_MOVE_DOWN,
    ZCL_LEVEL_MOVE_UP,
    ZCL_ONOFF_CONFIGURATION_SWITCH_TYPE_MOMENTARY,
    ZCL_ONOFF_CONFIGURATION_SWITCH_TYPE_TOGGLE,
)

ZCL_ATTR_DIMMING_THRESHOLD = 0xFF80
ZCL_ATTR_DIMMING_RESET_WINDOW = 0xFF81
ZCL_CMD_READ_ATTRIBUTES = 0x00
ZCL_ATTR_ONOFF = 0x0000
ZCL_ATTR_CURRENT_LEVEL = 0x0000
ZCL_TYPE_BOOLEAN = 0x10
ZCL_TYPE_UINT8 = 0x20
ZCL_STATUS_UNSUPPORTED_ATTRIBUTE = 0x86


@pytest.fixture()
def momentary_device(device: Device, relay_button_pair: RelayButtonPair) -> Device:
    device.zcl_switch_mode_set(
        relay_button_pair.switch_endpoint, ZCL_ONOFF_CONFIGURATION_SWITCH_TYPE_MOMENTARY
    )
    return device


def lamp_answers(
    dev: Device, ep: int, on: bool | None, level: int | None = None
) -> None:
    """Simulate the bound lamp answering our Read Attributes."""
    if on is not None:
        res = dev.p.exec(
            f"zcl_read_rsp {ep} 0x{ZCL_CLUSTER_ON_OFF:04X} 0x{ZCL_ATTR_ONOFF:04X} "
            f"0x00 0x{ZCL_TYPE_BOOLEAN:02X} {1 if on else 0:02X}"
        )
        assert res.ok, res.payload
    if level is not None:
        res = dev.p.exec(
            f"zcl_read_rsp {ep} 0x{ZCL_CLUSTER_LEVEL_CONTROL:04X} "
            f"0x{ZCL_ATTR_CURRENT_LEVEL:04X} 0x00 0x{ZCL_TYPE_UINT8:02X} {level:02X}"
        )
        assert res.ok, res.payload


DIM_UP_DELAY_MS = 250


def set_threshold(dev: Device, ep: int, pct: int) -> None:
    dev.write_zigbee_attr(
        ep, ZCL_CLUSTER_ON_OFF_SWITCH_CONFIG, ZCL_ATTR_DIMMING_THRESHOLD, pct
    )


def get_threshold(dev: Device, ep: int) -> int:
    return int(
        dev.read_zigbee_attr(
            ep, ZCL_CLUSTER_ON_OFF_SWITCH_CONFIG, ZCL_ATTR_DIMMING_THRESHOLD
        )
    )


def long_press(
    dev: Device,
    pair: RelayButtonPair,
    on: bool | None = None,
    level: int | None = None,
) -> int:
    """Press, let the lamp answer (if given), hold, release.

    Returns the direction of the Move command sent.
    """
    dev.clear_events()
    dev.press_button(pair.button_pin)
    lamp_answers(dev, pair.switch_endpoint, on, level)
    # Past the 800 ms long-press threshold plus the delayed Move up (lamp off)
    dev.step_time(900)
    dev.step_time(DIM_UP_DELAY_MS + 10)
    move = dev.wait_for_cmd_send(
        pair.switch_endpoint, ZCL_CLUSTER_LEVEL_CONTROL, ZCL_CMD_LEVEL_MOVE_WITH_ON_OFF
    )
    dev.release_button(pair.button_pin)
    dev.wait_for_cmd_send(
        pair.switch_endpoint, ZCL_CLUSTER_LEVEL_CONTROL, ZCL_CMD_LEVEL_STOP_WITH_ON_OFF
    )
    return move.data[0]


def sync(dev: Device) -> None:
    """Round-trip to the stub so events of already-run tasks are received."""
    dev.step_time(10)
    dev.status()


def move_to_level_cmds(dev: Device, ep: int) -> list:
    sync(dev)
    return [
        c
        for c in dev.zcl_list_cmds(ep, ZCL_CLUSTER_LEVEL_CONTROL)
        if c.cmd == ZCL_CMD_LEVEL_MOVE_TO_LEVEL_WITH_ON_OFF
    ]


# --- Read request -----------------------------------------------------------


def test_press_reads_lamp_onoff_and_level(
    momentary_device: Device, relay_button_pair: RelayButtonPair
):
    ep = relay_button_pair.switch_endpoint
    momentary_device.press_button(relay_button_pair.button_pin)

    for cluster in (ZCL_CLUSTER_ON_OFF, ZCL_CLUSTER_LEVEL_CONTROL):
        read = momentary_device.wait_for_cmd_send(
            ep, cluster, ZCL_CMD_READ_ATTRIBUTES, include_global=True
        )
        assert not read.cluster_specific
        assert read.data == bytes([0x00, 0x00])  # attribute 0x0000


def test_toggle_mode_does_not_read_lamp(
    device: Device, relay_button_pair: RelayButtonPair
):
    device.zcl_switch_mode_set(
        relay_button_pair.switch_endpoint, ZCL_ONOFF_CONFIGURATION_SWITCH_TYPE_TOGGLE
    )
    device.clear_events()
    device.press_button(relay_button_pair.button_pin)
    device.release_button(relay_button_pair.button_pin)
    sync(device)
    reads = [
        c
        for c in device.zcl_list_cmds(include_global=True)
        if not c.cluster_specific
    ]
    assert reads == []


# --- Direction rules --------------------------------------------------------


def test_lamp_off_starts_from_minimum_and_goes_up(
    momentary_device: Device, relay_button_pair: RelayButtonPair
):
    ep = relay_button_pair.switch_endpoint
    direction = long_press(momentary_device, relay_button_pair, on=False, level=254)
    assert direction == ZCL_LEVEL_MOVE_UP

    mtl = move_to_level_cmds(momentary_device, ep)
    assert len(mtl) == 1
    # Level 2, instant: a "with On/Off" command ending at level 1 (minimum)
    # would switch the lamp off again
    assert mtl[0].data == bytes([2, 0, 0])

    # MoveToLevel must go out before the Move
    level_cmds = [
        c.cmd for c in momentary_device.zcl_list_cmds(ep, ZCL_CLUSTER_LEVEL_CONTROL)
    ]
    assert level_cmds.index(ZCL_CMD_LEVEL_MOVE_TO_LEVEL_WITH_ON_OFF) < level_cmds.index(
        ZCL_CMD_LEVEL_MOVE_WITH_ON_OFF
    )



def test_lamp_off_move_up_is_delayed(
    momentary_device: Device, relay_button_pair: RelayButtonPair
):
    dev, ep = momentary_device, relay_button_pair.switch_endpoint
    dev.clear_events()
    dev.press_button(relay_button_pair.button_pin)
    lamp_answers(dev, ep, False, 254)
    dev.step_time(900)  # long press started, MoveToLevel sent
    assert len(move_to_level_cmds(dev, ep)) == 1
    moves = [
        c for c in dev.zcl_list_cmds(ep, ZCL_CLUSTER_LEVEL_CONTROL)
        if c.cmd == ZCL_CMD_LEVEL_MOVE_WITH_ON_OFF
    ]
    assert moves == []  # Move up not yet
    dev.step_time(DIM_UP_DELAY_MS)
    move = dev.wait_for_cmd_send(
        ep, ZCL_CLUSTER_LEVEL_CONTROL, ZCL_CMD_LEVEL_MOVE_WITH_ON_OFF
    )
    assert move.data[0] == ZCL_LEVEL_MOVE_UP
    dev.release_button(relay_button_pair.button_pin)


def test_lamp_off_released_before_delay_sends_no_move(
    momentary_device: Device, relay_button_pair: RelayButtonPair
):
    dev, ep = momentary_device, relay_button_pair.switch_endpoint
    dev.clear_events()
    dev.press_button(relay_button_pair.button_pin)
    lamp_answers(dev, ep, False, 254)
    dev.step_time(800)  # long press just started
    dev.release_button(relay_button_pair.button_pin)  # 60 ms later
    dev.wait_for_cmd_send(
        ep, ZCL_CLUSTER_LEVEL_CONTROL, ZCL_CMD_LEVEL_STOP_WITH_ON_OFF
    )
    dev.step_time(1_000)
    sync(dev)
    moves = [
        c for c in dev.zcl_list_cmds(ep, ZCL_CLUSTER_LEVEL_CONTROL)
        if c.cmd == ZCL_CMD_LEVEL_MOVE_WITH_ON_OFF
    ]
    assert moves == []
    # Next hold right after: lamp is on at a low level, continue up
    dev.step_time(200)
    assert long_press(dev, relay_button_pair, on=True, level=2) == ZCL_LEVEL_MOVE_UP


def test_lamp_on_above_threshold_goes_down(
    momentary_device: Device, relay_button_pair: RelayButtonPair
):
    direction = long_press(momentary_device, relay_button_pair, on=True, level=200)
    assert direction == ZCL_LEVEL_MOVE_DOWN
    assert move_to_level_cmds(momentary_device, relay_button_pair.switch_endpoint) == []


def test_lamp_on_below_threshold_goes_up(
    momentary_device: Device, relay_button_pair: RelayButtonPair
):
    direction = long_press(momentary_device, relay_button_pair, on=True, level=100)
    assert direction == ZCL_LEVEL_MOVE_UP


@pytest.mark.parametrize(
    "level,expected",
    [
        (152, ZCL_LEVEL_MOVE_UP),  # 59.8 % <= 60 %
        (153, ZCL_LEVEL_MOVE_DOWN),  # 60.2 % > 60 %
    ],
)
def test_default_threshold_is_60_percent(
    momentary_device: Device,
    relay_button_pair: RelayButtonPair,
    level: int,
    expected: int,
):
    assert get_threshold(momentary_device, relay_button_pair.switch_endpoint) == 60
    direction = long_press(momentary_device, relay_button_pair, on=True, level=level)
    assert direction == expected


@pytest.mark.parametrize(
    "threshold,level,expected",
    [
        (0, 10, ZCL_LEVEL_MOVE_DOWN),  # 0 %: lit lamp always dims down
        (100, 250, ZCL_LEVEL_MOVE_UP),  # 100 %: lit lamp always goes up
        (30, 100, ZCL_LEVEL_MOVE_DOWN),  # 39 % > 30 %
        (30, 70, ZCL_LEVEL_MOVE_UP),  # 27 % <= 30 %
    ],
)
def test_configured_threshold(
    momentary_device: Device,
    relay_button_pair: RelayButtonPair,
    threshold: int,
    level: int,
    expected: int,
):
    set_threshold(momentary_device, relay_button_pair.switch_endpoint, threshold)
    direction = long_press(momentary_device, relay_button_pair, on=True, level=level)
    assert direction == expected


# --- Re-grab and limits -----------------------------------------------------


def test_regrab_within_window_keeps_direction(
    momentary_device: Device, relay_button_pair: RelayButtonPair
):
    first = long_press(momentary_device, relay_button_pair, on=True, level=200)
    assert first == ZCL_LEVEL_MOVE_DOWN

    momentary_device.step_time(1_000)
    # Now below threshold, but re-grabbed quickly: keep dimming down
    second = long_press(momentary_device, relay_button_pair, on=True, level=100)
    assert second == ZCL_LEVEL_MOVE_DOWN


def test_regrab_after_window_uses_threshold_again(
    momentary_device: Device, relay_button_pair: RelayButtonPair
):
    first = long_press(momentary_device, relay_button_pair, on=True, level=200)
    assert first == ZCL_LEVEL_MOVE_DOWN

    momentary_device.step_time(3_500)
    second = long_press(momentary_device, relay_button_pair, on=True, level=100)
    assert second == ZCL_LEVEL_MOVE_UP


def test_continue_down_at_minimum_turns_up(
    momentary_device: Device, relay_button_pair: RelayButtonPair
):
    long_press(momentary_device, relay_button_pair, on=True, level=200)
    momentary_device.step_time(500)
    direction = long_press(momentary_device, relay_button_pair, on=True, level=1)
    assert direction == ZCL_LEVEL_MOVE_UP


def test_up_at_maximum_turns_down(
    momentary_device: Device, relay_button_pair: RelayButtonPair
):
    set_threshold(momentary_device, relay_button_pair.switch_endpoint, 100)
    direction = long_press(momentary_device, relay_button_pair, on=True, level=254)
    assert direction == ZCL_LEVEL_MOVE_DOWN


def test_regrab_after_lamp_went_off_starts_from_minimum(
    momentary_device: Device, relay_button_pair: RelayButtonPair
):
    # Dimmed all the way down, lamp switched itself off; grab again quickly
    long_press(momentary_device, relay_button_pair, on=True, level=200)
    momentary_device.step_time(500)
    direction = long_press(momentary_device, relay_button_pair, on=False, level=1)
    assert direction == ZCL_LEVEL_MOVE_UP
    assert len(move_to_level_cmds(momentary_device, relay_button_pair.switch_endpoint)) == 1


# --- Fallbacks ----------------------------------------------------------------


def test_no_answer_keeps_legacy_alternation(
    momentary_device: Device, relay_button_pair: RelayButtonPair
):
    assert long_press(momentary_device, relay_button_pair) == ZCL_LEVEL_MOVE_UP
    assert long_press(momentary_device, relay_button_pair) == ZCL_LEVEL_MOVE_DOWN
    assert move_to_level_cmds(momentary_device, relay_button_pair.switch_endpoint) == []


def test_error_answer_is_ignored(
    momentary_device: Device, relay_button_pair: RelayButtonPair
):
    ep = relay_button_pair.switch_endpoint
    momentary_device.clear_events()
    momentary_device.press_button(relay_button_pair.button_pin)
    # e.g. the coordinator answering "unsupported attribute"
    res = momentary_device.p.exec(
        f"zcl_read_rsp {ep} 0x{ZCL_CLUSTER_ON_OFF:04X} 0x0000 "
        f"0x{ZCL_STATUS_UNSUPPORTED_ATTRIBUTE:02X} 0x00"
    )
    assert res.ok
    momentary_device.step_time(2_000)
    move = momentary_device.wait_for_cmd_send(
        ep, ZCL_CLUSTER_LEVEL_CONTROL, ZCL_CMD_LEVEL_MOVE_WITH_ON_OFF
    )
    assert move.data[0] == ZCL_LEVEL_MOVE_UP  # legacy first direction
    assert move_to_level_cmds(momentary_device, ep) == []


def test_wrong_type_answer_is_ignored(
    momentary_device: Device, relay_button_pair: RelayButtonPair
):
    ep = relay_button_pair.switch_endpoint
    momentary_device.clear_events()
    momentary_device.press_button(relay_button_pair.button_pin)
    # OnOff answered as uint8 instead of boolean
    res = momentary_device.p.exec(
        f"zcl_read_rsp {ep} 0x{ZCL_CLUSTER_ON_OFF:04X} 0x0000 0x00 "
        f"0x{ZCL_TYPE_UINT8:02X} 00"
    )
    assert res.ok
    momentary_device.step_time(2_000)
    assert move_to_level_cmds(momentary_device, ep) == []


def test_answer_from_previous_press_is_not_reused(
    momentary_device: Device, relay_button_pair: RelayButtonPair
):
    ep = relay_button_pair.switch_endpoint
    # Smart press: lamp off -> up from minimum
    assert long_press(momentary_device, relay_button_pair, on=False) == ZCL_LEVEL_MOVE_UP
    momentary_device.step_time(3_500)
    # Next press gets no answer: must not reuse "lamp off" from last time
    direction = long_press(momentary_device, relay_button_pair)
    assert move_to_level_cmds(momentary_device, ep) == []
    assert direction == ZCL_LEVEL_MOVE_DOWN  # legacy alternation continues


def test_short_press_still_toggles_without_dimming(
    momentary_device: Device, relay_button_pair: RelayButtonPair
):
    ep = relay_button_pair.switch_endpoint
    momentary_device.clear_events()
    momentary_device.press_button(relay_button_pair.button_pin)
    lamp_answers(momentary_device, ep, on=True, level=200)
    momentary_device.release_button(relay_button_pair.button_pin)
    sync(momentary_device)
    assert momentary_device.zcl_list_cmds(ep, ZCL_CLUSTER_LEVEL_CONTROL) == []


# --- Threshold attribute and persistence -----------------------------------


def test_threshold_above_100_is_clamped(
    momentary_device: Device, relay_button_pair: RelayButtonPair
):
    set_threshold(momentary_device, relay_button_pair.switch_endpoint, 150)
    assert get_threshold(momentary_device, relay_button_pair.switch_endpoint) == 100


def test_threshold_survives_restart(
    device_config: str, relay_button_pair: RelayButtonPair
):
    ep = relay_button_pair.switch_endpoint
    with StubProc(device_config=device_config) as proc:
        dev = Device(proc)
        dev.zcl_switch_mode_set(ep, ZCL_ONOFF_CONFIGURATION_SWITCH_TYPE_MOMENTARY)
        set_threshold(dev, ep, 25)

    with StubProc(device_config=device_config) as proc:
        dev = Device(proc)
        assert get_threshold(dev, ep) == 25


# --- Reset window ---------------------------------------------------------------


def set_window(dev: Device, ep: int, ms: int) -> None:
    dev.write_zigbee_attr(
        ep, ZCL_CLUSTER_ON_OFF_SWITCH_CONFIG, ZCL_ATTR_DIMMING_RESET_WINDOW, ms
    )


def get_window(dev: Device, ep: int) -> int:
    return int(
        dev.read_zigbee_attr(
            ep, ZCL_CLUSTER_ON_OFF_SWITCH_CONFIG, ZCL_ATTR_DIMMING_RESET_WINDOW
        )
    )


def test_default_reset_window_is_1500_ms(
    momentary_device: Device, relay_button_pair: RelayButtonPair
):
    assert get_window(momentary_device, relay_button_pair.switch_endpoint) == 1500


@pytest.mark.parametrize(
    "pause_ms,expected",
    [
        (1_300, ZCL_LEVEL_MOVE_DOWN),  # re-pressed inside 1500 ms: keep going
        (1_700, ZCL_LEVEL_MOVE_UP),  # outside: threshold decides (39 % -> up)
    ],
)
def test_reset_window_is_measured_to_the_next_press(
    momentary_device: Device,
    relay_button_pair: RelayButtonPair,
    pause_ms: int,
    expected: int,
):
    # Window counts from release to the next *press*, not to the long-press
    # start 800 ms later
    long_press(momentary_device, relay_button_pair, on=True, level=200)
    momentary_device.step_time(pause_ms - 60)  # press_button adds 60 ms
    direction = long_press(momentary_device, relay_button_pair, on=True, level=100)
    assert direction == expected


def test_reset_window_zero_disables_continue(
    momentary_device: Device, relay_button_pair: RelayButtonPair
):
    set_window(momentary_device, relay_button_pair.switch_endpoint, 0)
    long_press(momentary_device, relay_button_pair, on=True, level=200)
    momentary_device.step_time(100)
    direction = long_press(momentary_device, relay_button_pair, on=True, level=100)
    assert direction == ZCL_LEVEL_MOVE_UP  # threshold decides immediately


def test_reset_window_configured_longer(
    momentary_device: Device, relay_button_pair: RelayButtonPair
):
    set_window(momentary_device, relay_button_pair.switch_endpoint, 4000)
    long_press(momentary_device, relay_button_pair, on=True, level=200)
    momentary_device.step_time(3_000)
    direction = long_press(momentary_device, relay_button_pair, on=True, level=100)
    assert direction == ZCL_LEVEL_MOVE_DOWN


def test_reset_window_above_5000_is_clamped(
    momentary_device: Device, relay_button_pair: RelayButtonPair
):
    set_window(momentary_device, relay_button_pair.switch_endpoint, 9000)
    assert get_window(momentary_device, relay_button_pair.switch_endpoint) == 5000


def test_reset_window_and_threshold_survive_restart(
    device_config: str, relay_button_pair: RelayButtonPair
):
    ep = relay_button_pair.switch_endpoint
    with StubProc(device_config=device_config) as proc:
        dev = Device(proc)
        set_threshold(dev, ep, 40)
        set_window(dev, ep, 2500)

    with StubProc(device_config=device_config) as proc:
        dev = Device(proc)
        assert get_threshold(dev, ep) == 40
        assert get_window(dev, ep) == 2500
