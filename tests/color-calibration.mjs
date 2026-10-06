// Ejecuta cuerpos reales de conversion y adquisicion con reloj/radio simulados.
// No sustituye la compilacion C++ ni la prueba fisica de los TCS3200.
import fs from 'node:fs';
import vm from 'node:vm';
import assert from 'node:assert/strict';
import { body, convert, install, mathGlobals } from './firmware-harness.mjs';

const read = path => fs.readFileSync(new URL(path, import.meta.url), 'utf8');
const ino = read('../BioIoT_Azure_Integrated.ino');
const math = read('../espnow_split/BioIoT_NodeA_Sensors/SensorMathA.cpp');
const config = read('../espnow_split/BioIoT_NodeA_Sensors/node_a_config.h');
const sampler = read('../espnow_split/BioIoT_NodeA_Sensors/SensorsA.cpp');
const refs = source => Object.fromEntries(['TCS_BLACK_PULSE_US', 'TCS_WHITE_PULSE_US'].map(key => {
  const match = source.match(new RegExp('^#define ' + key + '\\s+(\\d+)UL', 'm'));
  assert.ok(match, key);
  return [key, Number(match[1])];
}));
assert.deepEqual(refs(ino), refs(config));
assert.deepEqual(refs(ino), { TCS_BLACK_PULSE_US: 15, TCS_WHITE_PULSE_US: 200 });

function model(source) {
  const ctx = vm.createContext({ ...mathGlobals, ...refs(ino),
    lroundf: Math.round, fmaxf: Math.max, fminf: Math.min, fmodf: (a, b) => a % b,
    h: NaN, s: NaN, l: NaN });
  install(ctx, source, [['pulseToIntensity', 'pulse'], ['rgbToHsl', 'r,g,b']]);
  return ctx;
}
const original = model(ino), nodeA = model(math);
for (const ctx of [original, nodeA]) {
  for (const [pulse, value] of [[0, 0], [1, 0], [15, 0], [23, 11], [22, 10], [16, 1],
    [142, 175], [192, 244], [153, 190], [200, 255], [1000, 255], [30000, 0]])
    assert.equal(ctx.pulseToIntensity(pulse), value, 'pulse=' + pulse);
  for (const [rgb, h, s, l] of [[[0, 0, 0], 0, 0, 0], [[255, 255, 255], 0, 0, 1],
    [[128, 128, 128], 0, 0, 128 / 255], [[255, 0, 0], 0, 1, .5]]) {
    ctx.rgbToHsl(...rgb);
    assert.ok(Math.abs(ctx.h - h) < 1e-6);
    assert.ok(Math.abs(ctx.s - s) < 1e-6);
    assert.ok(Math.abs(ctx.l - l) < 1e-6);
  }
}
let previous = 0;
for (let pulse = 1; pulse < 30000; pulse++) {
  const value = original.pulseToIntensity(pulse);
  assert.equal(value, nodeA.pulseToIntensity(pulse));
  assert.ok(value >= previous && value >= 0 && value <= 255);
  previous = value;
}

// Ninguna lectura debe ocurrir hasta 10 ms despues del ultimo cambio de filtro.
let clock = 0, lastSwitch = -1, pulses = [];
const globals = {
  TCS_S2: 18, TCS_S3: 19, TCS_OUT_1: 34, TCS_OUT_2: 35, LOW: 0, HIGH: 1,
  kTcsStepGapMs: 10, kTcsPulseTimeoutUs: 30000, NODE_A_COLOR_INTERVAL_MS: 2000,
  int32_t: value => value | 0,
  millis: () => clock, delay: ms => { clock += ms; },
  digitalWrite: () => { lastSwitch = clock; },
  pulseIn: (pin, level, timeout) => {
    assert.equal(level, 0); assert.equal(timeout, 30000);
    assert.ok(clock - lastSwitch >= 10, 'pulseIn antes del asentamiento');
    pulses.push(pin); clock++;
    return pin === 34 ? 15 : 200;
  },
  rgbToHsl: () => {}, pulseToIntensity: original.pulseToIntensity,
  ColorResult: () => ({ rPulse: 0, gPulse: 0, bPulse: 0 }),
};
const readBody = ino.match(/^TcsReading readTcs3200\(int outPin\) \{([\s\S]*?)^\}/m)[1];
const acquisition = vm.createContext({ ...globals, diagnosticColor: [], diagnosticColorSampled: [],
  diagnosticColorAt: [], diagnosticHistory: [], recordDiagnosticHistory: () => {} });
vm.runInContext('function readTcs3200(outPin){' + convert(readBody.replace('TcsReading r;', 'let r = {};')) + '}', acquisition);
assert.equal(acquisition.readTcs3200(34).r, 0);
assert.equal(acquisition.readTcs3200(35).r, 255);
assert.deepEqual(pulses, [34, 34, 34, 35, 35, 35]);

clock = 0; lastSwitch = -1; pulses = [];
const state = vm.createContext({ ...globals, step_: 0, requested_: true, lastCycleMs_: 0,
  sensor_: 0, nextMs_: 0, filterSelected_: false, working_: {}, results_: [], reads_: 0 });
const loopBody = sampler.match(/^void ColorSampler::loop\(uint32_t nowMs\) \{([\s\S]*?)^\}/m)[1];
vm.runInContext('function loop(nowMs){' + convert(loopBody.replace('ColorResult& r = working_;', 'let r = working_;')) + '}', state);
while (clock < 200 && state.reads_ < 2) { state.loop(clock); clock++; }
assert.equal(state.reads_, 2);
assert.equal(state.results_[0].r, 0);
assert.equal(state.results_[1].r, 255);
assert.deepEqual(pulses, [34, 34, 34, 35, 35, 35]);
assert.equal(state.filterSelected_, false);
console.log('PASS: curva 15->0/200->255, limites, captura, HSL y asentamiento antes de medir en integrado/nodo A.');
