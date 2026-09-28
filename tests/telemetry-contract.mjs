// Host-side contract checks; no network or ESP32 access.
// Executes the sketch's string-building expressions with mocked readings.
// This is NOT a C++ runtime or a replacement for arduino-cli compile.
// ArduinoJson string escaping is represented by JSON.stringify.
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import assert from 'node:assert/strict';
import {makeJsonContext} from './calibration-json.mjs';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const sketch = fs.readFileSync(path.join(root, 'BioIoT_Azure_Integrated.ino'), 'utf8');
const config = fs.readFileSync(path.join(root, 'iot_configs.h'), 'utf8');
// Never print or evaluate credentials.
const deviceId = JSON.parse(config.match(/^#define\s+DEVICE_ID\s+("(?:[^"\\]|\\.)*")/m)[1]);
const experimentId = JSON.parse(config.match(/^#define\s+EXPERIMENT_ID\s+("(?:[^"\\]|\\.)*")/m)[1]);
const example = JSON.parse(fs.readFileSync(path.join(root, 'examples/telemetry-v1.json'), 'utf8'));
const jsonContext = makeJsonContext(example);
const packetLimit = Number(sketch.match(/#define\s+MQTT_PACKET_SIZE\s+(\d+)/)[1]);
const topicCapacity = Number(sketch.match(/char telemetryTopic\[(\d+)\]/)[1]);
const expectedTopic = 'devices/' + deviceId + '/messages/events/%24.ct=application%2Fjson&%24.ce=utf-8';

function body(name) {
  // Sketch functions close with an unindented brace.
  const match = sketch.match(new RegExp(
    '(?:String|void|const char\\*) ' + name + '\\([^;]*?\\)\\s*\\{([\\s\\S]*?)^\\}', 'm'));
  assert.ok(match, 'Function found: ' + name);
  return match[1];
}

function toJs(code) {
  let text = code.replace(/\/\/[^\n]*/g, '')
    .replace(/\b\w+\.reserve\([^)]*\);/g, '') // capacity hint only; no JSON effect
    .replace(/\bDEVICE_ID\b/g, JSON.stringify(deviceId))
    .replace(/\b(?:const\s+)?(?:String|bool)\s+(\w+)\s*=/g, 'let $1 =')
    .replace(/\bString\s+(\w+)\s*;/g, 'let $1 = "";')
    .replace(/(\w+)\.length\(\)/g, 'byteLength($1)')
    .replace(/\.c_str\(\)/g, '');
  // C++ concatenates adjacent string literals after expanding DEVICE_ID.
  const adjacent = /("(?:[^"\\]|\\.)*")\s+("(?:[^"\\]|\\.)*")/g;
  while (adjacent.test(text)) { adjacent.lastIndex = 0; text = text.replace(adjacent, '$1 + $2'); }
  return text;
}

const s = example.sensors;
const context = vm.createContext({
  String: (value, decimals) => decimals === undefined ? String(value) : Number(value).toFixed(decimals),
  byteLength: value => Buffer.byteLength(value),
  isnan: Number.isNaN, isinf: value => Math.abs(value) === Infinity, isfinite: Number.isFinite,
  jsonStringValue: JSON.stringify,
  EXPERIMENT_ID: experimentId,
  serviceActuators: () => {},
  WiFi: { RSSI: () => example.wifiRssi, status: () => 1 },
  WL_CONNECTED: 1, mqttClient: { connected: () => true },
  actuatorsJson: () => JSON.stringify(example.actuators),
  quickDiagnosticsJson: () => JSON.stringify(example.diagnostics),
  calibrationJson: () => jsonContext.calibrationJson(),
  co2Json: (index, reading) => jsonContext.co2Json(index, reading),
  o2Json: index => jsonContext.o2Json(index),
  nowMs: example.uptimeMs, timestampUtc: example.timestampUtc,
  alerts: example.alerts.map(JSON.stringify).join(','),
  ph: s.ph, co21: s.co2_1, co22: s.co2_2, turb: s.turbidity,
  dor: s.dissolved_oxygen, tds: s.tds,
  phCal: s.ph.value ?? NaN, co21Cal: s.co2_1.value ?? NaN, co22Cal: s.co2_2.value ?? NaN,
  turbCal: s.turbidity.value ?? NaN, doSatPct: s.dissolved_oxygen.saturation_pct ?? NaN,
  doMgL: s.dissolved_oxygen.value ?? NaN, tdsCal: s.tds.value ?? NaN,
  tempConnected: s.temperature.connected, tempRaw: s.temperature.raw, tempCal: s.temperature.value ?? NaN,
  bh1Connected: s.light_1.connected, bh2Connected: s.light_2.connected,
  lux1: s.light_1.value ?? NaN, lux2: s.light_2.value ?? NaN,
  warming: s.o2_gas_1.warming_up,
  o2_1_connected: s.o2_gas_1.connected, o2_2_connected: s.o2_gas_2.connected,
  o2_1_pct: s.o2_gas_1.value ?? NaN, o2_2_pct: s.o2_gas_2.value ?? NaN,
  tcs1: s.color_1, tcs2: s.color_2,
  cal: {
    version: example.calibration.version,
    ph_m: example.calibration.ph.m, ph_b: example.calibration.ph.b,
    turb_m: example.calibration.turb.m, turb_b: example.calibration.turb.b,
    do_m: example.calibration.do.m, temp_m: example.calibration.temp.m, temp_b: example.calibration.temp.b,
    co2_enabled: example.calibration.co2.enabled, co2_a: example.calibration.co2.a, co2_b: example.calibration.co2.b
  }
});
for (const [, name, value] of sketch.matchAll(/const bool (EXPECT_\w+)\s*=\s*(true|false);/g))
  context[name] = value === 'true';
for (const [name, args] of [
  ['jsonNumberOrNull', 'value, decimals = 3'], ['sensorQuality', 'connected, validValue, warmingUp = false'],
  ['analogJson', 'name, unit, r, value, expected'], ['colorJson', 'name, t, expected']
]) vm.runInContext('function ' + name + '(' + args + '){' + toJs(body(name)) + '}', context);
const assembly = body('buildTelemetryJson').slice(body('buildTelemetryJson').indexOf('String json ='));
const emit = () => vm.runInContext('(function(){' + toJs(assembly) + '})()', context);
const raw = emit();
const actual = JSON.parse(raw); // rejects broken commas, quoting, NaN and "inf".
const expected = { ...example, deviceId, experiment_id: experimentId };
assert.deepEqual(actual, expected, 'Complete snapshot including raw values and calibrations');
assert.deepEqual(Object.keys(actual).slice(0, 4), ['schema_version', 'deviceId', 'experiment_id', 'timestampUtc']);
assert.equal(actual.sensors.ph.value, null);
assert.equal(actual.sensors.ph.quality, 'out_of_range');
assert.equal(actual.sensors.light_1.quality, 'disconnected');
assert.equal(actual.sensors.o2_gas_1.quality, 'warming_up');
assert.equal(actual.sensors.temperature.quality, 'good');
assert.ok(Object.values(actual.sensors).every(sensor => typeof sensor.quality === 'string'));
assert.ok(Object.values(actual.sensors).every(sensor => sensor.value !== 'null'));
const snapshotBytes = Buffer.byteLength(raw);
assert.ok(snapshotBytes + Buffer.byteLength(expectedTopic) + 7 <= packetLimit);
assert.ok(Buffer.byteLength(expectedTopic) + 1 <= topicCapacity);
assert.ok(Buffer.byteLength('%24.ct=application%2Fjson&%24.ce=utf-8') <= 80);

// Missing clock, UTF-8/escaped identifiers, non-finite values, and quality precedence.
context.EXPERIMENT_ID = 'Ensayo "área" \\ A\nB';
context.timestampUtc = '';
context.warming = false;
context.o2_1_connected = true;
context.o2_1_pct = 20.9;
jsonContext.o2Readings[0] = {...jsonContext.o2Readings[0],connected:true,valid:true,warming:false,value:20.9,quality:'good'};
jsonContext.o2Readings[1] = {...jsonContext.o2Readings[1],warming:false,quality:'communication_fault'};
context.doMgL = Infinity;
context.tcs1 = { ...context.tcs1, connected: false };
const edge = JSON.parse(emit());
assert.equal(edge.experiment_id, context.EXPERIMENT_ID);
assert.equal(edge.timestampUtc, null);
assert.equal(edge.sensors.dissolved_oxygen.value, null);
assert.equal(edge.sensors.dissolved_oxygen.quality, 'out_of_range');
assert.equal(edge.sensors.o2_gas_1.quality, 'good');
assert.equal(edge.sensors.o2_gas_2.quality, 'communication_fault');
assert.equal(edge.sensors.color_1.quality, 'disconnected');
assert.equal(vm.runInContext('sensorQuality(false, true, true)', context), 'warming_up');
assert.equal(vm.runInContext('sensorQuality(false, true, false)', context), 'disconnected');

// The actual SDK and native C++ compilation are separate checks.
const init = body('initAzureSDK');
assert.equal((init.match(/az_iot_hub_client_telemetry_get_publish_topic\s*\(/g) || []).length, 1);
assert.match(init, /AZ_SPAN_FROM_STR\(AZ_IOT_MESSAGE_PROPERTIES_CONTENT_TYPE\)/);
assert.match(init, /AZ_SPAN_FROM_STR\(AZ_IOT_MESSAGE_PROPERTIES_CONTENT_ENCODING\)/);
assert.match(init, /AZ_SPAN_FROM_STR\("application%2Fjson"\)/);
assert.match(init, /AZ_SPAN_FROM_STR\("utf-8"\)/);
assert.equal((sketch.match(/Serial\.println\(telemetryTopic\)/g) || []).length, 1);
assert.equal((sketch.match(/mqttClient\.publish\(/g) || []).length, 4);
assert.match(body('publishDiagnosticReport'), /mqttClient\.publish\(telemetryTopic, pendingDiagnosticPayload\.c_str\(\)\)/);
for (const name of ['publishTelemetry', 'publishActuatorReport'])
  assert.match(body(name), /mqttClient\.publish\(telemetryTopic, payload\.c_str\(\)\)/);
assert.doesNotMatch(sketch, /setInsecure\s*\(/);
assert.match(sketch, /sslClient\.setCACert\(ca_pem\)/);

// Exercise the two publication paths using the same source and mocked MQTT.
const sent = [];
const logs = [];
context.EXPERIMENT_ID = experimentId;
context.telemetryTopic = expectedTopic;
context.MQTT_PACKET_SIZE = packetLimit;
context.strlen = value => Buffer.byteLength(value);
context.Serial = { print: value => logs.push(value), println: value => logs.push(value) };
context.buildTelemetryJson = emit;
context.utcTimestampIso8601 = () => context.timestampUtc;
context.mqttClient.publish = (topic, payload) => { sent.push({ topic, payload }); return true; };
context.actuatorReportSent = () => {};
const publishSample = () => vm.runInContext('(function(){' + toJs(body('publishTelemetry')) + '})()', context);
const reportBody = body('publishActuatorReport');
const reportAssembly = reportBody.slice(reportBody.indexOf('String payload ='));
const publishReport = () => vm.runInContext('(function(){' + toJs(reportAssembly) + '})()', context);
publishSample();
publishReport();
assert.equal(sent.length, 2);
assert.ok(sent.every(message => message.topic === expectedTopic));
assert.equal(sent[0].payload, emit(), 'The original JSON string is published unchanged');
const report = JSON.parse(sent[1].payload);
assert.equal(report.schema_version, '1.0');
assert.equal(report.experiment_id, experimentId);
assert.equal(report.type, 'actuator_state');
assert.equal(report.timestampUtc, null);
assert.deepEqual(report.actuators, example.actuators);
assert.ok(Buffer.byteLength(sent[1].payload) + Buffer.byteLength(expectedTopic) + 7 <= packetLimit);
// Deliberately excessive configuration is rejected before publish; no truncation.
context.EXPERIMENT_ID = 'X'.repeat(packetLimit + 1);
publishSample();
publishReport();
assert.equal(sent.length, 2, 'Neither path publishes an oversized packet');
assert.ok(logs.includes('Telemetria excede el buffer MQTT; no enviada.'));
assert.ok(logs.includes('Reporte de actuadores excede el buffer MQTT; no enviado.'));
console.log(JSON.stringify({
  result: 'PASS: source-assembly contract checks (mocked inputs, not ESP32 execution)',
  topic: expectedTopic, topicBytes: Buffer.byteLength(expectedTopic),
  payloadBytes: snapshotBytes, packetBytesUpperBound: snapshotBytes + Buffer.byteLength(expectedTopic) + 7,
  packetBuffer: packetLimit, sensors: Object.keys(actual.sensors).length
}, null, 2));
