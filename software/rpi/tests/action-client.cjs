"use strict";
// Exercise the actual browser message handler without requiring a browser process.
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const source = fs.readFileSync(path.join(__dirname, "../web/app.js"), "utf8");
const start = source.indexOf("function connect() {");
const end = source.indexOf('\n$("#enable").addEventListener', start);
assert(start >= 0 && end > start);

function setup() {
  const handlers = {};
  const context = vm.createContext({
    wsAddress: "ws://test/ws/control", socket: null, clientId: null, seq: 0,
    telemetry: null, lastTelemetry: 0, homePending: 5, homeOwned: false,
    desiredControl: false, ownershipConfirmed: false, enablePending: null, releasePending: false,
    performance: {now: () => 0}, renderControl() {}, renderRobot() {}, clearInput() {}, notice() {},
    WebSocket: class { addEventListener(type, handler) { handlers[type] = handler; } },
  });
  vm.runInContext(source.slice(start, end) + "\nconnect();", context);
  const send = (data) => handlers.message({data: JSON.stringify(data)});
  send({type: "welcome", client_id: "pc"});
  return {context, send};
}

const complete = {type: "telemetry", owner: null, reason: "action_complete",
  homing: {active_axis: null}, arm_action: {kind: null}};
for (const order of ["ack-first", "telemetry-first"]) {
  const {context, send} = setup();
  if (order === "ack-first") send({type: "ack", seq: 5});
  send(complete);
  if (order === "telemetry-first") send({type: "ack", seq: 5});
  assert.equal(context.homeOwned, false, order);
  assert.equal(context.homePending, null, order);
}
{
  const {context, send} = setup();
  send({type: "ack", seq: 5});
  assert.equal(context.homeOwned, false, "ACK alone must not start keepalives");
  send({type: "telemetry", owner: "pc", homing: {active_axis: "ZL"}, arm_action: {kind: "home"}});
  assert.equal(context.homeOwned, true);
  send(complete);
  assert.equal(context.homeOwned, false);
}
{
  const {context, send} = setup();
  context.homePending = 9;
  send(complete);
  send({type: "ack", seq: 5});
  assert.equal(context.homePending, 9, "Old completion/ACK cannot clear a new pending command");
  assert.equal(context.homeOwned, false);
}
console.log("PASS: 4 browser action ACK/telemetry ordering scenarios");

// Run the actual MOVE click handler: units, selected axis, invalid input.
{
  const begin = source.indexOf('$("#arm-move").addEventListener');
  const finish = source.indexOf('\n$("#arm-configure")', begin);
  assert(begin >= 0 && finish > begin);
  const elements = {
    "#position-axis": {value:"ZL"},
    "#z-accel-value": {value:"10", reportValidity:()=>true},
    "#x-accel-value": {value:"25", reportValidity:()=>true},
    "#arm-move": {addEventListener:(_, handler)=>{ elements.click=handler; }},
  };
  const sent=[];
  vm.runInNewContext(source.slice(begin,finish), {
    $:key=>elements[key], armValue:key=>key==="arm-target"?400000:200000,
    armAction:message=>sent.push(message),
  });
  elements.click();
  assert.equal(sent[0].acceleration_um_s2,10000);
  assert.equal(sent[0].position_um,400000);
  elements["#position-axis"].value="X";
  elements.click();
  assert.equal(sent[1].acceleration_um_s2,25000);
  elements["#x-accel-value"].reportValidity=()=>false;
  elements.click();
  assert.equal(sent.length,2);
  console.log("PASS: MOVE sends selected Z/X acceleration in um/s2 and rejects invalid input");
}
