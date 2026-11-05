"""Multi-press gestures and scenes (fork feature).

Active for a momentary switch with relay detached and binded mode "short":

Layer 1 (gestures): Short / Double / Triple click are confirmed when the
multi-press window (default 250 ms) expires after the last release; a Long
click at the long-press duration; a Very long hold at 15 s.
Reported via multistate presentValue: 5 single, 6 double, 7 triple,
2 long press, 0 released.

Layer 2 (actions):
  - Short click     -> On/Off from the on/off flag (NV), flag flipped
  - Long click      -> smart dimming (normal mode only)
  - Double / Triple -> scene mode, set 1 (scenes 0-9) / set 2 (10-19)
  - in scene mode   -> Short click = next scene, Long click = exit,
                       exit after the lockout
  - 15 s hold       -> factory reset armed; Short click within 3 s = reset
"""

import pytest

from tests.conftest import Device, RelayButtonPair
from tests.zcl_consts import (
    ZCL_ATTR_MULTISTATE_INPUT_PRESENT_VALUE,
    ZCL_CLUSTER_LEVEL_CONTROL,
    ZCL_CLUSTER_MULTISTATE_INPUT_BASIC,
    ZCL_CLUSTER_ON_OFF,
    ZCL_CLUSTER_ON_OFF_SWITCH_CONFIG,
    ZCL_CMD_LEVEL_MOVE_TO_LEVEL_WITH_ON_OFF,
    ZCL_CMD_LEVEL_MOVE_WITH_ON_OFF,
    ZCL_CMD_LEVEL_STOP_WITH_ON_OFF,
    ZCL_CMD_ONOFF_OFF,
    ZCL_CMD_ONOFF_ON,
    ZCL_LEVEL_MOVE_DOWN,
    ZCL_LEVEL_MOVE_UP,
    ZCL_ONOFF_CONFIGURATION_BINDED_MODE_RISE,
    ZCL_ONOFF_CONFIGURATION_BINDED_MODE_SHORT,
    ZCL_ONOFF_CONFIGURATION_RELAY_MODE_DETACHED,
    ZCL_ONOFF_CONFIGURATION_SWITCH_TYPE_MOMENTARY,
)

ZCL_CLUSTER_SCENES = 0x0005
ZCL_CMD_SCENES_RECALL_SCENE = 0x05

ATTR_DIMMING_SYNC = 0xFF82
ATTR_MULTI_PRESS_WINDOW = 0xFF83
ATTR_SCENE_COUNT_SET1 = 0xFF84
ATTR_SCENE_GROUP_ID_SET1 = 0xFF85
ATTR_SCENE_LOCKOUT_SET1 = 0xFF86
ATTR_SCENE_COUNT_SET2 = 0xFF87
ATTR_SCENE_GROUP_ID_SET2 = 0xFF88
ATTR_SCENE_LOCKOUT_SET2 = 0xFF89

ZCL_ATTR_ONOFF = 0x0000
ZCL_ATTR_CURRENT_LEVEL = 0x0000
ZCL_TYPE_BOOLEAN = 0x10
ZCL_TYPE_UINT8 = 0x20

MS_RELEASED = 0
MS_LONG_PRESS = 2
MS_SINGLE = 5
MS_DOUBLE = 6
MS_TRIPLE = 7

WINDOW_MS = 250
CONFIRM_MS = WINDOW_MS + 10
# Default level move rate of the stub config (level units per second)
MOVE_RATE = 50


@pytest.fixture()
def gdev(device: Device, relay_button_pair: RelayButtonPair) -> Device:
    ep = relay_button_pair.switch_endpoint
    device.zcl_switch_mode_set(ep, ZCL_ONOFF_CONFIGURATION_SWITCH_TYPE_MOMENTARY)
    device.zcl_switch_relay_mode_set(ep, ZCL_ONOFF_CONFIGURATION_RELAY_MODE_DETACHED)
    device.zcl_switch_binding_mode_set(ep, ZCL_ONOFF_CONFIGURATION_BINDED_MODE_SHORT)
    device.clear_events()
    return device


# --- helpers ----------------------------------------------------------------


def sync(dev: Device) -> None:
    dev.step_time(10)
    dev.status()


def press(dev: Device, pair: RelayButtonPair) -> None:
    dev.press_button(pair.button_pin)  # steps 60 ms


def release(dev: Device, pair: RelayButtonPair) -> None:
    dev.release_button(pair.button_pin)  # steps 60 ms


def clicks(dev: Device, pair: RelayButtonPair, n: int, confirm: bool = True) -> None:
    """n quick clicks (60 ms down, 60 ms up), then wait out the window."""
    for _ in range(n):
        press(dev, pair)
        release(dev, pair)
    if confirm:
        dev.step_time(CONFIRM_MS)
    sync(dev)


def set_attr(dev: Device, pair: RelayButtonPair, attr: int, value: int) -> None:
    dev.write_zigbee_attr(
        pair.switch_endpoint, ZCL_CLUSTER_ON_OFF_SWITCH_CONFIG, attr, value
    )


def get_attr(dev: Device, pair: RelayButtonPair, attr: int) -> int:
    return int(
        dev.read_zigbee_attr(
            pair.switch_endpoint, ZCL_CLUSTER_ON_OFF_SWITCH_CONFIG, attr
        )
    )


def onoff_cmds(dev: Device, pair: RelayButtonPair) -> list[int]:
    sync(dev)
    return [c.cmd for c in dev.zcl_list_cmds(pair.switch_endpoint, ZCL_CLUSTER_ON_OFF)]


def recalls(dev: Device, pair: RelayButtonPair) -> list[tuple[int, int]]:
    """(group, scene) of every RecallScene sent."""
    sync(dev)
    res = []
    for c in dev.zcl_list_cmds(pair.switch_endpoint, ZCL_CLUSTER_SCENES):
        assert c.cmd == ZCL_CMD_SCENES_RECALL_SCENE
        res.append((c.data[0] | (c.data[1] << 8), c.data[2]))
    return res


def level_cmds(dev: Device, pair: RelayButtonPair) -> list:
    sync(dev)
    return dev.zcl_list_cmds(pair.switch_endpoint, ZCL_CLUSTER_LEVEL_CONTROL)


def multistate_reports(dev: Device, pair: RelayButtonPair) -> list[int]:
    """Multistate values sent to Z2M, in order: explicit reports (gestures)
    and changes picked up by the periodic reporting (long press)."""
    sync(dev)
    res = []
    for e in dev._events:
        if e.kind not in ("zcl_attr_change", "zcl_report"):
            continue
        if (
            int(e.payload["ep"]) == pair.switch_endpoint
            and int(e.payload["cluster"], 16) == ZCL_CLUSTER_MULTISTATE_INPUT_BASIC
            and int(e.payload["attr"], 16) == ZCL_ATTR_MULTISTATE_INPUT_PRESENT_VALUE
        ):
            res.append(int(e.payload["value"]))
    return res


def lamp(dev: Device, ep: int, src: int, on: bool, level: int | None = None) -> None:
    """Simulate one bound lamp (short address src) answering our Read."""
    res = dev.p.exec(
        f"zcl_read_rsp_src {src:04X} {ep} 0x{ZCL_CLUSTER_ON_OFF:04X} "
        f"0x{ZCL_ATTR_ONOFF:04X} 0x00 0x{ZCL_TYPE_BOOLEAN:02X} {1 if on else 0:02X}"
    )
    assert res.ok, res.payload
    if level is not None:
        res = dev.p.exec(
            f"zcl_read_rsp_src {src:04X} {ep} 0x{ZCL_CLUSTER_LEVEL_CONTROL:04X} "
            f"0x{ZCL_ATTR_CURRENT_LEVEL:04X} 0x00 0x{ZCL_TYPE_UINT8:02X} {level:02X}"
        )
        assert res.ok, res.payload


def hold(
    dev: Device,
    pair: RelayButtonPair,
    hold_ms: int,
    lamps: list[tuple[bool, int | None]] | None = None,
) -> None:
    """Press, let the lamps answer, hold for hold_ms from the press, release."""
    press(dev, pair)
    for i, (on, level) in enumerate(lamps or []):
        lamp(dev, pair.switch_endpoint, 0x1000 + i, on, level)
    # Step past the long press first, so the Move starts at the right time
    # (the simulator runs due tasks only after a whole step)
    dev.step_time(800)
    dev.step_time(hold_ms - 860)
    release(dev, pair)
    sync(dev)


def enter_scenes(dev: Device, pair: RelayButtonPair, n: int = 2) -> None:
    clicks(dev, pair, n)


# --- Layer 1: gesture detection and reports ---------------------------------


def test_single_click_waits_for_the_window(gdev: Device, relay_button_pair):
    pair = relay_button_pair
    press(gdev, pair)
    release(gdev, pair)
    gdev.step_time(WINDOW_MS - 80)
    assert onoff_cmds(gdev, pair) == []  # not confirmed yet
    gdev.step_time(100)
    assert onoff_cmds(gdev, pair) == [ZCL_CMD_ONOFF_ON]


def test_gestures_are_reported(gdev: Device, relay_button_pair):
    pair = relay_button_pair
    clicks(gdev, pair, 1)
    clicks(gdev, pair, 2)
    clicks(gdev, pair, 3)
    assert multistate_reports(gdev, pair) == [
        MS_SINGLE, MS_RELEASED, MS_DOUBLE, MS_RELEASED, MS_TRIPLE, MS_RELEASED
    ]


def test_press_alone_is_not_reported(gdev: Device, relay_button_pair):
    pair = relay_button_pair
    press(gdev, pair)
    release(gdev, pair)
    assert multistate_reports(gdev, pair) == []


def test_gestures_use_explicit_reports(gdev: Device, relay_button_pair):
    """On Telink the periodic reporting would miss a value that is set and
    reset right away, so gestures must be sent as explicit reports."""
    pair = relay_button_pair
    clicks(gdev, pair, 2)
    explicit = [
        int(e.payload["value"])
        for e in gdev._events
        if e.kind == "zcl_report"
        and int(e.payload["cluster"], 16) == ZCL_CLUSTER_MULTISTATE_INPUT_BASIC
    ]
    assert explicit == [MS_DOUBLE, MS_RELEASED]


def test_press_does_not_change_multistate(gdev: Device, relay_button_pair):
    """Telink reports every change of the value, so a press must not touch it."""
    pair = relay_button_pair
    press(gdev, pair)
    assert gdev.zcl_switch_get_multistate_value(pair.switch_endpoint) == "0"
    release(gdev, pair)
    assert gdev.zcl_switch_get_multistate_value(pair.switch_endpoint) == "0"
    gdev.step_time(CONFIRM_MS)
    sync(gdev)
    changes = [
        int(e.payload["value"])
        for e in gdev._events
        if e.kind == "zcl_attr_change"
        and int(e.payload["cluster"], 16) == ZCL_CLUSTER_MULTISTATE_INPUT_BASIC
    ]
    assert changes == []


def test_long_click_is_reported(gdev: Device, relay_button_pair):
    pair = relay_button_pair
    hold(gdev, pair, 1000)
    assert multistate_reports(gdev, pair) == [MS_LONG_PRESS, MS_RELEASED]


def test_four_presses_are_ignored(gdev: Device, relay_button_pair):
    pair = relay_button_pair
    set_attr(gdev, pair, ATTR_SCENE_COUNT_SET1, 3)
    set_attr(gdev, pair, ATTR_SCENE_COUNT_SET2, 3)
    gdev.clear_events()
    clicks(gdev, pair, 4)
    assert onoff_cmds(gdev, pair) == []
    assert recalls(gdev, pair) == []
    assert multistate_reports(gdev, pair) == []


def test_multi_press_window_is_configurable(gdev: Device, relay_button_pair):
    pair = relay_button_pair
    assert get_attr(gdev, pair, ATTR_MULTI_PRESS_WINDOW) == 250
    set_attr(gdev, pair, ATTR_MULTI_PRESS_WINDOW, 400)
    gdev.clear_events()
    press(gdev, pair)
    release(gdev, pair)
    gdev.step_time(300)
    press(gdev, pair)  # still within 400 ms: a double click
    release(gdev, pair)
    gdev.step_time(410)
    assert multistate_reports(gdev, pair) == [MS_DOUBLE, MS_RELEASED]


@pytest.mark.parametrize("value,expected", [(50, 100), (1000, 400), (300, 300)])
def test_multi_press_window_is_clamped(gdev: Device, relay_button_pair, value, expected):
    set_attr(gdev, relay_button_pair, ATTR_MULTI_PRESS_WINDOW, value)
    assert get_attr(gdev, relay_button_pair, ATTR_MULTI_PRESS_WINDOW) == expected


def test_other_configs_keep_old_behaviour(gdev: Device, relay_button_pair):
    """Binded mode "rise": command on the press edge, no gestures."""
    pair = relay_button_pair
    gdev.zcl_switch_binding_mode_set(
        pair.switch_endpoint, ZCL_ONOFF_CONFIGURATION_BINDED_MODE_RISE
    )
    gdev.clear_events()
    press(gdev, pair)
    assert len(onoff_cmds(gdev, pair)) == 1  # sent at once
    release(gdev, pair)


# --- Short click: On/Off from the flag ---------------------------------------


def test_short_clicks_alternate_on_off(gdev: Device, relay_button_pair):
    pair = relay_button_pair
    clicks(gdev, pair, 1)
    clicks(gdev, pair, 1)
    clicks(gdev, pair, 1)
    assert onoff_cmds(gdev, pair) == [ZCL_CMD_ONOFF_ON, ZCL_CMD_ONOFF_OFF, ZCL_CMD_ONOFF_ON]


def test_short_click_does_not_read_or_dim(gdev: Device, relay_button_pair):
    pair = relay_button_pair
    clicks(gdev, pair, 1)
    assert level_cmds(gdev, pair) == []


def _gesture_stub():
    """One switch (A0), one relay, set up for gestures; NV kept between runs."""
    from tests.client import StubProc

    proc = StubProc(device_config="A;B;SA0u;RB0;").start()
    dev = Device(proc)
    dev.zcl_switch_mode_set(1, ZCL_ONOFF_CONFIGURATION_SWITCH_TYPE_MOMENTARY)
    dev.zcl_switch_relay_mode_set(1, ZCL_ONOFF_CONFIGURATION_RELAY_MODE_DETACHED)
    dev.zcl_switch_binding_mode_set(1, ZCL_ONOFF_CONFIGURATION_BINDED_MODE_SHORT)
    dev.clear_events()
    return proc, dev


def test_flag_and_settings_survive_restart():
    pair = RelayButtonPair(relay_pin="B0", button_pin="A0", switch_endpoint=1, relay_endpoint=2)
    proc, dev = _gesture_stub()
    try:
        set_attr(dev, pair, ATTR_SCENE_COUNT_SET1, 4)
        set_attr(dev, pair, ATTR_SCENE_GROUP_ID_SET2, 0x1234)
        set_attr(dev, pair, ATTR_DIMMING_SYNC, 1)
        clicks(dev, pair, 1)  # On, flag = 1
        assert onoff_cmds(dev, pair) == [ZCL_CMD_ONOFF_ON]
    finally:
        proc.stop()

    proc, dev = _gesture_stub()
    try:
        assert get_attr(dev, pair, ATTR_SCENE_COUNT_SET1) == 4
        assert get_attr(dev, pair, ATTR_SCENE_GROUP_ID_SET2) == 0x1234
        dev.clear_events()
        clicks(dev, pair, 1)
        assert onoff_cmds(dev, pair) == [ZCL_CMD_ONOFF_OFF]  # flag was 1
    finally:
        proc.stop()


# --- Dimming Click -----------------------------------------------------------


def test_long_click_dims_like_before(gdev: Device, relay_button_pair):
    pair = relay_button_pair
    hold(gdev, pair, 1000, [(True, 200)])
    cmds = [c.cmd for c in level_cmds(gdev, pair)]
    assert cmds == [ZCL_CMD_LEVEL_MOVE_WITH_ON_OFF, ZCL_CMD_LEVEL_STOP_WITH_ON_OFF]
    assert level_cmds(gdev, pair)[0].data[0] == ZCL_LEVEL_MOVE_DOWN


def test_group_on_needs_60_percent(gdev: Device, relay_button_pair):
    """2 of 5 lamps on = off: all start from minimum and go up."""
    pair = relay_button_pair
    hold(gdev, pair, 1000, [(True, 200), (True, 200), (False, 10), (False, 10), (False, 10)])
    mtl = [c for c in level_cmds(gdev, pair) if c.cmd == ZCL_CMD_LEVEL_MOVE_TO_LEVEL_WITH_ON_OFF]
    assert len(mtl) == 1 and mtl[0].data[0] == 2


def test_direction_from_average_of_lit_lamps(gdev: Device, relay_button_pair):
    """4 of 5 on (80 %): average of the lit ones decides (here 80 % > 60 %)."""
    pair = relay_button_pair
    lamps = [(True, 203)] * 4 + [(False, 10)]
    hold(gdev, pair, 1000, lamps)
    move = [c for c in level_cmds(gdev, pair) if c.cmd == ZCL_CMD_LEVEL_MOVE_WITH_ON_OFF]
    assert move[0].data[0] == ZCL_LEVEL_MOVE_DOWN


def test_average_below_threshold_goes_up(gdev: Device, relay_button_pair):
    pair = relay_button_pair
    hold(gdev, pair, 1000, [(True, 200), (True, 20), (True, 20)])  # avg 80 = 31 %
    move = [c for c in level_cmds(gdev, pair) if c.cmd == ZCL_CMD_LEVEL_MOVE_WITH_ON_OFF]
    assert move[0].data[0] == ZCL_LEVEL_MOVE_UP


def test_late_answers_are_ignored(gdev: Device, relay_button_pair):
    """Answers after 200 ms do not count: no answer = legacy alternation."""
    pair = relay_button_pair
    press(gdev, pair)
    gdev.step_time(250)  # > 200 ms after the Read went out
    lamp(gdev, pair.switch_endpoint, 0x1000, False, 10)
    gdev.step_time(750)
    release(gdev, pair)
    mtl = [c for c in level_cmds(gdev, pair) if c.cmd == ZCL_CMD_LEVEL_MOVE_TO_LEVEL_WITH_ON_OFF]
    assert mtl == []  # the "lamp off" answer was not used


def test_dimming_sync_moves_all_to_average_first(gdev: Device, relay_button_pair):
    pair = relay_button_pair
    set_attr(gdev, pair, ATTR_DIMMING_SYNC, 1)
    gdev.clear_events()
    press(gdev, pair)
    lamp(gdev, pair.switch_endpoint, 0x1000, True, 220)
    lamp(gdev, pair.switch_endpoint, 0x1001, True, 180)
    gdev.step_time(900)   # Long click: MoveToLevel(average)
    gdev.step_time(260)   # then the Move
    release(gdev, pair)
    cmds = level_cmds(gdev, pair)
    assert cmds[0].cmd == ZCL_CMD_LEVEL_MOVE_TO_LEVEL_WITH_ON_OFF
    assert cmds[0].data == bytes([200, 0, 0])
    assert cmds[1].cmd == ZCL_CMD_LEVEL_MOVE_WITH_ON_OFF
    assert cmds[1].data[0] == ZCL_LEVEL_MOVE_DOWN  # 200 = 79 % > 60 %


def test_without_sync_no_move_to_level(gdev: Device, relay_button_pair):
    pair = relay_button_pair
    hold(gdev, pair, 1000, [(True, 220), (True, 180)])
    cmds = [c.cmd for c in level_cmds(gdev, pair)]
    assert ZCL_CMD_LEVEL_MOVE_TO_LEVEL_WITH_ON_OFF not in cmds


# --- on/off flag after dimming ------------------------------------------------


def test_dim_up_sets_flag_on(gdev: Device, relay_button_pair):
    """Lamps off -> dimmed up: the next Short click switches them off."""
    pair = relay_button_pair
    hold(gdev, pair, 1500, [(False, 50)])
    gdev.clear_events()
    clicks(gdev, pair, 1)
    assert onoff_cmds(gdev, pair) == [ZCL_CMD_ONOFF_OFF]


def test_short_dim_down_keeps_flag_on(gdev: Device, relay_button_pair):
    pair = relay_button_pair
    # Level 200 at 50/s: needs 4 s to reach the minimum; held ~1.2 s of Move
    hold(gdev, pair, 2000, [(True, 200)])
    gdev.clear_events()
    clicks(gdev, pair, 1)
    assert onoff_cmds(gdev, pair) == [ZCL_CMD_ONOFF_OFF]


def test_dim_down_to_minimum_sets_flag_off(gdev: Device, relay_button_pair):
    pair = relay_button_pair
    # Level 160 (63 %, above the threshold: down) at 50/s reaches the
    # minimum after 3.2 s of Move; held ~4.2 s of Move
    hold(gdev, pair, 5000, [(True, 160)])
    gdev.clear_events()
    clicks(gdev, pair, 1)
    assert onoff_cmds(gdev, pair) == [ZCL_CMD_ONOFF_ON]


def test_dim_down_flag_uses_60_percent_of_lamps(gdev: Device, relay_button_pair):
    """2 of 3 lamps reach the minimum: 1/3 still on < 60 % -> flag 0."""
    pair = relay_button_pair
    set_attr(gdev, pair, 0xFF80, 30)  # threshold 30 %: average 103 = 41 % -> down
    gdev.clear_events()
    hold(gdev, pair, 3000, [(True, 50), (True, 60), (True, 200)])
    move = [c for c in level_cmds(gdev, pair) if c.cmd == ZCL_CMD_LEVEL_MOVE_WITH_ON_OFF]
    assert move[0].data[0] == ZCL_LEVEL_MOVE_DOWN
    gdev.clear_events()
    clicks(gdev, pair, 1)
    assert onoff_cmds(gdev, pair) == [ZCL_CMD_ONOFF_ON]


# --- Scenes on Double / Triple -------------------------------------------------


def test_scene_set_off_by_default(gdev: Device, relay_button_pair):
    pair = relay_button_pair
    assert get_attr(gdev, pair, ATTR_SCENE_COUNT_SET1) == 0
    assert get_attr(gdev, pair, ATTR_SCENE_COUNT_SET2) == 0
    clicks(gdev, pair, 2)
    clicks(gdev, pair, 3)
    assert recalls(gdev, pair) == []
    # still reported by Layer 1
    assert multistate_reports(gdev, pair) == [MS_DOUBLE, MS_RELEASED, MS_TRIPLE, MS_RELEASED]
    # and still in normal mode: a click switches the lamps
    clicks(gdev, pair, 1)
    assert onoff_cmds(gdev, pair) == [ZCL_CMD_ONOFF_ON]


def test_double_click_recalls_and_cycles_set1(gdev: Device, relay_button_pair):
    pair = relay_button_pair
    set_attr(gdev, pair, ATTR_SCENE_COUNT_SET1, 3)
    set_attr(gdev, pair, ATTR_SCENE_GROUP_ID_SET1, 7)
    gdev.clear_events()
    enter_scenes(gdev, pair, 2)
    clicks(gdev, pair, 1)
    clicks(gdev, pair, 1)
    clicks(gdev, pair, 1)
    assert recalls(gdev, pair) == [(7, 0), (7, 1), (7, 2), (7, 0)]
    assert onoff_cmds(gdev, pair) == []  # clicks in scene mode do not switch


def test_triple_click_uses_set2(gdev: Device, relay_button_pair):
    pair = relay_button_pair
    set_attr(gdev, pair, ATTR_SCENE_COUNT_SET2, 2)
    set_attr(gdev, pair, ATTR_SCENE_GROUP_ID_SET2, 9)
    gdev.clear_events()
    enter_scenes(gdev, pair, 3)
    clicks(gdev, pair, 1)
    clicks(gdev, pair, 1)
    assert recalls(gdev, pair) == [(9, 10), (9, 11), (9, 10)]


def test_double_and_triple_have_no_effect_in_scene_mode(gdev: Device, relay_button_pair):
    pair = relay_button_pair
    set_attr(gdev, pair, ATTR_SCENE_COUNT_SET1, 3)
    set_attr(gdev, pair, ATTR_SCENE_COUNT_SET2, 3)
    gdev.clear_events()
    enter_scenes(gdev, pair, 2)
    clicks(gdev, pair, 2)
    clicks(gdev, pair, 3)
    assert recalls(gdev, pair) == [(0, 0)]


def test_lockout_exits_and_last_scene_is_kept(gdev: Device, relay_button_pair):
    pair = relay_button_pair
    set_attr(gdev, pair, ATTR_SCENE_COUNT_SET1, 3)
    set_attr(gdev, pair, ATTR_SCENE_LOCKOUT_SET1, 5)
    gdev.clear_events()
    enter_scenes(gdev, pair, 2)          # scene 0
    clicks(gdev, pair, 1)                # scene 1
    gdev.step_time(5100)                 # lockout: back to normal
    clicks(gdev, pair, 1)                # normal mode: switches the lamps
    assert onoff_cmds(gdev, pair) == [ZCL_CMD_ONOFF_OFF]  # flag 1 after recall
    enter_scenes(gdev, pair, 2)          # starts at the last scene
    assert recalls(gdev, pair) == [(0, 0), (0, 1), (0, 1)]


def test_click_restarts_lockout(gdev: Device, relay_button_pair):
    pair = relay_button_pair
    set_attr(gdev, pair, ATTR_SCENE_COUNT_SET1, 3)
    set_attr(gdev, pair, ATTR_SCENE_LOCKOUT_SET1, 5)
    gdev.clear_events()
    enter_scenes(gdev, pair, 2)
    gdev.step_time(4000)
    clicks(gdev, pair, 1)                # restarts the 5 s
    gdev.step_time(4000)                 # 8 s after entering, 4.4 s after click
    clicks(gdev, pair, 1)                # still in scene mode
    assert recalls(gdev, pair) == [(0, 0), (0, 1), (0, 2)]
    assert onoff_cmds(gdev, pair) == []


@pytest.mark.parametrize("value,expected", [(2, 5), (30, 15), (8, 8)])
def test_lockout_is_clamped(gdev: Device, relay_button_pair, value, expected):
    set_attr(gdev, relay_button_pair, ATTR_SCENE_LOCKOUT_SET2, value)
    assert get_attr(gdev, relay_button_pair, ATTR_SCENE_LOCKOUT_SET2) == expected


def test_scene_count_is_clamped(gdev: Device, relay_button_pair):
    set_attr(gdev, relay_button_pair, ATTR_SCENE_COUNT_SET1, 25)
    assert get_attr(gdev, relay_button_pair, ATTR_SCENE_COUNT_SET1) == 10


def test_long_click_exits_scene_mode_without_dimming(gdev: Device, relay_button_pair):
    pair = relay_button_pair
    set_attr(gdev, pair, ATTR_SCENE_COUNT_SET1, 3)
    gdev.clear_events()
    enter_scenes(gdev, pair, 2)
    hold(gdev, pair, 1000, [(True, 200)])
    assert level_cmds(gdev, pair) == []
    clicks(gdev, pair, 1)                # normal mode again
    assert onoff_cmds(gdev, pair) == [ZCL_CMD_ONOFF_OFF]


def test_scene_mode_sets_flag_on(gdev: Device, relay_button_pair):
    pair = relay_button_pair
    set_attr(gdev, pair, ATTR_SCENE_COUNT_SET2, 1)
    gdev.clear_events()
    enter_scenes(gdev, pair, 3)
    hold(gdev, pair, 1000)               # exit
    clicks(gdev, pair, 1)
    assert onoff_cmds(gdev, pair) == [ZCL_CMD_ONOFF_OFF]


# --- Factory reset (15 s hold) -------------------------------------------------


def very_long_hold(dev: Device, pair: RelayButtonPair) -> None:
    press(dev, pair)
    dev.step_time(900)       # Long click first (tasks run after each step)
    dev.step_time(14_200)    # 15 s from the press edge
    release(dev, pair)
    sync(dev)


def test_hold_15s_then_click_resets(gdev: Device, relay_button_pair):
    pair = relay_button_pair
    assert gdev.is_joined()
    very_long_hold(gdev, pair)
    gdev.step_time(1000)
    clicks(gdev, pair, 1)
    assert not gdev.is_joined()  # stub factory reset leaves the network


def test_hold_15s_sends_stop(gdev: Device, relay_button_pair):
    pair = relay_button_pair
    press(gdev, pair)
    lamp(gdev, pair.switch_endpoint, 0x1000, True, 200)
    gdev.step_time(900)
    gdev.step_time(13_100)
    stops = [c for c in level_cmds(gdev, pair) if c.cmd == ZCL_CMD_LEVEL_STOP_WITH_ON_OFF]
    assert stops == []
    gdev.step_time(1_100)
    stops = [c for c in level_cmds(gdev, pair) if c.cmd == ZCL_CMD_LEVEL_STOP_WITH_ON_OFF]
    assert len(stops) == 1
    release(gdev, pair)
    stops = [c for c in level_cmds(gdev, pair) if c.cmd == ZCL_CMD_LEVEL_STOP_WITH_ON_OFF]
    assert len(stops) == 1  # not sent twice


def test_hold_15s_without_click_does_nothing(gdev: Device, relay_button_pair):
    pair = relay_button_pair
    very_long_hold(gdev, pair)
    gdev.step_time(3100)
    clicks(gdev, pair, 1)                # after the window: a normal click
    assert gdev.is_joined()
    assert onoff_cmds(gdev, pair)[-1:] in ([ZCL_CMD_ONOFF_ON], [ZCL_CMD_ONOFF_OFF])


def test_hold_15s_then_double_click_cancels(gdev: Device, relay_button_pair):
    pair = relay_button_pair
    very_long_hold(gdev, pair)
    clicks(gdev, pair, 2)
    assert gdev.is_joined()
    gdev.clear_events()
    clicks(gdev, pair, 1)                # back to normal mode
    assert len(onoff_cmds(gdev, pair)) == 1
    assert gdev.is_joined()


def test_hold_15s_from_scene_mode_does_not_arm(gdev: Device, relay_button_pair):
    pair = relay_button_pair
    set_attr(gdev, pair, ATTR_SCENE_COUNT_SET1, 2)
    gdev.clear_events()
    enter_scenes(gdev, pair, 2)
    very_long_hold(gdev, pair)
    clicks(gdev, pair, 1)
    assert gdev.is_joined()


def test_ten_quick_presses_do_not_reset(gdev: Device, relay_button_pair):
    """The old "10 presses" reset is replaced by the 15 s hold."""
    pair = relay_button_pair
    for _ in range(4):
        clicks(gdev, pair, 3, confirm=False)
    sync(gdev)
    assert gdev.is_joined()
