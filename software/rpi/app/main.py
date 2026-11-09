from __future__ import annotations

import asyncio
from contextlib import asynccontextmanager, suppress
from pathlib import Path
from typing import Any

from fastapi import FastAPI, HTTPException
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

from .config import load_config
from .control import router as control_router, tick_robot
from .hardware import HardwareController, UnknownOutputError, create_hardware_controller
from .robot import RobotSimulator
from .stm32 import SerialGateway, STM32Robot
from .stm32_config import load_robot_settings
from .arm_bench import BenchGateway, ArmBenchRobot
from .cameras import CameraService, router as camera_router
from .battery import BatteryMonitor, router as battery_router
from .service_access import ServiceAccess


ROOT_DIR = Path(__file__).resolve().parents[1]
WEB_DIR = ROOT_DIR / "web"


class SetOutputRequest(BaseModel):
    state: bool


@asynccontextmanager
async def lifespan(app: FastAPI):
    backend, stm32_config = load_robot_settings()
    config = load_config()
    app.state.config = config
    app.state.service_access = ServiceAccess()
    app.state.hardware = create_hardware_controller(config)
    app.state.cameras = CameraService()
    app.state.battery = BatteryMonitor()
    app.state.battery.start()
    if backend == "arm-bench":
        gateway = BenchGateway(stm32_config)
        app.state.robot = ArmBenchRobot(gateway)
    else:
        gateway = SerialGateway(stm32_config, backend) if stm32_config else None
        app.state.robot = STM32Robot(gateway) if gateway else RobotSimulator()
    if gateway:
        gateway.start()
    robot_task = asyncio.create_task(tick_robot(app.state.robot))
    try:
        yield
    finally:
        app.state.robot.stop("shutdown")
        robot_task.cancel()
        with suppress(asyncio.CancelledError):
            await robot_task
        if gateway:
            await asyncio.to_thread(gateway.close)
        app.state.hardware.close()
        await asyncio.to_thread(app.state.cameras.close)
        await asyncio.to_thread(app.state.battery.close)


app = FastAPI(title="ObotX RPi Control", version="0.9.7", lifespan=lifespan)
app.include_router(control_router)
app.include_router(camera_router)
app.include_router(battery_router)
app.mount("/static", StaticFiles(directory=WEB_DIR), name="static")


@app.get("/", include_in_schema=False)
def index() -> FileResponse:
    return FileResponse(WEB_DIR / "index.html")


@app.get("/api/status")
def get_status() -> dict[str, Any]:
    config = app.state.config
    hardware = _hardware()
    return {
        "service": "obotx-rpi-control",
        "hardware_backend": config.hardware_backend,
        "active_high": config.active_high,
        "outputs": [state.__dict__ for state in hardware.list_outputs()],
    }


@app.post("/api/outputs/{name}")
def set_output(name: str, request: SetOutputRequest) -> dict[str, Any]:
    try:
        state = _hardware().set_output(name, request.state)
    except UnknownOutputError as exc:
        raise HTTPException(status_code=404, detail=str(exc)) from exc
    return {"output": state.__dict__}


@app.post("/api/outputs/{name}/toggle")
def toggle_output(name: str) -> dict[str, Any]:
    try:
        state = _hardware().toggle_output(name)
    except UnknownOutputError as exc:
        raise HTTPException(status_code=404, detail=str(exc)) from exc
    return {"output": state.__dict__}


def _hardware() -> HardwareController:
    return app.state.hardware
