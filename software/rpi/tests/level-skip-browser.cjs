const assert = require('node:assert/strict');
const {chromium} = require('playwright');
(async () => {
  const browser = await chromium.launch({headless:true, channel:'msedge'});
  try {
    const page = await browser.newPage();
    const base = process.env.OBOTX_TEST_URL || 'http://127.0.0.1:8014';
    const template = await (await page.request.get(base + '/api/robot')).json();
    let level = true;
    const sent = [], errors = [];
    page.on('pageerror', e => errors.push(e.message));
    await page.routeWebSocket('**/ws/control', ws => {
      const welcome = {type:'welcome', protocol:1, client_id:'test', watchdog_ms:600, resume_token:'token', lease:'lease'};
      ws.send(JSON.stringify(welcome));
      const timer = setInterval(() => ws.send(JSON.stringify({...template,
        owner:null, enabled:false, reason:'idle', service:{active:false},
        homing:{sequence_supported:true, level_active:level, stage:0, blocked_reason:null},
        controllers:{connected:true, identities:{arm:{capabilities:['reference_level_center_v1']}},
          states:{arm:{enabled:false,fault:null,arm:{}}}},
      })), 50);
      timer.unref(); ws.onClose(() => clearInterval(timer));
      ws.onMessage(raw => {
        const m = JSON.parse(raw); sent.push(m);
        if (m.type === 'session_resume') ws.send(JSON.stringify(welcome));
        else ws.send(JSON.stringify({type:'ack',seq:m.seq}));
      });
    });
    await page.goto(base);
    await page.locator('#home-zl').click();
    await page.waitForFunction(() => document.querySelector('#home-tilt').hidden);
    assert.match(await page.locator('#home-warning').innerText(), /Level detected/);
    assert(!sent.some(m => m.type === 'reference_step'));
    level = false;
    await page.waitForFunction(() => !document.querySelector('#home-tilt').hidden);
    assert(await page.locator('#home-confirm').isDisabled());
    level = true;
    await page.waitForFunction(() => document.querySelector('#home-tilt').hidden);
    await page.locator('#home-dialog').screenshot({path:require('node:path').join(__dirname,'../../../tmp/level-skip.png')});
    await page.locator('#home-confirm').click();
    await page.waitForTimeout(100);
    assert(sent.some(m => m.type === 'reference_step' && m.step === 'level_center'));
    assert(!sent.some(m => m.step === 'level_down' || m.step === 'level_up'));
    assert.deepEqual(errors, []);
    console.log('PASS level detected, no automatic motion, lost level restores question, start centers sensor');
  } finally { await browser.close(); }
})().catch(e => {console.error(e);process.exitCode=1;});
