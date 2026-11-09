const assert = require('node:assert/strict');
const path = require('node:path');
const {chromium} = require('playwright');
const base = process.env.OBOTX_TEST_URL || 'http://127.0.0.1:8011';
(async () => {
  const browser = await chromium.launch({headless: true, channel: 'msedge'});
  try {
    const page = await browser.newPage({viewport: {width: 1366, height: 1000}});
    const errors = [];
    page.on('pageerror', e => errors.push(e.message));
    await page.goto(base);
    await page.waitForSelector('.camera-settings');
    assert.equal(await page.locator('#side-platform').getAttribute('transform'),
      'translate(122.5 123.5) scale(0.5)');
    for (const width of [1366, 1920, 390]) {
      await page.setViewportSize({width, height: 800});
      const box = await page.locator('.camera-media').first().boundingBox();
      assert(Math.abs(box.width / box.height - 4 / 3) < .02);
      await page.screenshot({path: path.resolve(__dirname, `../../../tmp/camera-layout-${width}.png`), fullPage: true});
    }
    await page.setViewportSize({width: 1366, height: 1000});
    await page.locator('.camera-settings').first().click();
    await page.locator('#camera-mode').selectOption('rtsp');
    await page.locator('#camera-host').fill('192.168.100.21');
    await page.locator('#camera-password').fill('test-camera-secret');
    await page.locator('#camera-form button[type=submit]').click();
    await page.waitForFunction(() => !document.querySelector('#camera-dialog').open);
    await page.waitForSelector('.camera.is-error');
    const framePattern = '**/api/cameras/0/frame';
    await page.route(framePattern, route => route.fulfill({contentType: 'image/svg+xml',
      body: '<svg xmlns="http://www.w3.org/2000/svg" width="640" height="360"><rect width="640" height="360" fill="#158477"/></svg>'}));
    await page.waitForSelector('.camera.is-live');
    assert(await page.locator('.camera-image').first().evaluate(img => img.naturalWidth === 640));
    await page.unroute(framePattern);
    await page.waitForSelector('.camera.is-error');
    assert(await page.locator('.camera-image').first().isHidden());
    const saved = await (await page.request.get(base + '/api/cameras')).json();
    assert.equal(saved.cameras[0].host, '192.168.100.21');
    assert.equal(saved.cameras[0].password_set, true);
    assert(!JSON.stringify(saved).includes('test-camera-secret'));
    await page.reload();
    await page.waitForFunction(() => document.querySelector('#camera-summary').textContent.startsWith('1 /'));
    await page.locator('.camera-settings').first().click();
    assert.equal(await page.locator('#camera-host').inputValue(), '192.168.100.21');
    assert.equal(await page.locator('#camera-password').inputValue(), '');
    for (const width of [1366, 390, 320]) {
      await page.setViewportSize({width, height: 1000});
      assert(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth));
      await page.screenshot({path: path.resolve(__dirname, `../../../tmp/camera-dialog-${width}.png`)});
    }
    await page.setViewportSize({width: 1366, height: 1000});
    await page.locator('#camera-clear').click();
    await page.waitForFunction(() => !document.querySelector('#camera-dialog').open);
    await page.locator('.camera-settings').first().click();
    await page.locator('#camera-mode').selectOption('mjpeg');
    await page.locator('#camera-url').fill(base + '/static/robot-mark.svg');
    await page.locator('#camera-form button[type=submit]').click();
    await page.waitForSelector('.camera.is-live');
    await page.locator('.camera-settings').first().click();
    await page.locator('#camera-clear').click();
    await page.waitForFunction(() => document.querySelector('#camera-summary').textContent.startsWith('0 /'));
    assert.deepEqual(errors, []);
    await page.evaluate(() => renderRobot({...telemetry,
      arm: {...telemetry.arm, z_left_mm: 0, z_right_mm: 0}}));
    assert.equal(await page.locator('#rail-angle').getAttribute('transform'), 'translate(245 225) rotate(0)');
    await page.evaluate(() => renderRobot({...telemetry,
      arm: {...telemetry.arm, z_left_mm: 1000, z_right_mm: 1000}}));
    assert.equal(await page.locator('#rail-angle').getAttribute('transform'), 'translate(245 65) rotate(0)');
    await page.evaluate(() => {
      const original = renderRobot;
      renderRobot = state => original({...state, arm: {...state.arm, z_left_mm: 700, z_right_mm: 300}});
      renderRobot(telemetry);
    });
    for (const width of [1366, 390]) {
      await page.setViewportSize({width, height: 1000});
      const geometry = await page.evaluate(() => ({
        left: document.querySelector('#left-joint').getAttribute('transform'),
        right: document.querySelector('#right-joint').getAttribute('transform'),
        rail: document.querySelector('#rail-angle').getAttribute('transform'),
      }));
      assert(geometry.left.startsWith('translate(220 '));
      assert(geometry.right.startsWith('translate(270 '));
      const angle = Number(geometry.rail.match(/rotate\(([^)]+)\)/)[1]);
      assert(Math.abs(angle - Math.atan2(64, 50) * 180 / Math.PI) < .001);
      await page.locator('#side-schematic').screenshot({path: path.resolve(__dirname, `../../../tmp/robot-tilt-${width}.png`)});
    }
    console.log('PASS camera settings, secret redaction, reload, offline, direct image and 3 viewport checks');
  } finally { await browser.close(); }
})().catch(e => {console.error(e); process.exitCode = 1;});
