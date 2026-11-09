(() => {
  const panel = document.querySelector('#wheel-test-panel');
  const buttons = [...panel.querySelectorAll('[data-wheel]')];
  const duty = document.querySelector('#wheel-test-duty');
  const status = document.querySelector('#wheel-test-status');
  let held = null;
  function available() {
    return serviceMode() && telemetry?.service?.wheel_test_supported &&
      !telemetry?.controllers?.arm_only &&
      telemetry?.controllers?.connected && !telemetry?.controllers?.states?.base?.fault &&
      socket?.readyState === WebSocket.OPEN && performance.now() - lastTelemetry < 650 &&
      !sessionPaused && !desiredControl && !enablePending && !homePending && !releasePending &&
      (!telemetry?.owner || telemetry.owner === clientId && telemetry.source === 'wheel_test');
  }
  function stop(text = 'Stopped — release and press again for another test.') {
    if (!held) return;
    held = null;
    window.wheelTestHeld = false;
    buttons.forEach(b => b.classList.remove('is-pressed'));
    send({type: 'stop'});
    status.textContent = text;
  }
  function begin(button) {
    if (held || !available()) return;
    const wheel = button.dataset.wheel;
    const pwm = Number(duty.value) * Number(button.dataset.direction);
    if (!Number.isFinite(pwm) || Math.abs(pwm) > .15) return;
    clearInput();
    // Zero explicitly arms one wheel. Reconnect/timeout cannot resume old nonzero frames.
    if (send({type: 'base_wheel_jog', wheel, pwm: 0}) === null) return;
    held = {button, wheel, pwm, started: performance.now()};
    window.wheelTestHeld = true;
    button.classList.add('is-pressed');
    status.textContent = `${wheel}: ${Math.round(pwm * 100)}% PWM — release to stop`;
    send({type: 'base_wheel_jog', wheel, pwm});
  }
  for (const button of buttons) {
    button.style.touchAction = 'none';
    button.addEventListener('pointerdown', event => {
      if (event.button !== 0) return;
      event.preventDefault();
      button.setPointerCapture(event.pointerId);
      begin(button);
    });
    for (const event of ['pointerup', 'pointercancel', 'lostpointercapture'])
      button.addEventListener(event, () => { if (held?.button === button) stop(); });
    button.addEventListener('keydown', event => {
      if (![' ', 'Enter'].includes(event.key)) return;
      event.preventDefault();
      if (!event.repeat) begin(button);
    });
    button.addEventListener('keyup', event => {
      if ([' ', 'Enter'].includes(event.key) && held?.button === button) stop();
    });
    button.addEventListener('blur', () => { if (held?.button === button) stop(); });
    button.addEventListener('contextmenu', event => event.preventDefault());
  }
  document.querySelector('#wheel-test-stop').addEventListener('click', () => {
    stop(); send({type: 'stop'});
  });
  window.addEventListener('wheel-test-cancel', () => stop());
  window.addEventListener('blur', () => stop());
  document.addEventListener('visibilitychange', () => { if (document.hidden) stop(); });
  setInterval(() => {
    const ok = available();
    panel.hidden = !serviceMode();
    if (!held) {
      status.textContent = telemetry?.controllers?.arm_only ? 'BASE excluded by ARM-only configuration.' :
        !telemetry?.controllers?.identities?.base ? 'BASE not connected.' :
        !telemetry?.service?.wheel_test_supported ? 'BASE firmware requires base_wheel_test_v1 (base-mdd10a-3).' :
        telemetry?.controllers?.states?.base?.fault ? `BASE fault: ${telemetry.controllers.states.base.fault}` :
        desiredControl || enablePending ? 'Release Take control to use individual wheel tests.' :
        !ok ? 'Waiting for connection or control release.' : 'Ready: FL / FR / RL / RR.';
    }
    buttons.forEach(button => { button.disabled = !ok || Boolean(held && held.button !== button); });
    duty.disabled = Boolean(held);
    const counts = telemetry?.controllers?.states?.base?.base?.encoder_counts;
    document.querySelector('#wheel-test-counts').textContent =
      'Encoder counts: ' + ['FL', 'FR', 'RL', 'RR'].map((wheel, i) => `${wheel}: ${counts?.[i] ?? '?'}`).join(' | ');
    if (!held) return;
    if (!ok || document.hidden || performance.now() - held.started >= 2000) { stop(); return; }
    send({type: 'base_wheel_jog', wheel: held.wheel, pwm: held.pwm});
  }, 80);
})();
