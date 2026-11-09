"""Real portable C parser/planner, synthetic host plant, actual Python gateway."""
import os
import unittest
from unittest.mock import patch
from test_link import Port
from app.robot import ZERO, Home, HomeKeepalive, ArmMove, ClearFault, Stop, Enable, ControlError
from app.stm32_protocol import SerialPeer, LinkError, encode_frame
from app.stm32 import SerialGateway, STM32Robot
from app.stm32_config import STM32Config


class ActionTests(unittest.TestCase):
    def test_z_move_rejects_200_mm_s(self):
        self.peer.arm_action("home", axis="ZL")
        self.finish()
        with self.assertRaises(LinkError):
            self.peer.arm_action("move", axis="ZL", position_um=400000, velocity_um_s=200000)

    def setUp(self):
        self.env = patch.dict(os.environ, ARM_LINK_FIXTURE=os.environ["ARM_ACTION_ZL_FIXTURE"])
        self.env.start()
        self.addCleanup(self.env.stop)
        self.port=Port()
        self.addCleanup(self.port.close)
        self.peer=SerialPeer(self.port,"arm","ARM-TEST","a"*32)
        self.peer.hello()
        self.port.command("unhome")

    def finish(self):
        for _ in range(200):
            self.port.command("plant 100")
            state=self.peer.arm_action("keepalive")
            if state.arm.active_axis is None:
                self.assertFalse(state.enabled)
                return state
        self.fail("Operation did not complete")

    def test_home_move_configure_and_real_ready(self):
        state=self.peer.setpoint(False,ZERO)
        self.assertFalse(state.ready)
        state=self.peer.arm_action("home",axis="ZL")
        self.assertTrue(state.enabled)
        self.assertEqual(state.arm.mode,3)
        state=self.finish()
        self.assertTrue(state.ready)
        self.assertTrue(state.arm.homed_axes["ZL"])
        self.assertFalse(state.arm.homed_axes["ZR"])
        self.assertIsNone(state.arm.z_right_mm)
        self.peer.arm_action("configure",axis="ZL",min_um=0,max_um=650000,velocity_um_s=190000)
        self.peer.arm_action("move",axis="ZL",position_um=20000,velocity_um_s=190000)
        state=self.finish()
        self.assertAlmostEqual(state.arm.z_left_mm,20,delta=.1)
        state=self.peer.arm_action("keepalive")
        self.assertIsNone(state.arm.active_axis)
        self.assertFalse(state.enabled)

    def test_watchdog_does_not_resume_after_hello(self):
        self.peer.arm_action("home",axis="ZL")
        status=self.port.command("plant 251")
        self.assertFalse(status["enabled"])
        self.assertTrue(status["expired"])
        self.peer.hello()
        state=self.peer.setpoint(False,ZERO)
        self.assertFalse(state.ready)
        self.assertIsNone(state.arm.active_axis)

    def test_move_uses_explicit_acceleration_and_rejects_out_of_range(self):
        self.peer.arm_action("home", axis="ZL")
        self.finish()
        self.peer.arm_action("move", axis="ZL", position_um=400000,
                             velocity_um_s=190000, acceleration_um_s2=10000)
        self.assertEqual(self.port.command("state")["accel"][0], 10000)
        self.peer.arm_action("stop")
        self.peer.arm_action("move", axis="ZL", position_um=20000,
                             velocity_um_s=190000)
        self.assertEqual(self.port.command("state")["accel"][0], 10000)
        self.peer.arm_action("stop")
        for bad in (0, 4999, 100001, 10000.5):
            self.peer.hello()
            request = self.peer.request("arm_action", action="move", axis="ZL",
                                       position_um=400000, velocity_um_s=190000,
                                       acceleration_um_s2=bad)
            self.assertFalse(self.port.raw(encode_frame(request))["accepted"])
            self.assertEqual(self.port.command("state")["accel"][0], 10000)

    def test_long_moves_both_directions_at_190_mm_s(self):
        self.peer.arm_action("home", axis="ZL")
        self.finish()
        for acceleration in (10000, 100000):
            for target in (600000, 0, 400000, 10000):
                with self.subTest(acceleration=acceleration, target=target):
                    self.peer.arm_action("move", axis="ZL", position_um=target,
                                         velocity_um_s=190000, acceleration_um_s2=acceleration)
                    previous = 0
                    for _ in range(200):
                        debug = self.port.command("plant 100")
                        speed = abs(debug["velocity"][0])
                        self.assertLessEqual(abs(speed - previous), acceleration / 10 + 200)
                        previous = speed
                        state = self.peer.arm_action("keepalive")
                        if state.arm.active_axis is None:
                            break
                    else:
                        self.fail("Long MOVE did not complete")
                    self.assertAlmostEqual(state.arm.z_left_mm, target / 1000, delta=.002)
                    self.assertEqual(state.arm.fault_code, 0)

    def test_stop_clear_and_rejected_inactive_axis(self):
        self.peer.arm_action("home",axis="ZL")
        self.assertFalse(self.peer.arm_action("stop").enabled)
        self.port.command("fault")
        self.assertIsNotNone(self.peer.setpoint(False,ZERO).fault)
        self.assertIsNone(self.peer.arm_action("clear_fault").fault)
        with self.assertRaises(LinkError):
            self.peer.arm_action("home",axis="X")
        self.peer.hello()
        with self.assertRaises(LinkError):
            self.peer.arm_action("move",axis="ZL",position_um=20000,velocity_um_s=10000)

    def test_replay_and_malformed_action_stop(self):
        data=self.peer.request("arm_action",action="home",axis="ZL")
        first=self.port.raw(encode_frame(data))
        self.assertTrue(first["accepted"])
        self.assertFalse(self.port.raw(encode_frame(data))["accepted"])
        self.peer.hello()
        bad=self.peer.request("arm_action",action="home",axis="ZL",position_um=10)
        self.assertFalse(self.port.raw(encode_frame(bad))["accepted"])


class GatewayActionTests(unittest.TestCase):
    def setUp(self):
        self.now=0.0
        self.env=patch.dict(os.environ,ARM_LINK_FIXTURE=os.environ["ARM_ACTION_ZL_FIXTURE"])
        self.env.start(); self.addCleanup(self.env.stop)
        self.port=Port()
        cfg=STM32Config(arm_only=True,arm={"port":"COM_TEST","device_id":"ARM-TEST"},commissioned=True,max_z_difference_mm=100.0)
        self.gateway=SerialGateway(cfg,opener=lambda *args:self.port,clock=lambda:self.now)
        self.gateway._connect()
        self.port.command("unhome")
        self.gateway._cycle()
        self.robot=STM32Robot(self.gateway,clock=lambda:self.now)
        self.addCleanup(self.gateway._fail,"test_complete")

    def test_web_messages_home_then_position_and_cancel(self):
        self.robot.apply("web",Home(type="home",seq=1,axis="ZL"))
        self.gateway._cycle()
        for seq in range(2,160):
            self.now+=.1
            self.port.command("plant 100")
            self.robot.apply("web",HomeKeepalive(type="home_keepalive",seq=seq))
            self.gateway._cycle()
            self.robot.tick()
            if self.robot.owner is None:
                break
        self.assertIsNone(self.robot.owner)
        self.gateway._cycle()
        self.assertTrue(self.robot.snapshot()["control_ready"])
        self.robot.apply("web",ArmMove(type="arm_move",seq=200,axis="ZL",position_um=20000,velocity_um_s=190000,acceleration_um_s2=10000))
        self.gateway._cycle()
        self.assertEqual(self.port.command("state")["accel"][0], 10000)
        self.assertEqual(self.gateway.states["arm"]["arm"]["active_axis"],"ZL")
        self.robot.apply("observer",Stop(type="stop",seq=1))
        self.gateway._cycle()
        self.assertFalse(self.gateway.states["arm"]["enabled"])
        with self.assertRaises(ControlError):
            self.robot.apply("web",HomeKeepalive(type="home_keepalive",seq=201))

    def test_worker_lease_expires_without_web_tick(self):
        self.robot.apply("web",Home(type="home",seq=1,axis="ZL"))
        self.gateway._cycle()
        from app.robot import WATCHDOG_SECONDS
        self.now = WATCHDOG_SECONDS + .001
        with self.assertRaisesRegex(LinkError,"operator_lease_expired"):
            self.gateway._cycle()
        self.gateway._fail("operator_lease_expired")
        self.assertIsNone(self.gateway.action)
        self.assertFalse(self.gateway.connected)


class FullArmActionTests(unittest.TestCase):
    def test_paired_home_x_home_and_paired_move(self):
        port=Port()
        self.addCleanup(port.close)
        peer=SerialPeer(port,"arm","ARM-TEST","b"*32)
        peer.hello(); port.command("unhome")
        for axis in ("Z","X"):
            peer.arm_action("home",axis=axis)
            for _ in range(400):
                port.command("plant 100")
                state=peer.arm_action("keepalive")
                if state.arm.active_axis is None:
                    break
            self.assertFalse(state.enabled)
            self.assertTrue(state.arm.homed_axes["X" if axis=="X" else "ZL"])
        self.assertTrue(state.ready)
        peer.arm_action("move",axis="Z",position_um=20000,velocity_um_s=190000)
        for _ in range(100):
            port.command("plant 100")
            state=peer.arm_action("keepalive")
            if state.arm.active_axis is None:
                break
        self.assertFalse(state.enabled)
        self.assertAlmostEqual(state.arm.z_left_mm,20,delta=.1)
        self.assertAlmostEqual(state.arm.z_right_mm,20,delta=.1)
