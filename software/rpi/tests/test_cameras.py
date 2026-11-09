import json
from pathlib import Path
import shutil
import unittest
import threading
import time
from unittest.mock import Mock, patch
from uuid import uuid4

from fastapi import FastAPI, HTTPException
from fastapi.testclient import TestClient
from pydantic import ValidationError

from app.cameras import CameraSettings, CameraService, Preview, ffmpeg_command, router


class CameraTests(unittest.TestCase):
    def setUp(self):
        self.folder = Path.cwd() / ("tmp-camera-test-" + uuid4().hex)
        self.folder.mkdir()
        self.service = CameraService(self.folder / "settings.json")
        self.addCleanup(lambda: shutil.rmtree(self.folder))
        self.addCleanup(self.service.close)
        app = FastAPI()
        app.state.cameras = self.service
        app.include_router(router)
        self.client = TestClient(app)
        self.addCleanup(self.client.close)

    def camera(self, **changes):
        return CameraSettings(name="Arm", host="192.168.100.21", password="test-secret", **changes)

    def test_persistence_and_redaction(self):
        self.service.save(0, self.camera())
        response = self.client.get("/api/cameras")
        self.assertNotIn("test-secret", response.text)
        self.assertTrue(response.json()["cameras"][0]["password_set"])
        other = CameraService(self.service.path)
        try:
            self.assertEqual(other.cameras[0].password.get_secret_value(), "test-secret")
            self.assertEqual(other.cameras[0].host, "192.168.100.21")
        finally:
            other.close()

    def test_password_preserved_only_for_same_identity(self):
        self.service.save(0, self.camera())
        self.service.save(0, CameraSettings(name="Renamed", host="192.168.100.21"))
        self.assertTrue(self.service.public()["cameras"][0]["password_set"])
        self.service.save(0, CameraSettings(name="Other", host="192.168.100.22"))
        self.assertFalse(self.service.public()["cameras"][0]["password_set"])

    def test_private_literal_ip_and_credential_free_direct_url(self):
        for host in ("127.0.0.1", "169.254.169.254", "8.8.8.8", "localhost", "192.168.1.1/path", "::1"):
            with self.subTest(host=host), self.assertRaises(ValidationError):
                CameraSettings(name="Arm", host=host)
        with self.assertRaises(ValidationError):
            CameraSettings(name="Arm", url="http://admin:secret@192.168.1.1/a")

    def test_save_requires_header_and_same_origin(self):
        body = {"name": "Arm", "host": "192.168.100.21", "password": "test-secret"}
        self.assertEqual(self.client.put("/api/cameras/0", json=body).status_code, 403)
        headers = {"X-ObotX-Camera": "1", "Origin": "http://evil.invalid"}
        self.assertEqual(self.client.put("/api/cameras/0", json=body, headers=headers).status_code, 403)
        headers["Origin"] = "http://testserver"
        response = self.client.put("/api/cameras/0", json=body, headers=headers)
        self.assertEqual(response.status_code, 200)
        self.assertNotIn("test-secret", response.text)
        self.assertEqual(self.client.get("/api/cameras/4/frame").status_code, 404)

    def test_missing_ffmpeg_and_unconfigured(self):
        self.assertEqual(self.client.get("/api/cameras/0/frame").status_code, 404)
        self.service.save(0, self.camera())
        self.service.executable = None
        self.assertEqual(self.client.get("/api/cameras/0/frame").status_code, 503)

    def test_shared_worker_and_reconfigure_closes_old_source(self):
        self.service.save(0, self.camera())
        self.service.executable = "ffmpeg"
        with patch("app.cameras.Preview") as factory:
            factory.return_value.latest.return_value = b"\xff\xd8jpeg\xff\xd9"
            first = self.client.get("/api/cameras/0/frame")
            second = self.client.get("/api/cameras/0/frame")
            self.assertEqual(first.status_code, 200)
            self.assertEqual(first.content, second.content)
            self.assertEqual(first.headers["cache-control"], "no-store")
            factory.assert_called_once()
            self.service.save(0, CameraSettings(name="Arm"))
            factory.return_value.close.assert_called_once()

    def test_ffmpeg_no_shell_and_escaped_auth(self):
        c = CameraSettings(name="Arm", host="192.168.100.21", username="a@b", password="x:y/#")
        args = ffmpeg_command("ffmpeg", c)
        self.assertIn("rtsp://a%40b:x%3Ay%2F%23@192.168.100.21:554/stream2", args)
        self.assertEqual(args[args.index("-rtsp_transport") + 1], "tcp")
        self.assertEqual(args[args.index("-vf") + 1], "fps=20,scale=640:-2")

    def test_worker_frame_parsing_staleness_and_shutdown(self):
        ended = threading.Event()
        chunks = iter([b'noise\xff', b'\xd8image', b'\xff\xd9'])
        process = Mock()
        def read(count):
            value = next(chunks, None)
            if value is not None:
                return value
            ended.wait(3)
            return b''
        process.stdout.read1.side_effect = read
        process.poll.side_effect = lambda: 0 if ended.is_set() else None
        process.kill.side_effect = ended.set
        with patch('app.cameras.subprocess.Popen', return_value=process) as popen:
            worker = Preview(self.camera(), 'ffmpeg')
            try:
                deadline = time.monotonic() + 2
                frame = None
                while frame is None and time.monotonic() < deadline:
                    frame = worker.latest()
                    time.sleep(.01)
                self.assertEqual(frame, b'\xff\xd8image\xff\xd9')
                with worker.lock:
                    worker.frame_at -= 3
                self.assertIsNone(worker.latest())
                self.assertNotIn('shell', popen.call_args.kwargs)
            finally:
                worker.close()
            self.assertFalse(worker.thread.is_alive())
            process.stdout.close.assert_called_once()


if __name__ == "__main__":
    unittest.main()
