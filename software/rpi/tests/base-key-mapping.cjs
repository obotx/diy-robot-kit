const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const source = fs.readFileSync(path.join(__dirname, '../web/app.js'), 'utf8');
const motion = source.slice(source.indexOf('function flushMotion()'), source.indexOf('const reasons ='));
for (const service of [false, true]) {
  for (const [key, axis, value] of [
    ['KeyW', 'vx_mps', .2], ['KeyS', 'vx_mps', -.2],
    ['KeyA', 'wz_radps', -.6], ['KeyD', 'wz_radps', .6],
    ['KeyQ', 'vy_mps', -.2], ['KeyE', 'vy_mps', .2],
  ]) {
    let sent;
    const context = {
      ownsControl: () => true,
      document: {querySelectorAll: () => []},
      $: selector => {
        assert(!['#speed', '#base-accel'].includes(selector));
        return {value: 50};
      },
      pressed: candidate => candidate === key,
      serviceMode: () => service,
      send: payload => { sent = payload; },
      keyButtons: [],
    };
    vm.runInNewContext(motion + '\nflushMotion();', context);
    assert.equal(sent.type, service ? 'service_jog' : 'motion');
    assert.equal(sent.acceleration.base_mps2, 1);
    for (const field of ['vx_mps', 'vy_mps', 'wz_radps']) {
      assert.equal(sent[field], field === axis ? value : 0, `${service}: ${key}: ${field}`);
    }
  }
}
console.log('Base key mapping: six keys in normal and service modes passed');
const pause = source.slice(source.indexOf('function pauseManualInput()'), source.indexOf('window.addEventListener("blur", pauseManualInput)'));
for (const homing of [false, true]) {
  const calls = [];
  vm.runInNewContext(pause + '\npauseManualInput();', {
    homePending: homing ? 1 : null, homeOwned: false,
    releaseControl: () => calls.push('release'),
    window: {dispatchEvent: () => calls.push('cancel-wheel-test')},
    Event: class {}, clearInput: () => calls.push('clear'),
    motion: () => calls.push('zero-motion'),
  });
  assert.deepEqual(calls, homing ? ['clear'] : ['cancel-wheel-test', 'clear', 'zero-motion']);
}
console.log('Hidden panel: manual input cleared without releasing control; homing retained');
const render = source.slice(source.indexOf('function renderRobot('), source.indexOf('function connect()'));
for (const [vy, wz, moving, rotating] of [[0, -.6, true, false], [-.2, 0, false, true]]) {
  const elements = new Map();
  const get = id => {
    if (!elements.has(id)) elements.set(id, {style: {}, setAttribute(k,v) {this[k]=v;}});
    return elements.get(id);
  };
  vm.runInNewContext(render + '\nrenderRobot(state);', {
    $: get, document: {querySelectorAll: () => []},
    state: {arm: {}, gripper: {}, command: {vx_mps: 0, vy_mps: vy, wz_radps: wz}},
  });
  assert.equal(get('#motion-arrow').visibility, moving ? 'visible' : 'hidden');
  assert.equal(get('#rotation-arrow').visibility, rotating ? 'visible' : 'hidden');
  assert.equal(get('#vy').textContent, moving ? '0.20' : '0.00');
  assert.equal(get('#wz').textContent, rotating ? '0.60' : '0.00');
}
console.log('Top view matches lateral and rotation panel mapping');
