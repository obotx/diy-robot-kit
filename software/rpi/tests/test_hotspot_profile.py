import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location(
    "hotspot", Path(__file__).resolve().parents[1] / "systemd" / "prepare-hotspot.py")
hotspot = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(hotspot)


class HotspotProfileTests(unittest.TestCase):
    def test_profile_starts_disabled_and_uses_separate_subnet(self):
        result = hotspot.profile("TestPassword1234")
        for value in ("autoconnect=false", "mode=ap", "method=shared",
                      "address1=192.168.60.1/24", "key-mgmt=wpa-psk", "proto=rsn;"):
            self.assertIn(value, result)
        self.assertNotIn("192.168.50.", result)

    def test_rejects_bad_passwords_and_keyfile_injection(self):
        for password in ("short", "a" * 64, "abcdefghijk\n", "abcdefghijk;", "abcdefghijk "):
            with self.subTest(password=password), self.assertRaises(ValueError):
                hotspot.profile(password)

    def test_unsupported_adapter_does_not_prompt_or_write(self):
        with patch.object(hotspot.os, "geteuid", return_value=0, create=True), \
             patch.object(hotspot.subprocess, "check_output", return_value="no\n"), \
             patch.object(hotspot.getpass, "getpass") as prompt, \
             patch.object(hotspot.tempfile, "mkstemp") as write:
            with self.assertRaises(SystemExit):
                hotspot.main()
            prompt.assert_not_called()
            write.assert_not_called()
