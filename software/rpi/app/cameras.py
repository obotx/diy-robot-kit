"""Private-LAN camera settings and bounded, shared RTSP preview workers."""
from __future__ import annotations

import asyncio
import ipaddress
import json
import os
from pathlib import Path
import shutil
import subprocess
import threading
import time
from urllib.parse import quote, urlsplit
from uuid import uuid4

from fastapi import APIRouter, HTTPException, Request
from fastapi.responses import Response
from pydantic import BaseModel, ConfigDict, Field, SecretStr, field_validator

router = APIRouter(prefix="/api/cameras")
NAMES = ("Arm", "rear", "front left", "front right")
NETWORKS = tuple(ipaddress.ip_network(n) for n in ("10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16"))


class CameraSettings(BaseModel):
    model_config = ConfigDict(extra="forbid")
    name: str = Field(min_length=1, max_length=32)
    mode: str = "rtsp"
    host: str = ""
    port: int = Field(default=554, ge=1, le=65535)
    stream: int = Field(default=2, ge=1, le=2)
    username: str = Field(default="admin", max_length=64)
    password: SecretStr | None = None
    url: str = Field(default="", max_length=1024)

    @field_validator("mode")
    @classmethod
    def valid_mode(cls, value):
        if value not in {"rtsp", "mjpeg"}:
            raise ValueError("Unknown camera mode")
        return value

    @field_validator("host")
    @classmethod
    def private_host(cls, value):
        if not value:
            return value
        address = ipaddress.ip_address(value)
        if not any(address in network for network in NETWORKS):
            raise ValueError("Use a private LAN IPv4 address")
        return str(address)

    @field_validator("url")
    @classmethod
    def public_url(cls, value):
        if value:
            url = urlsplit(value)
            if url.scheme not in {"http", "https"} or not url.hostname or url.username or url.password:
                raise ValueError("Use an HTTP(S) MJPEG URL without credentials")
        return value

    def public(self):
        return self.model_dump(exclude={"password"}) | {"password_set": bool(self.password and self.password.get_secret_value())}


def ffmpeg_command(executable, camera):
    credentials = ""
    if camera.username:
        password = camera.password.get_secret_value() if camera.password else ""
        credentials = f"{quote(camera.username, safe='')}:{quote(password, safe='')}@"
    url = f"rtsp://{credentials}{camera.host}:{camera.port}/stream{camera.stream}"
    return [executable, "-hide_banner", "-loglevel", "error", "-nostdin",
            "-rtsp_transport", "tcp", "-timeout", "5000000", "-threads", "1",
            "-i", url, "-an", "-sn", "-dn", "-vf", "fps=20,scale=640:-2",
            "-threads", "1", "-filter_threads", "1", "-c:v", "mjpeg", "-q:v", "6",
            "-f", "image2pipe", "pipe:1"]


class Preview:
    def __init__(self, camera, executable):
        self.camera, self.executable = camera, executable
        self.lock = threading.Lock()
        self.stop = threading.Event()
        self.frame = None
        self.frame_at = 0.0
        self.request_at = time.monotonic()
        self.progress_at = self.request_at
        self.process = None
        self.status = "connecting"
        self.thread = threading.Thread(target=self.run, daemon=True, name="camera-preview")
        self.thread.start()

    def latest(self):
        with self.lock:
            self.request_at = time.monotonic()
            if self.frame is not None and self.request_at - self.frame_at < 2.0:
                return self.frame
            return None

    def kill(self):
        with self.lock:
            process = self.process
        if process and process.poll() is None:
            try:
                process.kill()
            except OSError:
                pass

    def run(self):
        while not self.stop.is_set():
            process = None
            try:
                kwargs = {"creationflags": subprocess.CREATE_NO_WINDOW} if os.name == "nt" else {}
                # Never inherit stderr: FFmpeg errors can contain the password URL.
                process = subprocess.Popen(ffmpeg_command(self.executable, self.camera),
                    stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, **kwargs)
                with self.lock:
                    self.process = process
                    self.progress_at = time.monotonic()
                if self.stop.is_set():
                    self.kill()
                    break
                buffer = bytearray()
                while not self.stop.is_set():
                    chunk = process.stdout.read1(16384)
                    if not chunk:
                        break
                    buffer.extend(chunk)
                    if len(buffer) > 2 * 1024 * 1024:
                        raise ValueError("Camera frame too large")
                    while True:
                        start = buffer.find(b"\xff\xd8")
                        end = buffer.find(b"\xff\xd9", max(0, start + 2))
                        if start < 0 or end < 0:
                            break
                        with self.lock:
                            self.frame = bytes(buffer[start:end + 2])
                            self.frame_at = time.monotonic()
                            self.progress_at = self.frame_at
                            self.status = "live"
                        del buffer[:end + 2]
            except (OSError, ValueError):
                pass
            finally:
                if process:
                    if process.poll() is None:
                        process.kill()
                    process.wait()
                    process.stdout.close()
                with self.lock:
                    self.process = None
                    self.frame = None
                    self.status = "offline"
            self.stop.wait(3)

    def close(self):
        self.stop.set()
        self.kill()
        self.thread.join(timeout=7)


class CameraService:
    def __init__(self, path=None):
        self.path = Path(path or os.environ.get("OBOTX_CAMERA_CONFIG", Path.home() / ".config/obotx/cameras.json"))
        self.lock = threading.Lock()
        self.cameras = [CameraSettings(name=name) for name in NAMES]
        if self.path.exists():
            data = json.loads(self.path.read_text(encoding="utf-8"))
            if len(data) != 4:
                raise ValueError("Camera configuration must have four slots")
            self.cameras = [CameraSettings.model_validate(item) for item in data]
        self.workers = {}
        self.executable = shutil.which("ffmpeg")
        self.stopping = threading.Event()
        self.janitor = threading.Thread(target=self.watch, daemon=True, name="camera-watch")
        self.janitor.start()

    def public(self):
        with self.lock:
            return {"cameras": [c.public() for c in self.cameras], "rtsp_available": bool(self.executable)}

    def save(self, index, camera):
        with self.lock:
            old = self.cameras[index]
            if camera.password is None:
                camera = camera.model_copy(update={"password": old.password if old.host == camera.host and old.username == camera.username else None})
            updated = self.cameras.copy()
            updated[index] = camera
            data = [c.model_dump(exclude={"password"}) | {"password": c.password.get_secret_value() if c.password else None} for c in updated]
            self.path.parent.mkdir(parents=True, exist_ok=True)
            temporary = self.path.with_name(self.path.name + "." + uuid4().hex + ".tmp")
            try:
                fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
                with os.fdopen(fd, "w", encoding="utf-8") as output:
                    json.dump(data, output)
                os.replace(temporary, self.path)
            finally:
                temporary.unlink(missing_ok=True)
            self.cameras = updated
            worker = self.workers.pop(index, None)
        if worker:
            worker.close()
        return self.public()

    def frame(self, index):
        with self.lock:
            camera = self.cameras[index]
            if camera.mode != "rtsp" or not camera.host:
                raise HTTPException(404, "Camera not configured")
            if not self.executable:
                raise HTTPException(503, "FFmpeg is not installed on the server")
            if index not in self.workers:
                self.workers[index] = Preview(camera, self.executable)
            worker = self.workers[index]
            frame = worker.latest()
        if frame is None:
            raise HTTPException(503, "Camera connecting or offline; check IP, credentials and RTSP")
        return frame

    def watch(self):
        while not self.stopping.wait(1):
            idle = []
            with self.lock:
                for index, worker in list(self.workers.items()):
                    with worker.lock:
                        inactive = time.monotonic() - worker.request_at > 10
                        stalled = time.monotonic() - worker.progress_at > 10
                    if inactive:
                        idle.append(self.workers.pop(index))
                    elif stalled:
                        worker.kill()
            for worker in idle:
                worker.close()

    def close(self):
        self.stopping.set()
        self.janitor.join(timeout=8)
        with self.lock:
            workers = list(self.workers.values())
            self.workers.clear()
        for worker in workers:
            worker.close()


def slot(index):
    if not 0 <= index < 4:
        raise HTTPException(404, "Unknown camera")


@router.get("")
async def cameras(request: Request):
    return await asyncio.to_thread(request.app.state.cameras.public)


@router.put("/{index}")
async def save_camera(index: int, camera: CameraSettings, request: Request):
    slot(index)
    origin = request.headers.get("origin")
    if request.headers.get("x-obotx-camera") != "1" or (origin and origin != str(request.base_url).rstrip("/")):
        raise HTTPException(403, "Same-origin camera settings required")
    try:
        return await asyncio.to_thread(request.app.state.cameras.save, index, camera)
    except OSError:
        raise HTTPException(503, "Unable to save camera configuration") from None


@router.get("/{index}/frame")
async def camera_frame(index: int, request: Request):
    slot(index)
    data = await asyncio.to_thread(request.app.state.cameras.frame, index)
    return Response(data, media_type="image/jpeg", headers={"Cache-Control": "no-store"})
