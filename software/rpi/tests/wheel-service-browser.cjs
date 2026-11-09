const assert = require('node:assert/strict');
const {chromium} = require('playwright');
const base = process.env.OBOTX_TEST_URL || 'http://127.0.0.1:8026';
(async () => {
  const browser = await chromium.launch({headless:true, channel:'msedge'});
  try {
    const page = await browser.newPage();
    const template = await (await page.request.get(base + '/api/robot')).json();
    const sent = [], errors = [];
    let selected = false, owner = null, armOnly = false;
    page.on('pageerror', e => errors.push(e.message));
    await page.routeWebSocket('**/ws/control', ws => {
      ws.send(JSON.stringify({type:'welcome',protocol:1,client_id:'fixture',watchdog_ms:600,lease:'test'}));
      const timer = setInterval(() => ws.send(JSON.stringify({...template,
        owner, source:owner?'wheel_test':null, enabled:!!owner, control_ready:false,
        service_password_configured:true,
        service:{supported:true,wheel_test_supported:true,active:selected,client_id:selected?'fixture':null},
        controllers:{connected:true,arm_only:armOnly,identities:{arm:{capabilities:[]},base:{capabilities:['base_wheel_test_v1']}},
          states:{arm:{fault:null,enabled:false,arm:{}},base:{fault:null,enabled:!!owner,base:{encoder_counts:[0,123,-5,8]}}}},
      })),40);
      timer.unref(); ws.onClose(() => clearInterval(timer));
      ws.onMessage(raw => {
        const m=JSON.parse(raw);sent.push(m);
        if(m.type==='service_unlock') selected=true;
        if(m.type==='base_wheel_jog') owner=m.pwm?'fixture':null;
        if(m.type==='stop') owner=null;
        ws.send(JSON.stringify({type:'ack',seq:m.seq}));
      });
    });
    await page.goto(base);
    await page.waitForFunction(() => !document.querySelector('#service-toggle').disabled);
    assert(await page.locator('#wheel-test-panel').isHidden());
    await page.locator('#service-toggle').click();
    await page.locator('#service-confirm').check();
    await page.locator('#service-password').fill('fixture-password');
    await page.locator('#service-form button[type=submit]').click();
    const fr=page.locator('[data-wheel="FR"][data-direction="1"]');
    await fr.waitFor({state:'visible'});
    await page.waitForFunction(() => !document.querySelector('[data-wheel="FR"]').disabled);
    await fr.hover();await page.mouse.down();await page.waitForTimeout(250);await page.mouse.up();
    assert(sent.some(m=>m.type==='base_wheel_jog'&&m.wheel==='FR'&&m.pwm===.1));
    assert.equal(sent.at(-1).type,'stop');
    await page.waitForTimeout(150);
    const rr=page.locator('[data-wheel="RR"][data-direction="-1"]');
    await rr.hover();await page.mouse.down();await page.waitForTimeout(2300);
    assert.equal(sent.at(-1).type,'stop');
    assert(sent.some(m=>m.type==='base_wheel_jog'&&m.wheel==='RR'&&m.pwm===-.1));
    await page.mouse.up();await page.waitForTimeout(150);
    await fr.hover();await page.mouse.down();await page.waitForTimeout(120);
    await page.evaluate(()=>window.dispatchEvent(new Event('blur')));
    await page.waitForTimeout(100);assert.equal(sent.at(-1).type,'stop');await page.mouse.up();
    await page.waitForTimeout(150);
    const fl = page.locator('[data-wheel="FL"][data-direction="1"]');
    await fl.hover();await page.mouse.down();await page.waitForTimeout(150);await page.mouse.up();
    assert(sent.some(m=>m.type==='base_wheel_jog'&&m.wheel==='FL'&&m.pwm===.1));
    assert(!sent.some(m=>m.type==='enable'||m.type==='service_jog'));
    assert((await page.locator('#wheel-test-counts').textContent()).includes('FR: 123'));
    armOnly = true;
    await page.waitForFunction(()=>document.querySelector('[data-wheel="FR"]').disabled);
    assert(await page.locator('#wheel-test-panel').isVisible());
    await page.waitForFunction(()=>document.querySelector('#wheel-test-status').textContent.includes('ARM-only'));
    for(const width of [390,1366]) {
      await page.setViewportSize({width,height:900});
      await page.locator('#wheel-test-panel').screenshot({path:require('node:path').join(__dirname,'../../../tmp/wheels-017-'+width+'.png')});
      assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth+1));
    }
    assert.deepEqual(errors,[]);
    console.log('PASS wheel buttons, zero-arm, release, 2s limit, blur, no ARM/FL commands, counters, mobile layout');
  } finally { await browser.close(); }
})().catch(e=>{console.error(e);process.exitCode=1;});
