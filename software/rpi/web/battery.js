(() => {
  const soc = document.querySelector('#battery-soc');
  const gauge = document.querySelector('#battery-gauge');
  const electrical = document.querySelector('#battery-electrical');
  const status = document.querySelector('#battery-status');
  const alert = document.querySelector('#battery-alert');
  const strip = document.querySelector('.battery-strip');
  let lastCritical = false;
  const descriptions = {
    not_configured: 'SmartShunt: not configured', configuration_error: 'SmartShunt: configuration error',
    waiting: 'SmartShunt: waiting for Bluetooth data', bluetooth_unavailable: 'SmartShunt: Bluetooth unavailable',
    dependency_missing: 'SmartShunt: Bluetooth software missing', decode_error: 'SmartShunt: cannot decode data; check key',
    stale: 'SmartShunt: signal lost', soc_unknown: 'SmartShunt: charge unknown; check synchronization',
    offline: 'SmartShunt: server unavailable', live: 'SmartShunt: live',
  };
  function render(data) {
    const valid = data.fresh && Number.isFinite(data.soc_pct);
    if (valid) lastCritical = data.level === 'critical';
    else if (data.last_known_level === 'critical') lastCritical = true;
    soc.textContent = valid ? `${data.soc_pct.toFixed(1)}%` : '--%';
    gauge.hidden = !valid;
    if (valid) gauge.value = data.soc_pct;
    electrical.textContent = data.fresh ? [
      Number.isFinite(data.voltage_v) ? `${data.voltage_v.toFixed(2)} V` : '',
      Number.isFinite(data.current_a) ? `${data.current_a.toFixed(2)} A` : '',
    ].filter(Boolean).join(' / ') : '';
    status.textContent = descriptions[data.status] || 'SmartShunt: unavailable';
    const minutes = data.remaining_minutes;
    const estimating = valid && Number.isFinite(data.current_a) && data.current_a < 0 &&
      Number.isFinite(minutes) && minutes >= 0;
    document.querySelector('#battery-runtime').textContent = estimating ?
      `Remaining: ~${Math.floor(minutes / 60)} h ${Math.floor(minutes % 60)} min` :
      valid && data.current_a > 0 ? 'Charging' : 'Remaining: --';
    strip.dataset.level = valid ? data.level : 'unknown';
    let message = '';
    let level = valid ? data.level : 'unknown';
    if (valid && level === 'critical') message = `Battery critically low (${data.soc_pct.toFixed(1)}%). Stop the robot, shut down the Raspberry Pi, then switch off the power.`;
    else if (valid && level === 'low') message = `Battery low (${data.soc_pct.toFixed(1)}%). Recharge the battery.`;
    else if (!valid && lastCritical) {
      message = 'Battery data unavailable. Last known charge was critical. Stop the robot, shut down the Raspberry Pi, then switch off the power.';
      level = 'critical';
    } else if (!valid && data.status !== 'not_configured') message = `${status.textContent}. Battery charge cannot be verified.`;
    alert.dataset.level = level;
    if (alert.textContent !== message) alert.textContent = message;
    alert.hidden = !message;
  }
  async function poll() {
    try {
      const response = await fetch('/api/battery', {cache: 'no-store', signal: AbortSignal.timeout(4000)});
      if (!response.ok) throw new Error('battery unavailable');
      render(await response.json());
    } catch (_) { render({status: 'offline', fresh: false}); }
    finally { window.setTimeout(poll, 2000); }
  }
  poll();
})();
