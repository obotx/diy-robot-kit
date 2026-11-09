(() => {
  const toggle = document.querySelector('#service-toggle');
  const dialog = document.querySelector('#service-dialog');
  toggle.addEventListener('change', () => {
    clearInput();
    if (toggle.checked) {
      toggle.checked = false;
      document.querySelector('#service-form').reset();
      dialog.showModal();
    } else {
      releaseControl(false, 'service_mode_change');
      send({type: 'service_lock'});
    }
  });
  dialog.addEventListener('close', () => {
    document.querySelector('#service-password').value = '';
  });
  document.querySelector('#service-form').addEventListener('submit', event => {
    event.preventDefault();
    clearInput();
    const password = document.querySelector('#service-password');
    send({type: 'service_unlock', confirm_bypass: true, password: password.value});
    password.value = '';
    dialog.close();
  });
  setInterval(() => {
    const connected = socket?.readyState === WebSocket.OPEN && performance.now() - lastTelemetry < 650;
    const selected = connected && clientId && telemetry ? serviceMode() : serviceSelected;
    toggle.checked = selected;
    toggle.disabled = !connected || Boolean(telemetry?.owner && (!selected || telemetry.owner !== clientId)) ||
      (!selected && (!telemetry?.service?.supported || !telemetry?.service_password_configured || telemetry?.service?.active));
    document.body.classList.toggle('service-mode', selected);
    document.querySelector('#service-warning').hidden = !selected;
    document.querySelector('#service-limits').hidden = !selected;
    const limits = connected ? telemetry?.controllers?.states?.arm?.arm?.limit_switches : null;
    document.querySelector('#service-limits').textContent = ['ZL_MIN','ZR_MIN','X_MIN']
      .map(name => `${name}: ${typeof limits?.[name] !== 'boolean' ? '?' : limits[name] ? 'ACTIVE' : 'inactive'}`).join(' | ') +
      ` | LEVEL: ${!connected ? '?' : telemetry?.controllers?.states?.arm?.arm?.level_active ? 'ACTIVE' : 'inactive'}`;
  }, 100);
})();
