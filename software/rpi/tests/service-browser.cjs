const assert = require('node:assert/strict');
const path = require('node:path');
const {chromium} = require('playwright');
const base = process.env.OBOTX_TEST_URL || 'http://127.0.0.1:8011';
(async () => {
  const browser = await chromium.launch({headless: true, channel: 'msedge'});
  try {
    const page = await browser.newPage();
    const sent = [], errors = [];
    page.on('pageerror', e => errors.push(e.message));
    const template = await (await page.request.get(base + '/api/robot')).json();
    let selected = false, owner = null, fault = null, armOnly = false;
    let paused = false, lease = 'initial', recovery = false;
    await page.routeWebSocket('**/ws/control', ws => {
      const welcome = {type:'welcome',protocol:1,client_id:'fixture',watchdog_ms:600,resume_token:'test-tab-token',lease};
      ws.send(JSON.stringify(welcome));
      const timer = setInterval(() => ws.send(JSON.stringify({...template,
        ...(recovery ? {control_session:{paused,lease,wanted:true}} : {}),
        owner, enabled:!!owner, control_ready:false, control_blocked_reason:'commissioning_required',
        service_password_configured:true,
        service:{supported:true,active:selected,client_id:selected?'fixture':null,running:!!owner},
        controllers:{connected:true,arm_only:armOnly,identities:{arm:{capabilities:['arm_actions_v1','service_jog_v2','gripper_velocity','service_gripper_v1']},base:{capabilities:['full_state_v1','base_clear_fault_v1']}},states:{arm:{fault,arm:{}},base:{fault:null,ready:true}}},
        arm_action:{supported:true,kind:null,blocked_reason:'commissioning_required'},
      })), 50);
      timer.unref(); ws.onClose(() => clearInterval(timer));
      ws.onMessage(raw => {
        const m=JSON.parse(raw); sent.push(m);
        if(m.type==='session_resume') { ws.send(JSON.stringify({...welcome,wanted:false})); return; }
        if(m.type==='service_unlock') selected=true;
        if(m.type==='service_lock') {selected=false;owner=null;}
        if(m.type==='enable') owner=m.enabled?'fixture':null;
        if(m.type==='stop') owner=null;
        if(m.type==='clear_fault') fault=null;
        if(m.type==='session_ready' && m.lease===lease) {paused=false;owner='fixture';}
        ws.send(JSON.stringify({type:'ack',seq:m.seq}));
      });
    });
    await page.goto(base);
    await page.waitForFunction(() => !document.querySelector('#service-toggle').disabled);
    await page.waitForFunction(() => document.querySelector('#service-limits').hidden);
    await page.locator('#service-toggle').click();
    await page.locator('#service-confirm').check();
    await page.locator('#service-password').fill('fixture-service-password');
    await page.locator('#service-form button[type=submit]').click();
    await page.waitForFunction(() => document.body.classList.contains('service-mode'));
    assert(await page.locator('#service-limits').isVisible());
    assert.equal(owner,null);
    await page.locator('#enable').check();
    await page.waitForFunction(() => !document.querySelector('[data-key="KeyT"]').disabled);
    await page.locator('#acceleration-settings-panel').evaluate(el => el.open=true).catch(()=>{});
    await page.evaluate(() => {
      for(const [id,value] of [['z-speed','20'],['x-speed','40'],['z-accel','80'],['x-accel','400']]) {
        const el=document.getElementById(id);el.value=value;el.dispatchEvent(new Event('input',{bubbles:true}));
      }
    });
    await page.keyboard.down('u');await page.keyboard.down('t');
    await page.waitForTimeout(200);
    assert(sent.some(m=>m.type==='service_jog'&&Math.abs(m.x_mps-.04)<1e-9&&m.acceleration.x_mps2===.4&&m.acceleration.z_mps2===.08));
    await page.keyboard.down('w');await page.keyboard.down('o');
    await page.waitForTimeout(3200);
    assert.equal(await page.locator('[data-wheel]').count(), 8);
    assert(sent.some(m=>m.type==='service_jog'&&m.vx_mps>0&&m.gripper>0&&m.x_mps>0));
    assert(sent.filter(m=>m.type==='service_jog').at(-1).vx_mps > 0);
    await page.screenshot({path:require('node:path').join(__dirname, '../../../tmp/service-099.png'),fullPage:true});
    await page.keyboard.up('w');await page.keyboard.up('o');
    await page.waitForTimeout(150);
    assert.equal(sent.filter(m=>m.type==='service_jog').at(-1).gripper,0);
    armOnly = true;
    await page.waitForFunction(()=>document.querySelector('[data-key="KeyW"]').disabled);
    assert(await page.locator('[data-key="KeyO"]').isEnabled());
    assert(await page.locator('[data-key="KeyP"]').isEnabled());
    await page.keyboard.down('p');
    await page.waitForTimeout(150);
    assert(sent.filter(m=>m.type==='service_jog').at(-1).gripper < 0);
    await page.keyboard.up('p');
    await page.evaluate(() => { window.requestAnimationFrame = () => 0; });
    const beforeStall = sent.length;
    await page.waitForTimeout(700);
    assert(sent.slice(beforeStall).filter(m=>m.type==='service_jog').length >= 4,
      'Motion heartbeat must not depend on animation frames');
    await page.keyboard.up('u');await page.keyboard.up('t');
    await page.waitForTimeout(150);
    const released = sent.filter(m=>m.type==='service_jog').at(-1);
    assert.equal(released.x_mps,0);assert.equal(released.zl_mps,0);
    await page.keyboard.down('t');
    await page.waitForTimeout(150);
    const beforeRecovery = sent.length;
    recovery=true;paused=true;owner=null;lease='after-watchdog';
    await page.waitForTimeout(350);
    assert(sent.slice(beforeRecovery).some(m=>m.type==='session_ready'));
    assert.equal(sent.filter(m=>m.type==='service_jog').at(-1).x_mps,0);
    assert(await page.locator('#enable').isChecked());
    assert(await page.locator('#service-toggle').isChecked());
    await page.keyboard.up('t');
    await page.keyboard.down('t');
    await page.waitForTimeout(120);
    assert(sent.filter(m=>m.type==='service_jog').at(-1).x_mps>0);
    await page.keyboard.up('t');
    recovery=false;
    fault='latched_motion_fault';owner=null;
    await page.waitForFunction(() => document.querySelector('[data-key="KeyT"]').disabled);
    assert(await page.locator('#service-toggle').isChecked());
    await page.keyboard.up('u');await page.keyboard.up('t');
    await page.locator('#faults-panel').evaluate(el=>el.open=true);
    for (const width of [1366, 390]) {
      await page.setViewportSize({width, height:900});
      await page.locator('#faults-panel').screenshot({path:path.join(__dirname, '../../../tmp/faults-' + width + '.png')});
      assert(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth));
    }
    await page.locator('#arm-clear').click();
    await page.waitForTimeout(200);
    assert.equal(owner,null);
    assert(await page.locator('#service-toggle').isChecked());
    await page.locator('#enable').check();
    await page.waitForTimeout(200);
    const last=sent.filter(m=>m.type==='service_jog').at(-1);
    assert.equal(last.x_mps,0);assert.equal(last.zl_mps,0);
    await page.locator('#stop').click();
    await page.waitForTimeout(150);
    for(const width of [1366,390,320]) {
      await page.setViewportSize({width,height:900});
      assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth));
      await page.screenshot({path:path.resolve(__dirname,`../../../tmp/service-shared-${width}.png`)});
    }
    const unlockCount = sent.filter(m=>m.type==='service_unlock').length;
    await page.reload();
    await page.waitForFunction(()=>document.body.classList.contains('service-mode') && !document.querySelector('#service-toggle').disabled);
    assert.equal(sent.filter(m=>m.type==='service_unlock').length,unlockCount);
    assert(sent.some(m=>m.type==='session_resume' && m.token==='test-tab-token' && m.restore_control===false));
    assert(!(await page.locator('#enable').isChecked()));
    await page.locator('#service-toggle').click();
    await page.waitForFunction(()=>!document.body.classList.contains('service-mode'));
    await page.reload();
    await page.waitForFunction(()=>!document.querySelector('#service-toggle').disabled);
    assert(!(await page.locator('#service-toggle').isChecked()));
    assert.deepEqual(errors,[]);
    console.log('PASS shared controls, acceleration, retained mode after fault, explicit restart, responsive views');
  } finally {await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
