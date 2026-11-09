import asyncio
import json
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from Crypto.Cipher import AES
from Crypto.Util import Counter
from fastapi import FastAPI
from fastapi.testclient import TestClient
from victron_ble.devices.battery_monitor import BatteryMonitor as Parser

from app.battery import BatteryConfig, BatteryMonitor, router

KEY = "00112233445566778899aabbccddeeff"
MAC = "AA:BB:CC:DD:EE:FF"


def frame(soc=600, counter=1, remaining=120):
    # Battery-monitor fields packed LSB-first, then encrypted with the fixture key.
    fields = [(remaining, 16), (2560, 16), (0, 16), (0, 16), (3, 2),
              ((1 << 22) - 2500, 22), (50, 20), (soc, 10)]
    packed, shift = 0, 0
    for value, width in fields:
        packed |= value << shift
        shift += width
    cipher = AES.new(bytes.fromhex(KEY), AES.MODE_CTR,
                     counter=Counter.new(128, initial_value=counter, little_endian=True))
    return bytes.fromhex("101002a302") + counter.to_bytes(2, "little") + b"\x00" + cipher.encrypt(packed.to_bytes(15, "little"))


class BatteryTests(unittest.TestCase):
    def setUp(self):
        self.now = 0
        config = json.dumps(dict(address=MAC, advertisement_key=KEY))
        with patch.object(Path, "read_text", return_value=config):
            self.monitor = BatteryMonitor(clock=lambda: self.now)
        self.parser = Parser(KEY)

    def receive(self, soc=600, counter=1, address=MAC, raw=None):
        self.monitor.receive(SimpleNamespace(address=address),
            SimpleNamespace(manufacturer_data={0x02E1: raw if raw is not None else frame(soc, counter)}), self.parser)
        return self.monitor.snapshot()

    def test_real_parser_units_and_secret_redaction(self):
        data = self.receive()
        self.assertEqual(data["soc_pct"], 60)
        self.assertEqual(data["voltage_v"], 25.6)
        self.assertEqual(data["current_a"], -2.5)
        self.assertEqual(data["remaining_minutes"], 120)
        self.assertEqual(data["level"], "normal")
        self.assertNotIn(KEY, json.dumps(data))
        self.assertNotIn(KEY, repr(self.monitor.config))

    def test_thresholds_and_hysteresis_including_zero(self):
        for i, (soc, expected) in enumerate([(200, "low"), (50, "critical"),
            (0, "critical"), (60, "critical"), (70, "low"), (210, "low"), (220, "normal")]):
            self.assertEqual(self.receive(soc, i)["level"], expected)

    def test_remaining_unknown_zero_and_stale(self):
        self.assertIsNone(self.receive(raw=frame(remaining=65535))["remaining_minutes"])
        self.assertEqual(self.receive(raw=frame(counter=2, remaining=0))["remaining_minutes"], 0)
        self.now = 16
        self.assertIsNone(self.monitor.snapshot()["remaining_minutes"])

    def test_stale_duplicates_and_recovery(self):
        self.receive(50)
        self.now = 16
        data = self.receive(50)
        self.assertEqual(data["status"], "stale")
        self.assertIsNone(data["soc_pct"])
        self.assertEqual(data["last_known_level"], "critical")
        self.assertTrue(self.receive(500, 2)["fresh"])

    def test_unsynchronized_is_not_zero(self):
        data = self.receive(0x3ff)
        self.assertEqual(data["status"], "soc_unknown")
        self.assertIsNone(data["soc_pct"])
        self.assertEqual(data["level"], "unknown")

    def test_other_devices_and_short_frames_are_ignored(self):
        self.assertEqual(self.receive(address="00:11:22:33:44:55")["status"], "waiting")
        self.assertEqual(self.receive(raw=b"short")["status"], "waiting")

    def test_wrong_key_marks_data_unavailable(self):
        self.receive()
        raw = bytearray(frame(counter=2))
        raw[7] = 255
        data = self.receive(raw=bytes(raw))
        self.assertEqual(data["status"], "decode_error")
        self.assertFalse(data["fresh"])
        self.assertIsNone(data["soc_pct"])

    def test_invalid_config_and_missing_config_do_not_crash_server(self):
        with patch.object(Path, "read_text", side_effect=FileNotFoundError):
            self.assertEqual(BatteryMonitor().snapshot()["status"], "not_configured")
        with patch.object(Path, "read_text", return_value="invalid"):
            self.assertEqual(BatteryMonitor().snapshot()["status"], "configuration_error")
        for extra in ({"advertisement_key": "bad"}, {"address": "bad"},
                      {"low_pct": 10, "critical_pct": 15}):
            with self.assertRaises(ValueError):
                BatteryConfig(**(dict(address=MAC, advertisement_key=KEY) | extra))

    def test_read_only_api_and_no_key(self):
        app = FastAPI()
        app.state.battery = self.monitor
        app.include_router(router)
        with TestClient(app) as client:
            self.receive(50)
            response = client.get("/api/battery")
            self.assertEqual(response.status_code, 200)
            self.assertEqual(response.json()["level"], "critical")
            self.assertNotIn(KEY, response.text)
            self.assertEqual(client.post("/api/battery").status_code, 405)

    def test_scanner_retries_without_leaking_key_and_closes(self):
        calls = []
        monitor = self.monitor
        class Scanner:
            def __init__(self, **kwargs):
                calls.append(kwargs)
                self.kwargs = kwargs

            async def __aenter__(self):
                if len(calls) == 1:
                    raise RuntimeError("adapter unavailable")
                self.kwargs["detection_callback"](SimpleNamespace(address=MAC),
                    SimpleNamespace(manufacturer_data={0x02E1: frame()}))
                monitor.stop_event.set()
                return self

            async def __aexit__(self, *args):
                calls.append("closed")

        async def fast_sleep(_):
            pass

        monitor.scanner_factory = Scanner
        with patch("app.battery.asyncio.sleep", fast_sleep):
            asyncio.run(monitor._scan())
        self.assertEqual(len(calls), 3)
        self.assertEqual(calls[1]["bluez"], {"adapter": "hci0"})
        self.assertEqual(calls[2], "closed")
        self.assertEqual(monitor.snapshot()["status"], "live")
