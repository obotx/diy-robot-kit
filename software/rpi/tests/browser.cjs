const assert = require("node:assert/strict");
const path = require("node:path");
const { chromium } = require("playwright");

const base = process.env.OBOTX_TEST_URL || "http://127.0.0.1:8010";
const shots = path.resolve(__dirname, "../docs/images");
const errors = [];
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

async function waitState(page, predicate) {
  for (let attempt = 0; attempt < 40; attempt++) {
    const state = await (await page.request.get(`${base}/api/robot`)).json();
    if (predicate(state)) return state;
    await sleep(50);
  }
  const last = await (await page.request.get(`${base}/api/robot`)).json();
  throw new Error(`Robot state did not match expectation: ${JSON.stringify({last, errors, notice: await page.locator("#notice").textContent()})}`);
}

async function enable(page) {
  await page.locator("#enable").check();
  await page.waitForFunction(
    () =>
      document.querySelector("#control-status").textContent ===
      "Control active",
  );
}

(async () => {
  const browser = await chromium.launch({
    headless: true,
    channel: process.env.OBOTX_BROWSER || "msedge",
  });
  try {
    const context = await browser.newContext({
      viewport: { width: 1440, height: 1080 },
    });
    const page = await context.newPage();
    page.on("pageerror", (error) => errors.push(error.message));
    await page.goto(base);
    await page.waitForFunction(
      () => !document.querySelector("#enable").disabled,
    );
    assert.equal(await page.locator(".camera").count(), 4);
    assert.equal(await page.locator(".wheel").count(), 4);
    assert.equal(await page.locator("i[data-lucide]").count(), 0);
    assert.equal(await page.locator("html").getAttribute("lang"), "en");
    assert.equal(await page.locator("#backend-label").textContent(), "SIMULATION");
    assert.equal(await page.locator("#z-speed-units").textContent(), "70 mm/s");
    assert.equal(await page.locator("#x-speed-units").textContent(), "70 mm/s");
    await page.locator("#home-zl").click();
    await page.locator("#home-confirm").click();
    await waitState(page, (s) => s.homing.active_axis === "ZL");
    await sleep(600);
    await waitState(page, (s) => s.homing.active_axis === "ZL");
    await page.locator("#stop").click();
    await waitState(page, (s) => !s.enabled && !s.homing.active_axis);
    await page.locator(".acceleration-settings summary").click();
    await page.locator("#x-accel").fill("100");
    await enable(page);
    await page.keyboard.down("t");
    await waitState(page, (s) => s.command.x_mps > 0 && s.acceleration.x_mps2 === 0.1);
    await page.keyboard.up("t");
    await page.locator("#stop").click();
    await waitState(page, (s) => !s.enabled);
    await page.locator("#x-accel").fill("200");
    await page.locator(".acceleration-settings summary").click();
    assert.deepEqual(
      await page.locator(".camera-title strong").allTextContents(),
      ["Arm", "rear", "front left", "front right"],
    );
    await page.screenshot({
      path: path.join(shots, "operator-panel-desktop.png"),
      fullPage: true,
    });
    await enable(page);
    await page.keyboard.down("w");
    await waitState(page, (s) => s.command.vx_mps > 0);
    await page.keyboard.down("a");
    const diagonal = await waitState(page, (s) => s.command.vy_mps > 0);
    assert(
      Math.hypot(diagonal.command.vx_mps, diagonal.command.vy_mps) <= 0.070001,
    );
    await page.keyboard.up("w");
    await page.keyboard.up("a");
    await waitState(
      page,
      (s) => s.command.vx_mps === 0 && s.command.vy_mps === 0,
    );
    const before = await waitState(page, () => true);
    await page.keyboard.down("r");
    await waitState(page, (s) => s.arm.z_left_mm > before.arm.z_left_mm + 1);
    await page.keyboard.up("r");
    const after = await waitState(page, (s) => s.command.z_mps === 0);
    assert(Math.abs((after.arm.z_left_mm - after.arm.z_right_mm) - (before.arm.z_left_mm - before.arm.z_right_mm)) < 0.2);
    const beforeIndependent = await waitState(page, (s) =>
      Math.abs(s.simulated_velocity.zl_mps) < 1e-8 && Math.abs(s.simulated_velocity.zr_mps) < 1e-8);
    await page.keyboard.down("u");
    const leftOnly = await waitState(
      page,
      (s) => s.arm.z_left_mm > beforeIndependent.arm.z_left_mm + 2,
    );
    await page.keyboard.up("u");
    await waitState(page, (s) => s.command.zl_mps === 0);
    assert.equal(leftOnly.arm.z_right_mm, beforeIndependent.arm.z_right_mm);
    const tilt = await page.locator("#rail-angle").getAttribute("transform");
    assert(!tilt.endsWith("rotate(0)"));
    await page.keyboard.down("i");
    await waitState(
      page,
      (s) => s.command.zr_mps > 0 && s.command.zl_mps === 0,
    );
    await page.keyboard.up("i");
    await page.evaluate(() => {
      for (const [id, value] of [
        ["speed", "20"],
        ["z-speed", "40"],
        ["x-speed", "80"],
        ["gripper-speed", "60"],
      ]) {
        const input = document.getElementById(id);
        input.value = value;
        input.dispatchEvent(new Event("input"));
      }
    });
    const railBefore = await page.locator("#x-rail").getAttribute("transform");
    const mountBefore = await page
      .locator("#gripper-mount")
      .getAttribute("transform");
    await page.keyboard.down("t");
    await waitState(page, (s) => Math.abs(s.command.x_mps - 0.16) < 0.000001);
    await sleep(150);
    await page.keyboard.up("t");
    assert.notEqual(
      await page.locator("#x-rail").getAttribute("transform"),
      railBefore,
    );
    assert.equal(
      await page.locator("#gripper-mount").getAttribute("transform"),
      mountBefore,
    );
    await page.keyboard.down("o");
    await waitState(page, (s) => s.command.gripper === 0.6);
    await page.keyboard.up("o");
    await page.keyboard.down("r");
    await page.keyboard.down("u");
    await waitState(
      page,
      (s) =>
        Math.abs(s.command.z_mps - 0.08) < 0.000001 && s.command.zl_mps === 0,
    );
    await page.keyboard.up("u");
    await page.keyboard.up("r");
    await page.keyboard.down("w");
    await waitState(page, (s) => Math.abs(s.command.vx_mps - 0.04) < 0.000001);
    await page.keyboard.up("w");
    const button = page.locator('[data-key="KeyE"]');
    await button.scrollIntoViewIfNeeded();
    const box = await button.boundingBox();
    await page.mouse.move(box.x + 10, box.y + 10);
    await page.mouse.down();
    await waitState(page, (s) => s.command.wz_radps < 0);
    await page.mouse.move(10, 10);
    await page.mouse.up();
    await waitState(page, (s) => s.command.wz_radps === 0);
    await page.keyboard.down("w");
    await waitState(page, (s) => s.command.vx_mps > 0);
    await page.keyboard.press("Space");
    await page.keyboard.up("w");
    await waitState(page, (s) => !s.enabled && s.command.vx_mps === 0);
    await enable(page);
    await page.keyboard.down("w");
    await waitState(page, (s) => s.command.vx_mps > 0);
    await page.evaluate(() => window.dispatchEvent(new Event("blur")));
    await page.keyboard.up("w");
    await waitState(page, (s) => !s.enabled);
    await enable(page);
    await page.locator(".camera-settings").first().click();
    await waitState(page, (s) => !s.enabled);
    await page.locator("#camera-name").fill("Test camera");
    await page.locator("#camera-mode").selectOption("mjpeg");
    await page.locator("#camera-url").fill("rtsp://127.0.0.1/live");
    await page.locator("#camera-form button[type=submit]").click();
    assert.match(await page.locator("#camera-error").textContent(), /RTSP/);
    await page.locator("#camera-url").fill(`${base}/static/robot-mark.svg`);
    await page.locator("#camera-form button[type=submit]").click();
    await page.waitForSelector(".camera.is-live");
    await page.reload();
    await page.waitForSelector(".camera.is-live");
    assert.equal(
      await page.locator(".camera-title strong").first().textContent(),
      "Test camera",
    );
    await page.locator(".camera-settings").first().click();
    await page.locator("#camera-url").fill(`${base}/missing-camera`);
    await page.locator("#camera-form button[type=submit]").click();
    await page.waitForSelector(".camera.is-error");
    await page.locator(".camera-settings").first().click();
    await page.locator("#camera-clear").click();
    assert.equal(await page.locator(".camera-image").count(), 0);
    await enable(page);
    await context.setOffline(true);
    await waitState(
      page,
      (s) => !s.enabled && Object.values(s.command).every((n) => n === 0),
    );
    await page.waitForFunction(
      () =>
        document.querySelector("#enable").disabled &&
        !document.querySelector("#enable").checked,
    );
    await context.setOffline(false);
    await page.waitForFunction(
      () => !document.querySelector("#enable").disabled,
    );
    assert.equal(await page.locator("#enable").isChecked(), false);
    await page.locator("#api-open").click();
    assert.equal(
      await page.locator("#ws-address").textContent(),
      `${base.replace(/^http/, "ws")}/ws/control`,
    );
    await page.keyboard.press("Escape");
    await page.evaluate(() => localStorage.removeItem("obotx-cameras-v1"));
    await page.reload();
    await page.waitForFunction(() =>
      document.querySelector("#connection").classList.contains("connected"),
    );
    await sleep(800);
    assert.equal(await page.locator("#enable").isChecked(), false);
    for (let attempt = 0; attempt < 5; attempt++) {
      await enable(page);
      await page.keyboard.press("Space");
    }
    await page.waitForFunction(
      () => !document.querySelector("#enable").disabled,
    );
    const geometryIssues = await page.evaluate(async () => {
      const original = await (await fetch("/api/robot")).json();
      const issues = [];
      for (const zl of [0, 1000])
        for (const zr of [0, 1000])
          for (const x of [0, 1000]) {
            const state = structuredClone(original);
            Object.assign(state.arm, {
              z_left_mm: zl,
              z_right_mm: zr,
              x_mm: x,
            });
            renderRobot(state);
            const viewport = document
              .querySelector("#side-schematic")
              .getBoundingClientRect();
            const gripper = document
              .querySelector("#gripper-mount")
              .getBoundingClientRect();
            if (
              gripper.left < viewport.left ||
              gripper.right > viewport.right ||
              gripper.top < viewport.top ||
              gripper.bottom > viewport.bottom
            )
              issues.push({ zl, zr, x });
          }
      renderRobot(original);
      return issues;
    });
    assert.deepEqual(geometryIssues, []);
    for (const [width, height, name] of [
      [1920, 1080, "wide"],
      [1366, 768, "laptop"],
      [768, 1024, "tablet"],
      [390, 844, "mobile"],
      [320, 720, "small"],
    ]) {
      await page.setViewportSize({ width, height });
      await page.evaluate(() => window.scrollTo(0, 0));
      assert.equal(
        await page.evaluate(
          () => document.documentElement.scrollWidth > innerWidth,
        ),
        false,
        `Overflow at ${width}`,
      );
      const overlaps = await page
        .locator(".camera-head, .section-heading, .session-actions, .topbar")
        .evaluateAll((groups) =>
          groups.flatMap((group) => {
            const nodes = [...group.children].filter(
              (e) => e.getBoundingClientRect().width > 0,
            );
            return nodes.flatMap((a, i) =>
              nodes
                .slice(i + 1)
                .filter((b) => {
                  const x = a.getBoundingClientRect(),
                    y = b.getBoundingClientRect();
                  return (
                    Math.min(x.right, y.right) - Math.max(x.left, y.left) > 1 &&
                    Math.min(x.bottom, y.bottom) - Math.max(x.top, y.top) > 1
                  );
                })
                .map(() => group.className),
            );
          }),
        );
      assert.deepEqual(overlaps, [], `Overlaps at ${width}`);
      await page.screenshot({
        path: path.join(shots, `operator-panel-${name}.png`),
        fullPage: true,
      });
    }
    assert.deepEqual(errors, []);
    // Hold the socket in a read-only disconnected-hardware state for visual QA.
    const offlineContext = await browser.newContext();
    const offlinePage = await offlineContext.newPage();
    offlinePage.on("pageerror", (error) => errors.push(error.message));
    const hardware = await (await page.request.get(`${base}/api/robot`)).json();
    Object.assign(hardware, {backend: "stm32", enabled: false, owner: null,
      control_ready: false, control_blocked_reason: "hardware_disconnected",
      controllers: {connected: false, error: "reply_timeout"},
      homing: {supported_axes: [], active_axis: null, blocked_reason: "homing_not_supported_by_json_firmware"},
      arm: {z_left_mm: null, z_right_mm: null, x_mm: null, homed: false},
      gripper: {opening_pct: null}});
    const timers = [];
    await offlinePage.routeWebSocket("**/ws/control", (ws) => {
      ws.send(JSON.stringify({type: "welcome", client_id: "ui-test"}));
      timers.push(setInterval(() => ws.send(JSON.stringify(hardware)), 100));
    });
    try {
      await offlinePage.goto(base);
      await offlinePage.waitForFunction(() => document.querySelector("#backend-label").textContent === "STM32 HARDWARE");
      assert.equal(await offlinePage.locator("#z-left").textContent(), "--");
      assert.equal(await offlinePage.locator("#enable").isDisabled(), true);
      assert.equal(await offlinePage.locator("#home-zl").isDisabled(), true);
      await offlinePage.locator(".acceleration-settings summary").click();
      for (const width of [1366, 390, 320]) {
        await offlinePage.setViewportSize({width, height: 900});
        assert.equal(await offlinePage.evaluate(() => document.documentElement.scrollWidth > innerWidth), false);
        const overlap = await offlinePage.locator(".topbar, .speed-control, .acceleration-grid label").evaluateAll((groups) => groups.some((group) => {
          const children = [...group.children];
          return children.some((a, i) => children.slice(i + 1).some((b) => {
            const x = a.getBoundingClientRect(), y = b.getBoundingClientRect();
            return Math.min(x.right, y.right) - Math.max(x.left, y.left) > 1 && Math.min(x.bottom, y.bottom) - Math.max(x.top, y.top) > 1;
          }));
        }));
        assert.equal(overlap, false, `Hardware controls overlap at ${width}`);
        await offlinePage.screenshot({path: path.join(shots, `operator-hardware-offline-${width}.png`), fullPage: true});
      }
      hardware.controllers = {connected: true, discovery: true,
        identities: {arm: {device_id: "ARM-TEST", firmware: "arm-link-1"}, base: {device_id: "BASE-TEST", firmware: "base-link-1"}},
        ports: {arm: "/dev/ttyACM1", base: "/dev/ttyACM0"}};
      hardware.control_blocked_reason = "commissioning_required";
      await offlinePage.locator(".controller-details summary").click();
      await offlinePage.waitForFunction(() => document.querySelector("#arm-link-detail").textContent.includes("ARM-TEST"));
      assert.match(await offlinePage.locator("#base-link-detail").textContent(), /BASE-TEST.*ttyACM0/);
      for (const width of [1366, 390]) {
        await offlinePage.setViewportSize({width, height: 900});
        assert.equal(await offlinePage.evaluate(() => document.documentElement.scrollWidth > innerWidth), false);
        await offlinePage.screenshot({path: path.join(shots, `operator-auto-discovery-fixture-${width}.png`), fullPage: true});
      }
    } finally {
      timers.forEach(clearInterval);
      await offlineContext.close();
    }
    assert.deepEqual(errors, []);
    console.log(
      "PASS: English UI, camera names, independent/grouped Z, separate speeds, sliding rail with fixed gripper, keyboard, STOP, disconnect, camera settings and 5 viewport layouts",
    );
  } finally {
    await browser.close();
  }
})().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
