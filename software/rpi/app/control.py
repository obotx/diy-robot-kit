from __future__ import annotations

import asyncio
import logging
import json
from contextlib import suppress
from urllib.parse import urlsplit
from uuid import uuid4

from fastapi import APIRouter, Request, WebSocket, WebSocketDisconnect
from pydantic import ValidationError

from .robot import COMMAND, ControlError, ServiceUnlock, Enable, WATCHDOG_SECONDS
from .control_session import ControlSession, ControlSessions


router = APIRouter()
LOG = logging.getLogger(__name__)


@router.get("/api/robot")
async def robot_status(request: Request):
    return request.app.state.robot.snapshot()


@router.websocket("/ws/control")
async def control_socket(ws: WebSocket):
    # Native PC clients omit Origin; browsers must use this server's origin.
    origin = ws.headers.get("origin")
    expected_scheme = "https" if ws.url.scheme == "wss" else "http"
    if origin:
        parsed = urlsplit(origin)
        if parsed.scheme != expected_scheme or parsed.netloc != ws.headers.get("host"):
            await ws.close(code=1008)
            return
    await ws.accept()
    robot = ws.app.state.robot
    client_id = uuid4().hex
    if not hasattr(ws.app.state, "control_sessions"):
        ws.app.state.control_sessions = ControlSessions()
    sessions = ws.app.state.control_sessions
    session = ControlSession(client_id)
    sessions.add(session)
    resumable = False
    last_seq = -1
    send_lock = asyncio.Lock()

    async def send(data):
        async with send_lock:
            await asyncio.wait_for(ws.send_json(data), timeout=1.0)

    def check_pause():
        robot.tick()
        if resumable and session.wanted and robot.owner is None:
            if robot.reason == "watchdog":
                if not session.paused:
                    session.generation = robot.control_generation
                session.pause()
            elif not session.paused:
                session.wanted = False

    async def welcome():
        await send({"type": "welcome", "protocol": 1, "client_id": client_id,
                    "watchdog_ms": int(WATCHDOG_SECONDS * 1000),
                    "resume_token": session.token, "lease": session.lease,
                    "paused": session.paused, "wanted": session.wanted})

    async def publish():
        while True:
            check_pause()
            snapshot = robot.snapshot()
            if resumable:
                snapshot["control_session"] = {"paused": session.paused,
                    "lease": session.lease, "wanted": session.wanted}
            access = getattr(ws.app.state, "service_access", None)
            snapshot["service_password_configured"] = bool(access and access.record)
            await send(snapshot)
            await asyncio.sleep(0.1)

    async def receive():
        nonlocal last_seq, session, client_id, resumable
        while True:
            raw = await ws.receive_text()
            try:
                if len(raw) > 4096:
                    raise ControlError("message_too_large")
                envelope = json.loads(raw)
                if not isinstance(envelope, dict):
                    raise ControlError("invalid_command")
                if envelope.get("type") == "session_resume":
                    if last_seq != -1 or resumable:
                        raise ControlError("resume_must_be_first")
                    previous = sessions.items.get(envelope.get("token"))
                    if previous and previous.connected:
                        # The old TCP connection may not yet have noticed the outage.
                        await send({"type": "session_retry"})
                        continue
                    resumed = None
                    service_owner = getattr(robot, "service_client", None)
                    if robot.owner is None and (service_owner is None or
                            (previous and service_owner == previous.client_id)):
                        resumed = sessions.resume(envelope.get("token"), robot.control_generation)
                    if resumed:
                        sessions.items.pop(session.token, None)
                        session = resumed
                        client_id = session.client_id
                        if envelope.get("restore_control") is False:
                            session.wanted = False
                        if session.service and getattr(robot, "service_client", None) is None:
                            robot.service_client = client_id
                    resumable = True
                    await welcome()
                    continue
                lease = envelope.pop("lease", None)
                check_pause()
                if envelope.get("type") == "session_ready":
                    if resumable and lease == session.lease and session.paused:
                        if session.generation != robot.control_generation:
                            raise ControlError("recovery_cancelled")
                        if session.wanted:
                            robot.apply(client_id, Enable(type="enable", seq=0, enabled=True, source="browser:resume"))
                        session.paused = False
                    continue
                # Reject pre-pause buffered movement, even after recovery. STOP always works.
                if resumable and envelope.get("type") not in ("stop", "service_lock") and (session.paused or lease != session.lease):
                    continue
                message = COMMAND.validate_python(envelope)
                if message.seq <= last_seq:
                    raise ControlError("stale_sequence")
                last_seq = message.seq
                if isinstance(message, ServiceUnlock):
                    generation = robot.control_generation
                    access = getattr(ws.app.state, "service_access", None)
                    if not access or not await access.authorize(message.password.get_secret_value()):
                        raise ControlError("service_auth_failed_or_rate_limited")
                    if robot.control_generation != generation:
                        raise ControlError("service_unlock_cancelled")
                if message.type in ("home", "reference_step", "arm_move", "stop") or (
                        message.type == "enable" and not message.enabled):
                    LOG.warning("Control request: type=%s seq=%s source=%s owner=%s previous_reason=%s",
                                message.type, message.seq, getattr(message, "source", None),
                                robot.owner, robot.reason)
                robot.apply(client_id, message)
                if message.type == "enable":
                    session.wanted = message.enabled
                elif message.type in ("stop", "service_lock", "home", "reference_step", "arm_move"):
                    session.wanted = False
                    session.paused = False
                session.service = getattr(robot, "service_client", None) == client_id
            except (ValidationError, ControlError, ValueError, TypeError) as exc:
                session.wanted = False
                session.paused = False
                robot.invalid(client_id)
                code = str(exc) if isinstance(exc, ControlError) else "invalid_command"
                LOG.warning("Control rejected: %s", code)
                await send({"type": "error", "code": code})
            else:
                await send({"type": "ack", "seq": message.seq})

    tasks = []
    try:
        await welcome()
        tasks = [asyncio.create_task(publish()), asyncio.create_task(receive())]
        done, _ = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
        for task in done:
            task.result()
    except (WebSocketDisconnect, OSError, RuntimeError, TimeoutError, KeyError):
        pass
    finally:
        session.service = getattr(robot, "service_client", None) == client_id
        robot.disconnect(client_id)
        if resumable and session.service:
            robot.service_client = client_id
        sessions.detach(session, robot.control_generation)
        for task in tasks:
            task.cancel()
        for task in tasks:
            with suppress(asyncio.CancelledError, Exception):
                await task


async def tick_robot(robot):
    while True:
        robot.tick()
        await asyncio.sleep(0.02)
