const assert = require('node:assert/strict');
const path = require('node:path');
const { chromium } = require('playwright');
const url = process.env.OBOTX_TEST_URL || 'http://127.0.0.1:8026';
(async () => {
  const browser = await chromium.launch({headless:true, channel:'msedge'});
  try {
    for (const width of [1366, 390, 320]) {
      const page = await browser.newPage({viewport:{width,height:950}});
      const errors=[];
      page.on('pageerror', e=>errors.push(e.message));
      const state=await (await page.request.get(url+'/api/robot')).json();
      Object.assign(state,{backend:'stm32',control_ready:true,control_blocked_reason:null,hardware_connected:true,
        owner:null,reason:'startup',homing:{supported_axes:['ZL'],active_axis:null,blocked_reason:null},
        arm_action:{supported:true,active_axis:null,kind:null,blocked_reason:null},
        controllers:{connected:true,identities:{arm:{capabilities:['arm_actions_v1','zl_only']}},states:{arm:{fault:null}}}});
      const sent=[];
      let timer;
      await page.routeWebSocket('**/ws/control', ws=>{
        ws.send(JSON.stringify({type:'welcome',protocol:1,client_id:'TEST',watchdog_ms:400}));
        const publish=()=>ws.send(JSON.stringify(state));
        ws.onMessage(raw=>{
          const msg=JSON.parse(raw); sent.push(msg);
          ws.send(JSON.stringify({type:'ack',seq:msg.seq}));
          if (['home','arm_move','arm_configure','clear_fault'].includes(msg.type)) {
            state.owner='TEST'; state.arm_action.kind=msg.type; state.arm_action.active_axis=msg.axis || null;
            state.homing.active_axis=msg.type==='home'?msg.axis:null;
          }
          if (msg.type==='stop' || (msg.type==='enable' && !msg.enabled)) {
            state.owner=null; state.arm_action.kind=null; state.arm_action.active_axis=null; state.homing.active_axis=null;
          }
          publish();
        });
        timer=setInterval(publish,100);
        ws.onClose(()=>clearInterval(timer));
        publish();
      });
      await page.goto(url);
      await page.locator('#arm-position-controls').waitFor({state:'visible'});
      await page.locator('#arm-position-controls summary').click();
      await page.locator('#arm-target').fill('20');
      await page.locator('#arm-velocity').fill('200');
      await page.locator('#arm-move').click();
      await page.waitForTimeout(250);
      assert(sent.some(m=>m.type==='arm_move' && m.position_um===20000 && m.velocity_um_s===200000));
      assert(sent.some(m=>m.type==='home_keepalive'));
      await page.locator('#stop').click();
      await page.waitForTimeout(150);
      await page.locator('#arm-configure').click();
      await page.waitForTimeout(100);
      assert(sent.some(m=>m.type==='arm_configure' && m.max_um===650000));
      await page.locator('#stop').click();
      await page.waitForTimeout(150);
      await page.locator('#arm-clear').click();
      await page.waitForTimeout(100);
      assert(sent.some(m=>m.type==='clear_fault'));
      await page.locator('#stop').click();
      await page.waitForTimeout(150);
      assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth), `Overflow at ${width}`);
      assert.deepEqual(errors,[]);
      await page.screenshot({path:path.resolve(__dirname,`../../../tmp/json-arm-ui-${width}.png`),fullPage:true});
      clearInterval(timer);
      await page.close();
    }
    console.log('PASS JSON action UI: payload units, lease, STOP, configure, clear, desktop/mobile layout');
  } finally { await browser.close(); }
})().catch(e=>{console.error(e);process.exitCode=1;});
