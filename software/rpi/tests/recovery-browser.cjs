const assert = require('node:assert/strict');
const {chromium} = require('playwright');
(async () => {
  const browser = await chromium.launch({headless:true, channel:'msedge'});
  try {
    const page = await browser.newPage();
    await page.goto('http://127.0.0.1:8012');
    await page.waitForFunction(() => clientId && telemetry);
    assert.equal(await page.evaluate(() => telemetry.backend), 'mock');
    await page.locator('#enable').check();
    await page.waitForFunction(() => ownsControl());
    const original = await page.evaluate(() => clientId);
    await page.keyboard.down('w');
    await page.waitForFunction(() => telemetry.command.vx_mps > 0);
    // Block the browser main thread while the real local server keeps ticking.
    await page.evaluate(() => { const end=performance.now()+800; while(performance.now()<end) {} });
    await page.waitForFunction(() => ownsControl() && telemetry.command.vx_mps === 0);
    assert(await page.locator('#enable').isChecked());
    await page.keyboard.up('w');
    await page.keyboard.down('w');
    await page.waitForFunction(() => telemetry.command.vx_mps > 0);
    await page.evaluate(() => socket.close());
    await page.waitForFunction(id => clientId === id && ownsControl() && telemetry.command.vx_mps === 0, original);
    await page.keyboard.up('w');
    await page.locator('#stop').click();
    await page.waitForFunction(() => !telemetry.owner);
    console.log('PASS real local WebSocket: browser stall, held key, reconnect, same session, zero motion, STOP');
  } finally { await browser.close(); }
})().catch(error => {console.error(error);process.exitCode=1;});
