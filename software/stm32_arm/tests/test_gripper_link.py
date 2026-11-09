import unittest
import test_link


class GripperTests(unittest.TestCase):
    setUp = test_link.LinkTests.setUp
    tearDown = test_link.LinkTests.tearDown
    packet = test_link.LinkTests.packet
    send = test_link.LinkTests.send

    def test_gripper_commands_and_stop(self):
        for value in (0.25, -0.25, 0.0):
            packet = self.packet(command=dict(zl_mps=0, zr_mps=0, x_mps=0, gripper=value))
            reply = self.send(packet)
            self.assertTrue(reply['accepted'])
            self.assertIsNone(reply['arm']['gripper_pct'])
        reply = self.send(self.packet(enabled=False,
            command=dict(zl_mps=0, zr_mps=0, x_mps=0, gripper=0)))
        self.assertTrue(reply['accepted'])
        self.assertFalse(reply['enabled'])
