const assert = require('node:assert/strict');
const path = require('node:path');
const {chromium} = require('playwright');
const base = process.env.OBOTX_TEST_URL || 'http://127.0.0.1:8011';
(async () => {
  const browser = await chromium.launch({headless:true,channel:'msedge'});
  try {
    const page = await browser.newPage();
    const template = await (await page.request.get(base+'/api/robot')).json();
    const errors=[],sent=[];
    page.on('pageerror',e=>errors.push(e.message));
    let owner=null,kind=null,stage=0,reason='startup',complete;
    await page.routeWebSocket('**/ws/control',ws=>{
      ws.send(JSON.stringify({type:'welcome',client_id:'fixture',protocol:1,watchdog_ms:400}));
      const timer=setInterval(()=>ws.send(JSON.stringify({...template,owner,reason,
        homing:{supported_axes:['Z','X'],sequence_supported:true,stage,blocked_reason:null},
        arm_action:{supported:true,kind,blocked_reason:null},
        controllers:{connected:true,arm_only:false,identities:{arm:{capabilities:['reference_sequence_v1']}},states:{arm:{fault:null,arm:{}}}},
      })),50);
      ws.onClose(()=>clearInterval(timer));timer.unref();
      ws.onMessage(raw=>{
        const m=JSON.parse(raw);sent.push(m);
        if(m.type==='reference_step') {
          owner='fixture';kind=m.step;reason=m.step;
          complete=()=>{stage={level_down:1,level_up:1,home_z:2,lift_z:3,home_x:4}[kind];owner=null;kind=null;reason='action_complete';};
        }
        if(m.type==='stop'||m.type==='enable'&&!m.enabled) {owner=null;kind=null;reason='operator';stage=0;}
        ws.send(JSON.stringify({type:'ack',seq:m.seq}));
      });
    });
    await page.goto(base);
    assert.equal(await page.locator('#home-axis').count(),0);
    await page.locator('#home-zl').click();
    assert(await page.locator('#home-confirm').isDisabled());
    await page.locator('#home-tilt-select').selectOption('level_down');
    for(const [index,step] of ['level_down','home_z','lift_z','home_x'].entries()) {
      await page.locator('#home-confirm').click();
      await page.waitForTimeout(250);
      assert.equal(sent.filter(m=>m.type==='reference_step').at(-1).step,step);
      assert(await page.locator('#home-confirm').isDisabled());
      assert(sent.some(m=>m.type==='home_keepalive'));
      complete();
      await page.waitForFunction(()=>!document.querySelector('#home-confirm').disabled);
      await page.waitForTimeout(150);
      assert.equal(sent.filter(m=>m.type==='reference_step').length,index+1);
    }
    for(const width of [1366,390,320]) {
      await page.setViewportSize({width,height:900});
      assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth));
      await page.screenshot({path:path.resolve(__dirname,`../../../tmp/reference-${width}.png`)});
    }
    await page.locator('#home-confirm').click();
    assert(!(await page.locator('#home-dialog').isVisible()));
    await page.locator('#home-zl').click();
    await page.locator('#home-tilt-select').selectOption('level_up');
    await page.locator('#home-confirm').click();
    await page.waitForTimeout(150);
    await page.locator('[data-close="home-dialog"]').click();
    await page.waitForTimeout(150);
    assert.equal(owner,null);
    assert(sent.some(m=>m.type==='stop'));
    assert.deepEqual(errors,[]);
    console.log('PASS reference wizard: four explicit stages, keepalive, cancellation, responsive dialog');
  } finally {await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
