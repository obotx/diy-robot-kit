const assert = require('node:assert/strict');
const path = require('node:path');
const {chromium} = require('playwright');
const base = process.env.OBOTX_TEST_URL || 'http://127.0.0.1:8011';
(async () => {
  const browser = await chromium.launch({headless: true, channel: 'msedge'});
  try {
    const page = await browser.newPage();
    const errors = [];
    page.on('pageerror', error => errors.push(error.message));
    let data = {status: 'live', fresh: true, soc_pct: 64.2, voltage_v: 25.6, current_a: -2.5, remaining_minutes: 125, level: 'normal'};
    await page.route('**/api/battery', route => route.fulfill({json: data}));
    await page.goto(base);
    await page.waitForFunction(() => document.querySelector('#battery-soc').textContent === '64.2%');
    assert(await page.locator('#battery-alert').isHidden());
    assert.equal(await page.locator('#battery-runtime').textContent(), 'Remaining: ~2 h 5 min');
    data = {...data, soc_pct: 20, level: 'low'};
    await page.waitForFunction(() => document.querySelector('#battery-alert').textContent.includes('Recharge'));
    data = {...data, soc_pct: 5, level: 'critical'};
    await page.waitForFunction(() => document.querySelector('#battery-alert').textContent.includes('switch off'));
    for (const width of [1366, 390, 320]) {
      await page.setViewportSize({width, height: 900});
      assert(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth));
      const alert = await page.locator('#battery-alert').boundingBox();
      assert(alert.width <= width && alert.height > 0);
      await page.screenshot({path: path.resolve(__dirname, `../../../tmp/battery-critical-${width}.png`)});
    }
    data = {status: 'stale', fresh: false, soc_pct: null, last_known_level: 'critical'};
    await page.waitForFunction(() => document.querySelector('#battery-soc').textContent === '--%');
    assert(await page.locator('#battery-gauge').isHidden());
    assert.equal(await page.locator('#battery-runtime').textContent(), 'Remaining: --');
    assert((await page.locator('#battery-alert').textContent()).includes('Last known charge was critical'));
    data = {status: 'live', fresh: true, soc_pct: 30, level: 'normal'};
    await page.waitForFunction(() => document.querySelector('#battery-alert').hidden);
    data = {status: 'soc_unknown', fresh: true, soc_pct: null};
    await page.waitForFunction(() => document.querySelector('#battery-status').textContent.includes('synchronization'));
    assert.equal(await page.locator('#battery-soc').textContent(), '--%');
    await page.unroute('**/api/battery');
    await page.route('**/api/battery', route => route.abort());
    await page.waitForFunction(() => document.querySelector('#battery-status').textContent.includes('server unavailable'));
    assert.deepEqual(errors, []);
    console.log('PASS battery normal/low/critical/stale/recovery/unknown/offline and 3 viewport checks');
  } finally { await browser.close(); }
})().catch(error => {console.error(error); process.exitCode = 1;});
