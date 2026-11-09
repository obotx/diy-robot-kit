const assert = require('node:assert/strict');
const path = require('node:path');
const {chromium} = require('playwright');
(async () => {
  const browser = await chromium.launch({headless:true,channel:'msedge'});
  try {
    const page = await browser.newPage();
    const errors=[];
    page.on('pageerror',e=>errors.push(e.message));
    await page.goto(process.env.OBOTX_TEST_URL || 'http://127.0.0.1:8013');
    await page.waitForFunction(()=>clientId && telemetry);
    assert.equal(await page.locator('#gripper-position').count(),0);
    assert.equal(await page.locator('#speed-settings-panel summary svg').count(),0);
    assert.equal(await page.locator('.robot-section .controls-section').count(),1);
    assert(await page.evaluate(()=>document.querySelector('.service-toolbar').getBoundingClientRect().top < document.querySelector('.controller-details').getBoundingClientRect().top));
    assert(await page.evaluate(()=>{
      const a=getComputedStyle(document.querySelector('#arm-position-controls summary'));
      const b=getComputedStyle(document.querySelector('#acceleration-settings-panel summary'));
      return a.fontFamily===b.fontFamily && a.fontSize===b.fontSize && a.fontWeight===b.fontWeight;
    }));
    for (const width of [1920,1366,900,390,320]) {
      await page.setViewportSize({width,height:1000});
      await page.locator('.controls-section').scrollIntoViewIfNeeded();
      assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth));
      assert(await page.evaluate(()=>{
        const controls=document.querySelector('.controls-section').getBoundingClientRect();
        const top=document.querySelector('.base-view').getBoundingClientRect();
        return controls.top>=top.bottom;
      }));
      await page.screenshot({path:path.resolve(__dirname,`../../../tmp/layout-097-${width}.png`)});
    }
    assert.deepEqual(errors,[]);
    console.log('PASS layout: order, headings, no gripper percentage, controls below views, 5 viewport widths');
  } finally {await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
