"use strict";

const $ = (selector) => document.querySelector(selector);
const icons = () => window.lucide?.createIcons();
const wsAddress = `${location.protocol === "https:" ? "wss" : "ws"}://${location.host}/ws/control`;
const keyButtons = [...document.querySelectorAll("[data-key]")];
const validKeys = new Set(keyButtons.map((button) => button.dataset.key));
const keyboard = new Set();
const pointers = new Map();
let socket,
  clientId,
  telemetry,
  seq = 0,
  lastTelemetry = 0;
let desiredControl = false,
  enablePending = null;
let ownershipConfirmed = false;
let releasePending = false;
let homePending = null;
let homeOwned = false;
function readSession(key) {
  try { return sessionStorage.getItem(key); } catch { return null; }
}
function saveSession(key, value) {
  try { sessionStorage.setItem(key, value); } catch { /* Storage may be disabled. */ }
}
let resumeToken = readSession('obotx-resume-token'), controlLease = null, sessionPaused = false, readyLease = null;
let reloadedSession = Boolean(resumeToken);
let serviceSelected = readSession('obotx-service-selected') === 'true';
let lastHeartbeat = performance.now();

function notice(text = "") {
  $("#notice").textContent = text;
  $("#notice").hidden = !text;
}

function ownsControl() {
  return Boolean(
    desiredControl &&
      !sessionPaused &&
      clientId &&
      telemetry?.owner === clientId &&
      socket?.readyState === WebSocket.OPEN,
  );
}

function send(payload) {
  if (socket?.readyState !== WebSocket.OPEN) return null;
  if (socket.bufferedAmount > 1024) {
    socket.close();
    return null;
  }
  const number = ++seq;
  socket.send(JSON.stringify({ ...payload, lease: controlLease, seq: number }));
  return number;
}

function serviceMode() {
  return Boolean(clientId && telemetry?.service?.active && telemetry.service.client_id === clientId);
}

function clearInput() {
  keyboard.clear();
  pointers.clear();
  keyButtons.forEach((button) => button.classList.remove("is-pressed"));
}

function releaseControl(globalStop = false, cause = "release") {
  window.dispatchEvent(new Event("wheel-test-cancel"));
  const shouldRelease = desiredControl || enablePending !== null || homePending !== null || homeOwned;
  homePending = null;
  homeOwned = false;
  clearInput();
  desiredControl = false;
  ownershipConfirmed = false;
  enablePending = null;
  if (globalStop) releasePending = send({ type: "stop" }) !== null;
  else if (shouldRelease)
    releasePending =
      send({ type: "enable", enabled: false, source: `browser:${cause}` }) !== null;
  renderControl();
}

function pressed(key) {
  if (keyButtons.find(button => button.dataset.key === key)?.disabled) return false;
  return keyboard.has(key) || [...pointers.values()].includes(key);
}

function motion() {
  // Input release and watchdog refresh must not wait for a rendering frame.
  flushMotion();
}

function flushMotion() {
  if (!ownsControl()) return;
  if (![...document.querySelectorAll(".parameter-number")].every((input) => input.checkValidity())) {
    releaseControl(true);
    return;
  }
  const speed = 1; // Fixed panel base speed: 200 mm/s.
  const zSpeed = Number($("#z-speed").value) / 190;
  const xSpeed = Number($("#x-speed").value) / 200;
  const gripperSpeed = Number($("#gripper-speed").value) / 50;
  const groupedZ = pressed("KeyR") || pressed("KeyF");
  if (serviceMode()) {
    const z = (Number(pressed("KeyR")) - Number(pressed("KeyF"))) * 0.19 * zSpeed;
    const forward = Number(pressed("KeyW")) - Number(pressed("KeyS"));
    const left = Number(pressed("KeyE")) - Number(pressed("KeyQ"));
    const length = Math.max(1, Math.hypot(forward, left));
    send({type: "service_jog",
      zl_mps: groupedZ ? z : (Number(pressed("KeyU")) - Number(pressed("KeyJ"))) * 0.19 * zSpeed,
      zr_mps: groupedZ ? z : (Number(pressed("KeyI")) - Number(pressed("KeyK"))) * 0.19 * zSpeed,
      x_mps: (Number(pressed("KeyT")) - Number(pressed("KeyG"))) * 0.2 * xSpeed,
      gripper: (Number(pressed("KeyO")) - Number(pressed("KeyP"))) * gripperSpeed,
      vx_mps: forward / length * 0.2 * speed,
      vy_mps: left / length * 0.2 * speed,
      wz_radps: (Number(pressed("KeyD")) - Number(pressed("KeyA"))) * 0.6 * speed,
      acceleration: {z_mps2: Number($("#z-accel").value) / 1000, x_mps2: Number($("#x-accel").value) / 1000,
        base_mps2: 1, rotation_radps2: Number($("#rotation-accel").value),
        gripper_per_s: Number($("#gripper-accel").value) / 50}});
    keyButtons.forEach(button => button.classList.toggle("is-pressed", pressed(button.dataset.key)));
    return;
  }
  let forward = Number(pressed("KeyW")) - Number(pressed("KeyS"));
  let left = Number(pressed("KeyE")) - Number(pressed("KeyQ"));
  const length = Math.max(1, Math.hypot(forward, left));
  forward /= length;
  left /= length;
  send({
    type: "motion",
    vx_mps: forward * 0.2 * speed,
    vy_mps: left * 0.2 * speed,
    wz_radps: (Number(pressed("KeyD")) - Number(pressed("KeyA"))) * 0.6 * speed,
    z_mps: (Number(pressed("KeyR")) - Number(pressed("KeyF"))) * 0.19 * zSpeed,
    zl_mps: groupedZ
      ? 0
      : (Number(pressed("KeyU")) - Number(pressed("KeyJ"))) * 0.19 * zSpeed,
    zr_mps: groupedZ
      ? 0
      : (Number(pressed("KeyI")) - Number(pressed("KeyK"))) * 0.19 * zSpeed,
    x_mps: (Number(pressed("KeyT")) - Number(pressed("KeyG"))) * 0.2 * xSpeed,
    gripper: (Number(pressed("KeyO")) - Number(pressed("KeyP"))) * gripperSpeed,
    acceleration: {
      base_mps2: 1,
      rotation_radps2: Number($("#rotation-accel").value),
      z_mps2: Number($("#z-accel").value) / 1000,
      x_mps2: Number($("#x-accel").value) / 1000,
      gripper_per_s: Number($("#gripper-accel").value) / 50,
    },
  });
  keyButtons.forEach((button) =>
    button.classList.toggle("is-pressed", pressed(button.dataset.key)),
  );
}

const reasons = {
  startup: "Idle",
  enabled: "Ready",
  operator: "Operator STOP",
  released: "Control released",
  watchdog: "STOP: command timeout",
  disconnect: "STOP: disconnected",
  invalid_command: "STOP: invalid command",
  hardware_fault: "STOP: controller link fault",
  hardware_disconnected: "Controllers disconnected",
  commissioning_required: "Commissioning required",
  controller_not_ready: "Controller not ready",
  homing_required: "Homing required",
  z_difference_limit: "STOP: Z difference limit",
  unsupported_command: "STOP: unsupported command",
  homing: "Homing ZL",
  homing_complete: "ZL homing complete",
  bench_stopped: "Bench stopped",
  bench_home_zl_only: "ZL homing bench only",
};
function renderControl() {
  const online = Boolean(
    clientId &&
      socket?.readyState === WebSocket.OPEN &&
      performance.now() - lastTelemetry < 650,
  );
  const own = online && ownsControl();
  const busy = online && telemetry?.owner && telemetry.owner !== clientId;
  $("#connection").classList.toggle("connected", online);
  $("#connection-text").textContent = online
    ? "Server connected"
    : "Disconnected";
  const service = serviceMode();
  const blocked = service ? !telemetry?.controllers?.connected || Boolean(telemetry?.controllers?.states?.arm?.fault) : telemetry?.control_ready === false;
  $("#enable").disabled = Boolean(window.wheelTestHeld) || telemetry?.source === "wheel_test" || !online || Boolean(busy) || releasePending || blocked || homeOwned || homePending !== null;
  const sequenceSupported = Boolean(telemetry?.homing?.sequence_supported);
  $("#home-zl").disabled = !online || Boolean(telemetry?.owner) || releasePending || homePending !== null
    || !sequenceSupported || Boolean(telemetry?.homing?.blocked_reason);
  $("#home-zl").title = online && !sequenceSupported ? "Guided homing requires ARM firmware 0.9.0 and updated server" : "Home robot";
  renderFaults(online);
  if (service) {
    $("#home-zl").disabled = true;
  }
  const single = telemetry?.controllers?.identities?.arm?.capabilities?.includes("zl_only");
  const armOnly = telemetry?.controllers?.arm_only;
  const baseState = telemetry?.controllers?.states?.base;
  const baseIdentity = telemetry?.controllers?.identities?.base;
  $("#platform-control-status").textContent = armOnly ? "BASE excluded: ARM-only configuration" :
    !baseIdentity && telemetry?.backend !== "mock" ? "BASE not connected" :
    baseState && !baseState.ready ? "Platform not ready; check BASE firmware and faults" : "";
  $("#homing-status").textContent = telemetry?.homing?.active_axis ? `Homing ${telemetry.homing.active_axis}...` :
    telemetry?.arm?.homed ? "Referenced" : Object.values(telemetry?.arm?.homed_axes || {}).some(Boolean) ?
      `Referenced: ${Object.entries(telemetry.arm.homed_axes).filter(([, homed]) => homed).map(([axis]) => axis).join(", ")}` :
    telemetry?.homing?.blocked_reason === "homing_not_supported_by_json_firmware" ? "Homing unavailable in JSON mode" :
    telemetry?.homing?.blocked_reason ? reasons[telemetry.homing.blocked_reason] || telemetry.homing.blocked_reason : single ? "ZL not referenced" : "Axes not referenced";
  $("#enable").checked = own || (sessionPaused && desiredControl) || enablePending !== null;
  $("#control-status").textContent = !online
    ? "Disconnected"
    : own
      ? "Control active"
      : busy
        ? "Another client is controlling"
        : service ? (blocked ? "Service: motion blocked" : "Service: ready to take control") : reasons[telemetry?.control_blocked_reason] || reasons[telemetry?.reason] || "Idle";
  $("#control-source").textContent = own
    ? "This browser"
    : busy
      ? `Client: ${telemetry.source}`
      : telemetry?.backend === "mock"
        ? "Simulated robot"
        : telemetry?.controllers?.error || (telemetry?.controllers?.connected ?
          telemetry.backend === "arm-bench" ? "Standalone ZL connected" : armOnly ? "ARM controller connected" : "Both controllers connected" : "Robot not connected");
  $("#backend-label").textContent = !online ? "DISCONNECTED" : {
    mock: "SIMULATION", stm32: "STM32 HARDWARE", "stm32-emulator": "STM32 EMULATOR", "arm-bench": "ZL BENCH / ASCII",
  }[telemetry?.backend] || "CONNECTING";
  $("#data-source").textContent = !online ? "Data unavailable" : telemetry?.backend === "mock"
    ? "Simulated data" : telemetry?.backend === "stm32-emulator" ? "Emulated controllers" : "Controller-reported data";
  $("#stm32-status").textContent = !online || !telemetry?.controllers?.connected
    ? "STM32: not connected" : telemetry.backend === "arm-bench" ? "STM32: standalone ZL only" : armOnly ? "STM32: ARM only" : telemetry.backend === "stm32-emulator" ? "STM32: emulated pair" : "STM32: arm + base connected";
  for (const role of ["arm", "base"]) {
    const identity = online && telemetry?.controllers?.connected && telemetry.controllers.identities?.[role];
    $(`#${role}-link-detail`).textContent = identity
      ? `${identity.device_id} | ${identity.firmware} | ${telemetry.controllers.ports?.[role] || "Serial link"}`
      : role === "arm" && online && telemetry?.backend === "arm-bench" && telemetry.controllers?.connected
        ? "Standalone ASCII bench (no device identity)" : "Not connected";
  }
  keyButtons.forEach((button) => {
    const key = button.dataset.key;
    const inactive = single && !["KeyU", "KeyJ"].includes(key) ||
      (armOnly || baseState?.ready === false) && ["KeyW","KeyS","KeyA","KeyD","KeyQ","KeyE"].includes(key);
    const unsupportedGripper = telemetry?.backend === "stm32" && ["KeyO","KeyP"].includes(key) &&
      !telemetry?.controllers?.identities?.arm?.capabilities?.includes("gripper_velocity");
    const serviceGripperUnsupported = service && ["KeyO","KeyP"].includes(key) &&
      !telemetry?.controllers?.identities?.arm?.capabilities?.includes("service_gripper_v1");
    button.disabled = !own || inactive || unsupportedGripper || serviceGripperUnsupported;
  });
}

function renderRobot(state) {
  const { arm, command, gripper } = state;
  // Match the installed platform's panel mapping; these are commanded values.
  const c = {vx_mps: command.vx_mps, vy_mps: -command.wz_radps / 3,
    wz_radps: -command.vy_mps * 3};
  const number = (value, digits) => Number.isFinite(value) ? value.toFixed(digits) : "--";
  $("#z-left").textContent = number(arm.z_left_mm, 1);
  $("#z-right").textContent = number(arm.z_right_mm, 1);
  $("#x-position").textContent = number(arm.x_mm, 1);
  $("#vx").textContent = c.vx_mps.toFixed(2);
  $("#vy").textContent = c.vy_mps.toFixed(2);
  $("#wz").textContent = c.wz_radps.toFixed(2);
  // Diagram coordinates are schematic, not a calibrated mechanical angle.
  const known = [arm.z_left_mm, arm.z_right_mm, arm.x_mm].every(Number.isFinite);
  for (const id of ["#rail-angle", "#left-joint", "#right-joint"]) {
    $(id).style.opacity = known ? "1" : "0.25";
  }
  // Map the 1000 mm Z stroke between the lower and upper carriage centres.
  const leftY = 225 - (arm.z_left_mm ?? 350) * 0.16;
  const rightY = 225 - (arm.z_right_mm ?? 350) * 0.16;
  const angle = (Math.atan2(rightY - leftY, 50) * 180) / Math.PI;
  $("#rail-angle").setAttribute(
    "transform",
    `translate(245 ${(leftY + rightY) / 2}) rotate(${angle})`,
  );
  $("#x-rail").setAttribute("transform", `translate(${(arm.x_mm ?? 300) * 0.09} 0)`);
  $("#left-joint").setAttribute(
    "transform",
    `translate(220 ${leftY}) rotate(${angle})`,
  );
  $("#right-joint").setAttribute(
    "transform",
    `translate(270 ${rightY}) rotate(${angle})`,
  );
  const jaw = 5 + (gripper.opening_pct ?? 50) * 0.15;
  $("#gripper-fingers").setAttribute(
    "d",
    `M${-jaw} 27V48H${-jaw + 6}M${jaw} 27V48H${jaw - 6}`,
  );
  const moving = Math.hypot(c.vx_mps, c.vy_mps) > 0;
  $("#motion-arrow").setAttribute("visibility", moving ? "visible" : "hidden");
  $("#motion-arrow").setAttribute(
    "transform",
    `rotate(${(-Math.atan2(c.vy_mps, c.vx_mps) * 180) / Math.PI} 140 120)`,
  );
  $("#rotation-arrow").setAttribute(
    "visibility",
    c.wz_radps ? "visible" : "hidden",
  );
  $("#rotation-arrow").setAttribute(
    "transform",
    c.wz_radps > 0 ? "translate(280 0) scale(-1 1)" : "",
  );
  document
    .querySelectorAll(".wheel")
    .forEach((wheel) =>
      wheel.classList.toggle("is-moving", moving || c.wz_radps !== 0),
    );
}

function connect() {
  socket = new WebSocket(wsAddress);
  let handshake = false;
  clientId = null;
  seq = 0;
  socket.addEventListener("message", (event) => {
    const data = JSON.parse(event.data);
    if (data.type === "session_retry") {
      socket.close();
      return;
    }
    if (data.type === "welcome") {
      if (!handshake && data.resume_token) {
        handshake = true;
        socket.send(JSON.stringify({type: "session_resume", token: resumeToken, restore_control: !reloadedSession}));
        return;
      }
      resumeToken = data.resume_token;
      if (resumeToken) saveSession('obotx-resume-token', resumeToken);
      reloadedSession = false;
      controlLease = data.lease;
      sessionPaused = Boolean(data.paused);
      readyLease = null;
      desiredControl = Boolean(data.wanted);
      clientId = data.client_id;
      lastTelemetry = performance.now();
    }
    if (data.type === "telemetry") {
      if (!clientId) return;
      telemetry = data;
      serviceSelected = serviceMode();
      saveSession('obotx-service-selected', String(serviceSelected));
      lastTelemetry = performance.now();
      if (data.control_session) {
        const state = data.control_session;
        controlLease = state.lease;
        sessionPaused = state.paused;
        if (sessionPaused) {
          clearInput();
          desiredControl = state.wanted;
          ownershipConfirmed = false;
          notice("Connection recovered. Release and press a movement button again.");
          if (readyLease !== controlLease) {
            readyLease = controlLease;
            send({type: "session_ready"});
          }
        } else readyLease = null;
      }
      if (data.owner === clientId && (data.homing?.active_axis || data.arm_action?.kind)) {
        homeOwned = true;
        homePending = null;
        // Telemetry continues in background tabs even when timers are throttled.
        send({ type: "home_keepalive" });
      } else if (homeOwned) {
        homeOwned = false;
      }
      if (data.owner !== clientId) releasePending = false;
      if (desiredControl && data.owner === clientId) {
        ownershipConfirmed = true;
        enablePending = null;
      } else if (!sessionPaused && ownershipConfirmed && data.owner !== clientId) {
        desiredControl = false;
        ownershipConfirmed = false;
        clearInput();
      }
      renderControl();
      renderRobot(data);
      renderReference();
    }
    if (data.type === "error") {
      homePending = null;
      homeOwned = false;
      desiredControl = false;
      ownershipConfirmed = false;
      enablePending = null;
      clearInput();
      notice(
        data.code === "busy"
          ? "Another client already owns control."
          : `Command rejected: ${data.code}`,
      );
      renderControl();
      if (referenceRunning) { referenceRunning = false; referenceFailed = true; renderReference(); }
    }
    if (data.type === "ack" && data.seq === homePending) {
      homePending = null;
      // ACK confirms receipt, not an active action; telemetry owns that state.
    }
  });
  socket.addEventListener("close", () => {
    homePending = null;
    homeOwned = false;
    clearInput();
    desiredControl = false;
    ownershipConfirmed = false;
    enablePending = null;
    releasePending = false;
    clientId = null;
    telemetry = null;
    if (referenceRunning) { referenceRunning = false; referenceFailed = true; renderReference(); }
    renderControl();
    [
      "#vx",
      "#vy",
      "#wz",
      "#z-left",
      "#z-right",
      "#x-position",
    ].forEach((id) => {
      $(id).textContent = "--";
    });
    $("#motion-arrow").setAttribute("visibility", "hidden");
    $("#rotation-arrow").setAttribute("visibility", "hidden");
    document
      .querySelectorAll(".wheel")
      .forEach((wheel) => wheel.classList.remove("is-moving"));
    setTimeout(connect, 1500);
  });
  socket.addEventListener("error", () => socket.close());
}

$("#enable").addEventListener("change", (event) => {
  if (event.target.checked) {
    clearInput();
    desiredControl = true;
    ownershipConfirmed = false;
    enablePending = send({ type: "enable", enabled: true, source: "browser" });
    notice();
  } else releaseControl(false, "enable_unchecked");
  // Leave focus off the checkbox so Space always acts as STOP, never toggles enable.
  event.target.blur();
});
$("#stop").addEventListener("click", () => releaseControl(true));
$("#home-zl").addEventListener("click", () => {
  if (!telemetry?.homing?.sequence_supported) return;
  referenceWizard = Boolean(telemetry?.homing?.sequence_supported);
  referenceRunning = referenceFailed = false;
  referenceExpected = 0;
  referenceLevelSkipped = false;
  $("#home-tilt").hidden = !referenceWizard;
  $("#home-dialog h2").textContent = referenceWizard ? "Robot homing" : "Homing";
  if (referenceWizard) {
    $("#home-tilt-select").value = "";
    renderReference();
    $("#home-dialog").showModal();
    return;
  }
});
$("#home-confirm").addEventListener("click", () => {
  if (referenceWizard) {
    if (referenceRunning || referenceFailed) return;
    if (referenceExpected === 4) { $("#home-dialog").close(); return; }
    const step = referenceExpected === 0 ? (referenceLevelSkipped ? "level_center" : $("#home-tilt-select").value) :
      [null, "home_z", "lift_z", "home_x"][referenceExpected];
    if (!step) return;
    referenceLevelSkipped = false;
    referenceExpected++;
    referenceRunning = true;
    referenceSeenActive = false;
    armAction({type:"reference_step", step});
    renderReference();
    return;
  }
});
let referenceWizard = false, referenceRunning = false, referenceFailed = false, referenceExpected = 0, referenceSeenActive = false;
let referenceLevelSkipped = false;
function renderReference() {
  if (!referenceWizard) return;
  const h = telemetry?.homing;
  if (!referenceRunning && !referenceFailed && referenceExpected === 0) {
    referenceLevelSkipped = h?.level_active === true &&
      telemetry?.controllers?.identities?.arm?.capabilities?.includes("reference_level_center_v1") === true;
  }
  if (referenceRunning && telemetry?.owner === clientId) referenceSeenActive = true;
  if (referenceRunning && !telemetry?.owner && !homePending) {
    if (h?.stage === referenceExpected && telemetry?.reason === "action_complete") referenceRunning = false;
    else if (referenceSeenActive) { referenceRunning = false; referenceFailed = true; }
  }
  $("#home-tilt").hidden = referenceExpected !== 0 || referenceLevelSkipped;
  const labels = ["Is the gripper end of X tilted down or up?", "Leveling ZL + ZR", "Homing ZL + ZR", "Raising Z to 200 mm", "Homing X (retract)"];
  $("#home-warning").textContent = referenceFailed ? "Homing interrupted. Close and start again." :
    referenceRunning ? labels[referenceExpected] : referenceLevelSkipped ? "Level detected. Find the center of the sensor range at 5 mm/s." : referenceExpected === 0 ? labels[0] :
      referenceExpected === 4 ? "Homing complete" : `${labels[referenceExpected]}: complete`;
  $("#home-confirm").textContent = referenceExpected === 0 ? "Start leveling" : referenceExpected === 4 && !referenceRunning ? "Finish" : "Next";
  $("#home-confirm").disabled = referenceRunning || referenceFailed || (referenceExpected === 0 && !referenceLevelSkipped && !$("#home-tilt-select").value);
}
$("#home-tilt-select").addEventListener("change", renderReference);
$("#home-dialog").addEventListener("close", () => {
  if (referenceRunning) releaseControl(true, "homing_cancel");
  referenceWizard = referenceRunning = false;
  $("#home-confirm").disabled = false;
});
function renderFaults(online) {
  const states = telemetry?.controllers?.states || {};
  const descriptions = {
    encoder_no_forward_feedback: "No encoder feedback in the requested direction. Check motor, encoder and wiring.",
    encoder_speed_invalid: "Encoder speed exceeds the configured range. Check signals and calibration.",
    control_tick_overrun: "Motor control timing exceeded its allowed interval.",
    local_watchdog: "Controller stopped after losing fresh commands.",
    communication_timeout: "Communication with the controller timed out.",
    reply_timeout: "No controller reply received in time.",
    wheel_test_timeout: "Legacy individual wheel test exceeded its duration.",
    latched_motion_fault: "ARM motion fault is latched.",
  };
  const armCodes = ["", "Drive not enabled", "Motor driver alarm", "Limit switch reached",
    "Position limit reached", "Z height difference exceeded", "Motor power disabled",
    "Invalid motion command", "Homing timeout", "Homing switch error",
    "Step generator error", "Communication timeout"];
  const idle = online && telemetry?.controllers?.connected && !telemetry?.owner &&
    !Object.values(states).some(state => state.enabled) && homePending === null;
  let count = 0;
  for (const role of ["arm", "base"]) {
    const state = states[role], fault = state?.fault;
    const code = role === "arm" ? state?.arm?.fault_code : null;
    const output = $(`#${role}-fault`);
    if (fault) count++;
    output.textContent = !online || !state ? "State unavailable" : fault ?
      `${fault}${code ? ` (code ${code})` : ""}: ${code && armCodes[code] ? armCodes[code] : descriptions[fault] || "Controller reported a fault; inspect wiring and logs."}` : "No active faults";
    output.classList.toggle("fault-active", Boolean(fault));
    const caps = telemetry?.controllers?.identities?.[role]?.capabilities || [];
    const supported = caps.includes(role === "base" ? "base_clear_fault_v1" : "arm_actions_v1");
    $(`#${role}-clear`).disabled = !idle || !supported || !fault;
    $(`#${role}-clear`).title = !supported ? "Controller firmware does not support fault clearing" : "Clear fault without restarting motion";
  }
  $("#fault-count").textContent = count ? `(${count})` : "";
  $("#fault-clear-status").textContent = telemetry?.owner ? "Release Take control before clearing faults." :
    homePending !== null || telemetry?.arm_action?.kind ? "Controller action in progress..." : "";
  const last = telemetry?.controllers?.last_failure;
  $("#fault-history").textContent = last ? `Last stop (history): ${last.reason}` : "";
}

function armAction(payload) {
  clearInput();
  homePending = send(payload);
  renderControl();
}
$("#arm-clear").addEventListener("click", () => armAction({type:"clear_fault"}));
$("#base-clear").addEventListener("click", () => armAction({type:"clear_fault", controller:"base"}));

function renderSpeedUnits() {
  $("#z-speed-units").textContent = `${Number($("#z-speed").value)} mm/s`;
  $("#x-speed-units").textContent = `${Number($("#x-speed").value)} mm/s`;
  $("#gripper-speed-units").textContent = `${Number($("#gripper-speed").value)} %/s`;
}
function renderAccelerations() {
  document.querySelectorAll(".acceleration-grid input[type=range]").forEach((input) => {
    $(`#${input.id}-value`).value = input.value;
  });
}
for (const slider of document.querySelectorAll(".speed-settings input[type=range], .acceleration-grid input[type=range]")) {
  const number = $(`#${slider.id}-value`);
  slider.addEventListener("input", () => {
    number.value = slider.value;
    renderSpeedUnits();
    motion();
  });
  number.addEventListener("change", () => {
    if (!number.reportValidity()) return;
    slider.value = number.value;
    number.value = slider.value;
    renderSpeedUnits();
    motion();
  });
}

function editing(event) {
  return Boolean(
    event.target.closest(
      "input, textarea, select, [contenteditable=true], dialog[open]",
    ),
  );
}
window.addEventListener("keydown", (event) => {
  if (event.code === "Escape" || (event.code === "Space" && !editing(event))) {
    event.preventDefault();
    releaseControl(true);
    document
      .querySelectorAll("dialog[open]")
      .forEach((dialog) => dialog.close());
    return;
  }
  if (event.ctrlKey || event.altKey || event.metaKey) {
    if (keyboard.size) {
      clearInput();
      motion();
    }
    return;
  }
  if (!validKeys.has(event.code) || editing(event) || !ownsControl()) return;
  event.preventDefault();
  if (!event.repeat) {
    keyboard.add(event.code);
    motion();
  }
});
window.addEventListener("keyup", (event) => {
  if (keyboard.delete(event.code)) {
    event.preventDefault();
    motion();
  }
});
keyButtons.forEach((button) => {
  button.disabled = true;
  button.addEventListener("pointerdown", (event) => {
    if (!ownsControl() || event.button !== 0) return;
    event.preventDefault();
    button.setPointerCapture(event.pointerId);
    pointers.set(event.pointerId, button.dataset.key);
    motion();
  });
  for (const name of ["pointerup", "pointercancel", "lostpointercapture"]) {
    button.addEventListener(name, (event) => {
      if (pointers.delete(event.pointerId)) motion();
    });
  }
  button.addEventListener("contextmenu", (event) => event.preventDefault());
});
function pauseManualInput() {
  if (homePending !== null || homeOwned) {
    clearInput();
    return;
  }
  window.dispatchEvent(new Event("wheel-test-cancel"));
  clearInput();
  motion();
}
window.addEventListener("blur", pauseManualInput);
window.addEventListener("pagehide", () => releaseControl(false, "pagehide"));
window.addEventListener("offline", () => {
  releaseControl(true);
  socket?.close();
});
document.addEventListener("visibilitychange", () => {
  if (document.hidden) pauseManualInput();
});
document.addEventListener("focusin", (event) => {
  if (
    event.target.matches(
      "input:not(#enable),textarea,select,[contenteditable=true]",
    )
  ) {
    clearInput();
    motion();
  }
});
setInterval(() => {
  const now = performance.now();
  if (now - lastHeartbeat > 600 || (clientId && now - lastTelemetry > 650)) {
    clearInput();
  }
  lastHeartbeat = now;
  if (clientId && now - lastTelemetry > 5000) socket.close();
  if (homeOwned) send({ type: "home_keepalive" });
  else if (!document.hidden) motion();
}, 100);

const defaultCameras = ["Arm", "rear", "front left", "front right"].map(
  (name) => ({ name, mode: "rtsp", host: "", port: 554, stream: 2, username: "admin", url: "", password_set: false }),
);
const previousCameraNames = [
  "Prz\u00f3d",
  "Ty\u0142",
  "Lewa strona",
  "Chwytak",
];
let cameras = defaultCameras.map((camera) => ({ ...camera }));
let cameraIndex = 0;
let cameraTimers = [];
let cameraAbort = [];
let cameraGeneration = 0;
let cameraPollTimers = [];
function validateCameraUrl(value) {
  if (!value) return "";
  const url = new URL(value);
  if (!["http:", "https:"].includes(url.protocol))
    throw new Error(
      "Enter an HTTP/HTTPS MJPEG URL. RTSP requires a video bridge.",
    );
  if (url.username || url.password)
    throw new Error("Camera URLs must not contain a username or password.");
  if (location.protocol === "https:" && url.protocol !== "https:")
    throw new Error("An HTTPS panel requires an HTTPS stream.");
  return url.href;
}
async function loadCameras() {
  try {
    const response = await fetch("/api/cameras", {cache: "no-store"});
    if (!response.ok) throw new Error("Camera settings unavailable");
    const data = await response.json();
    cameras = data.cameras;
    renderCameras();
  } catch {
    $("#camera-summary").textContent = "Camera settings unavailable";
  }
}

function cameraMode() {
  const rtsp = $("#camera-mode").value === "rtsp";
  $("#camera-rtsp").hidden = !rtsp;
  $("#camera-mjpeg").hidden = rtsp;
}
$("#camera-mode").addEventListener("change", cameraMode);

function openDialog(id) {
  releaseControl(false, "settings_dialog");
  $(id).showModal();
}
function renderCameras() {
  const generation = ++cameraGeneration;
  cameraPollTimers.forEach(clearTimeout);
  cameraPollTimers = [];
  cameraAbort.forEach((controller) => controller.abort());
  cameraAbort = [];
  cameraTimers.forEach(clearInterval);
  cameraTimers = [];
  document
    .querySelectorAll(".camera-image")
    .forEach((image) => image.removeAttribute("src"));
  $("#camera-grid").replaceChildren();
  cameras.forEach((camera, index) => {
    const tile = document.createElement("article");
    tile.className = "camera";
    tile.innerHTML = `<div class="camera-head"><div class="camera-title"><span class="camera-number">0${index + 1}</span><strong></strong></div><div class="camera-tools"><button class="icon-button camera-settings" title="Camera settings ${index + 1}" aria-label="Camera settings ${index + 1}"><i data-lucide="settings-2"></i></button><button class="icon-button camera-fullscreen" title="Camera fullscreen ${index + 1}" aria-label="Camera fullscreen ${index + 1}"><i data-lucide="maximize"></i></button></div></div><div class="camera-media"><div class="camera-placeholder"><i data-lucide="video-off"></i><span></span><small>CAM ${String(index + 1).padStart(2, "0")} &middot; IP VIDEO</small></div></div><div class="camera-foot"><span class="dot"></span><span class="camera-state"></span></div>`;
    tile.querySelector("strong").textContent = camera.name;
    const text = tile.querySelector(".camera-placeholder span");
    const status = tile.querySelector(".camera-state");
    const configured = camera.mode === "rtsp" ? camera.host : camera.url;
    text.textContent = configured
      ? "Connecting to camera..."
      : "Camera not connected";
    status.textContent = configured ? "Waiting for image" : "Not configured";
    tile.querySelector(".camera-settings").addEventListener("click", () => {
      cameraIndex = index;
      $("#camera-dialog-title").textContent = `Camera ${index + 1}`;
      $("#camera-name").value = camera.name;
      $("#camera-url").value = camera.url;
      $("#camera-mode").value = camera.mode;
      $("#camera-host").value = camera.host;
      $("#camera-port").value = camera.port;
      $("#camera-stream").value = camera.stream;
      $("#camera-user").value = camera.username;
      $("#camera-password").value = "";
      $("#camera-password").placeholder = camera.password_set ? "Unchanged" : "Password";
      cameraMode();
      $("#camera-error").textContent = "";
      openDialog("#camera-dialog");
    });
    tile
      .querySelector(".camera-fullscreen")
      .addEventListener("click", async () => {
        releaseControl(false, "camera_fullscreen");
        try {
          if (document.fullscreenElement) await document.exitFullscreen();
          else await tile.requestFullscreen();
        } catch {
          notice("Fullscreen is unavailable in this browser.");
        }
      });
    if (configured && camera.mode === "rtsp") {
      const image = document.createElement("img");
      image.className = "camera-image";
      image.alt = camera.name;
      image.hidden = true;
      tile.querySelector(".camera-media").append(image);
      const controller = new AbortController();
      cameraAbort.push(controller);
      let objectUrl = null;
      controller.signal.addEventListener("abort", () => {
        if (objectUrl) URL.revokeObjectURL(objectUrl);
      });
      const poll = async () => {
        if (generation !== cameraGeneration) return;
        const started = performance.now();
        let delay = 50;
        try {
          if (document.hidden) throw new Error("Preview paused");
          const response = await fetch(`/api/cameras/${index}/frame`, {cache: "no-store",
            signal: AbortSignal.any([controller.signal, AbortSignal.timeout(4000)])});
          if (!response.ok) {
            const error = await response.json();
            throw new Error(error.detail || "Camera offline");
          }
          const blob = await response.blob();
          if (generation !== cameraGeneration) return;
          if (objectUrl) URL.revokeObjectURL(objectUrl);
          objectUrl = URL.createObjectURL(blob);
          image.src = objectUrl;
          await image.decode();
          if (generation !== cameraGeneration) return;
          image.hidden = false;
          tile.classList.add("is-live");
          tile.classList.remove("is-error");
          status.textContent = "Live preview \u00b7 RTSP";
        } catch (error) {
          image.hidden = true;
          tile.classList.remove("is-live");
          tile.classList.add("is-error");
          text.textContent = "No live image";
          status.textContent = error.message;
          delay = 1500;
        }
        if (generation === cameraGeneration)
          cameraPollTimers[index] = setTimeout(poll, Math.max(0, delay - (performance.now() - started)));
      };
      poll();
    } else if (configured) {
      const image = document.createElement("img");
      image.className = "camera-image";
      image.alt = camera.name;
      image.hidden = true;
      image.referrerPolicy = "no-referrer";
      let failed = false;
      const start = performance.now();
      image.addEventListener("error", () => {
        failed = true;
        image.hidden = true;
        tile.classList.remove("is-live");
        tile.classList.add("is-error");
        text.textContent = "No image";
        status.textContent = "Check URL or connection";
      });
      // Multipart MJPEG can expose the first frame before the load event fires.
      const checkFrame = () => {
        if (!failed && image.naturalWidth > 0) {
          image.hidden = false;
          tile.classList.add("is-live");
          tile.classList.remove("is-error");
          status.textContent = "Image received \u00b7 MJPEG";
        } else if (!failed && performance.now() - start > 8000) {
          tile.classList.add("is-error");
          text.textContent = "No image";
          status.textContent = "Connection timed out";
        }
      };
      image.addEventListener("load", checkFrame);
      image.src = camera.url;
      tile.querySelector(".camera-media").append(image);
      cameraTimers.push(setInterval(checkFrame, 500));
    }
    $("#camera-grid").append(tile);
  });
  $("#camera-summary").textContent =
    `${cameras.filter((camera) => camera.mode === "rtsp" ? camera.host : camera.url).length} / 4 configured`;
  icons();
}

$("#camera-form").addEventListener("submit", async (event) => {
  event.preventDefault();
  const button = event.submitter;
  if (button) button.disabled = true;
  try {
    const next = {
      name: $("#camera-name").value.trim(),
      url: validateCameraUrl($("#camera-url").value.trim()),
      mode: $("#camera-mode").value,
      host: $("#camera-host").value.trim(),
      port: Number($("#camera-port").value),
      stream: Number($("#camera-stream").value),
      username: $("#camera-user").value.trim(),
      password: $("#camera-password").value || null,
    };
    if (!next.name) throw new Error("Enter a camera name.");
    const response = await fetch(`/api/cameras/${cameraIndex}`, {
      method: "PUT", headers: {"Content-Type": "application/json", "X-ObotX-Camera": "1"},
      body: JSON.stringify(next),
    });
    if (!response.ok) throw new Error("Unable to save: check IP address, port and server connection");
    cameras = (await response.json()).cameras;
    $("#camera-password").value = "";
    $("#camera-dialog").close();
    renderCameras();
  } catch (error) {
    $("#camera-error").textContent = error.message;
  } finally {
    if (button) button.disabled = false;
  }
});
$("#camera-clear").addEventListener("click", () => {
  $("#camera-url").value = "";
  $("#camera-host").value = "";
  $("#camera-form").requestSubmit();
});
document
  .querySelectorAll("[data-close]")
  .forEach((button) =>
    button.addEventListener("click", () =>
      document.getElementById(button.dataset.close).close(),
    ),
  );
$("#ws-address").textContent = wsAddress;
$("#api-open").addEventListener("click", () => openDialog("#api-dialog"));
$("#api-footer").addEventListener("click", () => openDialog("#api-dialog"));
renderCameras();
loadCameras();
renderSpeedUnits();
renderControl();
connect();

renderAccelerations();
