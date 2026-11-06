"""Boot the simulator with the BSEED _TZ3210_5ct6e7ye discovery config_str.

Guards the compiled-in default of OUTLET_BSEED_PM_TS011F_5ct6e7ye: the string
must parse on a router, bring up the relay and the HLW8012 metering cluster,
and contain no switch (so no multi-press factory reset during pin discovery).
"""

from pathlib import Path

import pytest
import yaml

from conftest import Device

ZCL_CLUSTER_ON_OFF = 0x0006
ZCL_CLUSTER_ELECTRICAL_MEASUREMENT = 0x0B04
ZCL_ATTR_ON_OFF = 0x0000
ZCL_ATTR_ELEC_MEAS_RMS_VOLTAGE = 0x0505

BOARD = "OUTLET_BSEED_PM_TS011F_5ct6e7ye"
DB_FILE = Path(__file__).resolve().parent.parent / "device_db.yaml"


def _db_config_str() -> str:
    db = yaml.safe_load(DB_FILE.read_text())
    return db[BOARD]["config_str"]


@pytest.fixture
def device_config() -> str:
    return _db_config_str()


def test_db_entry_has_no_switch_token():
    peripherals = _db_config_str().rstrip(";").split(";")[2:]
    assert not any(p.startswith("S") for p in peripherals)


def test_relay_toggles_from_zigbee(device: Device):
    device.call_zigbee_cmd(1, ZCL_CLUSTER_ON_OFF, 1)  # On
    assert device.get_gpio("D2", refresh=True)
    assert device.read_zigbee_attr(1, ZCL_CLUSTER_ON_OFF, ZCL_ATTR_ON_OFF) == "1"
    device.call_zigbee_cmd(1, ZCL_CLUSTER_ON_OFF, 0)  # Off
    assert not device.get_gpio("D2", refresh=True)


def test_metering_cluster_present(device: Device):
    assert (
        device.read_zigbee_attr(
            1, ZCL_CLUSTER_ELECTRICAL_MEASUREMENT, ZCL_ATTR_ELEC_MEAS_RMS_VOLTAGE
        )
        is not None
    )
