// Executes selected firmware control-flow expressions with mocked hardware.
// Complements, but does not replace, native ESP32 compilation and bench tests.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const source = fs.readFileSync(path.join(root, 'BioIoT_Azure_Integrated.ino'), 'utf8');
function body(name) {
  const match = source.match(new RegExp(
    '(?:void|bool|uint8_t|const char\\*|I2CProbeResult|TcaSelectResult) ' + name +
    '\\([^;]*?\\)\\s*\\{([\\s\\S]*?)^\\}', 'm'));
  assert.ok(match, name);
  return match[1];
}
function convert(code) {
  return code.replace(/\/\/[^\n]*/g, '')
    .replace(/for \(auto &scan : diagnosticChannelScans\) scan = I2CScanSegment\(\);/g,
      'diagnosticChannelScans = Array.from({length:8}, I2CScanSegment);')
    .replace(/\b(?:const\s+)?(?:auto|uint8_t|uint32_t|bool|int|I2CProbeResult|TcaSelectResult|TcsReading)\s*&?\s*(\w+)\s*=/g, 'let $1 =')
    .replace(/\bI2CProbeResult (\w+);/g, 'let $1 = I2CProbeResult();')
    .replace(/\bTcaSelectResult (\w+);/g, 'let $1 = TcaSelectResult();')
    .replace(/\bbool (\w+);/g, 'let $1;')
    .replace(/\(unsigned long\)/g, '')
    .replace(/(\d+)UL\b/g, '$1');
}
function make(scenario = {}) {
  let clock = scenario.warm ? 1000 : 200000;
  let address = 0, mask = 0, write = null, rootPresent = scenario.tca !== false;
  const probes = [], writes = [];
  const channels = scenario.channels ?? [[0x23], [0x23], [0x73], [0x73], [], [], [], []];
  const ctx = vm.createContext({
    DIAG_IDLE:0, DIAG_BEGIN:1, DIAG_MAIN:2, DIAG_SELECT:3, DIAG_CHANNEL:4,
    DIAG_ANALOG_READ:5, DIAG_COLOR_READ:6, DIAG_TEMP_READ:7, DIAG_READY:8, DIAG_O2_READ:9,
    DIAG_FULL:0, DIAG_I2C:1, DIAG_ANALOG:2, DIAG_COLOR:3, DIAG_SYSTEM:4, DIAG_QUICK:5, DIAG_RECOVER:6,
    diagnosticRequestPending:true, diagnosticScope:1, diagnosticPhase:0,
    diagnosticStartedAt:0, diagnosticCompletedAt:0, diagnosticWarmup:false, diagnosticWarmupRemaining:0,
    diagnosticSerialPrinted:false, diagnosticReportSequence:0, pendingDiagnosticPayload:'',
    diagnosticAddress:1, diagnosticChannel:0, diagnosticIndex:0, diagnosticBusDirty:false,
    recoveryBefore:{}, recoveryAfter:{}, recoveryEndOk:false, recoveryBeginOk:false,
    diagnosticTca:{}, diagnosticIsolation:{}, diagnosticCleanup:{},
    TCA_ADDR:0x72, TCA_CH_BH1750_1:0, TCA_CH_BH1750_2:1, TCA_CH_O2_1:2, TCA_CH_O2_2:3,
    BH1750_ADDR_1:0x23, BH1750_ADDR_2:0x23, O2_ADDR_1:0x73, O2_ADDR_2:0x73,
    I2C_SDA_PIN:21, I2C_SCL_PIN:22, TCS_OUT_1:34, TCS_OUT_2:35, O2_WARMUP_MS:180000, bootMillis:0,
    uint8_t: value => value, millis: () => clock, time: () => 0,
    diagnosticHistory:Array.from({length:9},()=>({success:0,failure:0})),
    tcaProbeHistory:{success:0,failure:0},tcaSelectHistory:{success:0,failure:0},tcaDisableHistory:{success:0,failure:0},
    recordDiagnosticHistory:(h,ok)=>{h[ok?'success':'failure']++;},
    lastTcaProbe:{}, lastTcaObserved:false, lastTcaProbeAt:0,
    I2CProbeResult:()=>({address:0,detected:false,errorCode:0}),
    TcaSelectResult:()=>({attempted:false,selected:false,channel:0,errorCode:0}),
    I2CScanSegment:()=>({codes:Array(127).fill(0),tested:Array(127).fill(false),
      selection:{attempted:false,selected:false,errorCode:0},completed:false}),
    Serial:{printf:()=>{},println:()=>{}},
    Wire:{
      beginTransmission:a=>{address=a;write=null;}, write:v=>{write=v;},
      endTransmission:()=>{
        if (write !== null) {
          writes.push(write);
          assert.ok(write === 0 || (write & (write-1)) === 0, 'Only one TCA channel selected');
          if (!rootPresent) return 2;
          if (scenario.selectFail === Math.log2(write)) return 5;
          mask=write; return 0;
        }
        probes.push({address,mask});
        if (address === 0x72) return rootPresent ? 0 : (scenario.rootError ?? 2);
        const index = mask ? Math.log2(mask) : -1;
        return index >= 0 && channels[index].includes(address) ? 0 : 2;
      },
      end:()=>{mask=0;return true;},
      begin:()=>{rootPresent=true;return true;}
    },
    readAnalogMux:()=>{},readTcs3200:()=>{},readRawTemperatureC:()=>{},readO2Percent:()=>{},
    o2IsWarmingUp:()=>clock<180000, ANALOG_SIMILAR_SPREAD:8,
    analogBatchValid:false,analogAllNearZero:false,analogTooSimilar:false,
    analogMinAverage:0,analogMaxAverage:0,analogAllZeroCount:0,
    analogDiagnostics:Array.from({length:6},()=>({sampled:true,rawAvg:0})),
    fminf:Math.min,fmaxf:Math.max,
    diagnosticColorSampled:[true,true],
    diagnosticColor:[{rPulse:0,gPulse:0,bPulse:0,connected:false},{rPulse:13615,gPulse:15889,bPulse:14736,connected:true}]
  });
  ctx.diagnosticMainScan=ctx.I2CScanSegment();
  ctx.diagnosticChannelScans=Array.from({length:8},ctx.I2CScanSegment);
  for(const [name,args] of [
    ['i2cErrorName','code'],['probeI2C','address'],['tcaSelectDetailed','channel'],['tcaDisableAll',''],
    ['scanMainI2CBus',''],['scanTcaChannels',''],['diagnosticNextGroup',''],['diagnosticExpectedAddress','channel'],
    ['diagnosticScopeName','scope'],['processDiagnostics',''],['updateAnalogDiagnostics',''],
    ['diagnosticColorPulseOk','index'],['diagnosticColorAllZero','index']
  ]) vm.runInContext('function '+name+'('+args+'){'+convert(body(name))+'}',ctx);
  return {ctx, probes, writes, mask:()=>mask,
    tick:()=>{vm.runInContext('processDiagnostics()',ctx);clock++;},
    interrupt:()=>{mask=0;ctx.diagnosticBusDirty=true;}};
}
function run(model, interrupted=false){
  for(let n=0;n<2000 && model.ctx.diagnosticPhase!==8;n++){
    if(interrupted && n===200) model.interrupt(); // normal telemetry changes the mux
    model.tick();
  }
  assert.equal(model.ctx.diagnosticPhase,8,'Finite staged scan');
  assert.equal(model.mask(),0,'All channels disabled at end');
}
const healthy=make();run(healthy,true);
assert.equal(healthy.ctx.diagnosticTca.detected,true);
for(let ch=0;ch<8;ch++) assert.equal(healthy.ctx.diagnosticChannelScans[ch].completed,true);
for(let ch=0;ch<4;ch++) assert.equal(healthy.ctx.diagnosticChannelScans[ch].codes[ch<2?0x23:0x73],0);
const noTca=make({tca:false});run(noTca);
assert.equal(noTca.ctx.diagnosticTca.errorCode,2);
assert.ok(noTca.ctx.diagnosticChannelScans.every(scan=>!scan.selection.attempted));
assert.equal(noTca.probes.length,126,'No downstream probing if TCA unavailable');
const empty=make({channels:Array.from({length:8},()=>[])});run(empty);
for(let ch=0;ch<4;ch++) assert.equal(empty.ctx.diagnosticChannelScans[ch].codes[ch<2?0x23:0x73],2);
const failedSelection=make({selectFail:0});run(failedSelection);
assert.equal(failedSelection.ctx.diagnosticChannelScans[0].selection.errorCode,5);
assert.equal(failedSelection.ctx.diagnosticChannelScans[0].completed,false);
const warm=make({warm:true});run(warm);
for(let ch=2;ch<=3;ch++) assert.equal(warm.ctx.diagnosticChannelScans[ch].tested[0x73],true);
assert.ok(warm.probes.some(p=>p.mask===4&&p.address===0x73));
assert.ok(warm.probes.some(p=>p.mask===8&&p.address===0x73));
assert.equal(healthy.ctx.tcaSelectHistory.success,9); // eight channels + restore after telemetry
assert.equal(healthy.ctx.tcaDisableHistory.success,2);
assert.equal(healthy.ctx.diagnosticHistory[0].success,healthy.ctx.tcaProbeHistory.success);
const recovered=make({tca:false});recovered.ctx.diagnosticScope=6;run(recovered);
assert.equal(recovered.ctx.recoveryBefore.errorCode,2);
assert.equal(recovered.ctx.recoveryAfter.errorCode,0);

const c=healthy.ctx;
for(const values of [[0,0,0,0,0,0],[117,118,116,117,113,114],[100,900,1700,2300,3000,3900]]){
  c.analogDiagnostics=values.map(rawAvg=>({sampled:true,rawAvg}));
  vm.runInContext('updateAnalogDiagnostics()',c);
  assert.equal(c.analogAllNearZero,values[0]===0);
  assert.equal(c.analogTooSimilar,values[0]!==100);
}
assert.equal(c.analogAllZeroCount,1);
assert.equal(vm.runInContext('diagnosticColorAllZero(0) && diagnosticColorPulseOk(1)',c),true);
assert.match(source,/"tcs1_power_oe_out_or_gpio34"/);
assert.match(source,/"color_1_individual_path_suspected"/);
assert.match(source,/"multiple_tca_channels_missing_devices"/);
assert.match(source,/channel\["expected_found"\] = nullptr/);
for(const [code,name] of [[0,'ok'],[1,'data_too_long'],[2,'address_nack'],[3,'data_nack'],[4,'other_error'],[5,'timeout'],[42,'unknown']])
  assert.equal(vm.runInContext('i2cErrorName('+code+')',c),name);
const callback=body('mqttCallback');
const branch=callback.match(/if \(strcmp\(action, "diagnostics"\) == 0\) \{([\s\S]*?)\n  \}/)[1];
assert.match(branch,/requestDiagnostics/);
assert.doesNotMatch(branch,/publish|scanMain|scanTca|processDiagnostics|probeI2C/);
assert.doesNotMatch(callback,/mqttClient\.publish/);
assert.match(source,/#define TELEMETRY_INTERVAL_MS\s+120000UL/);
assert.match(source,/#define O2_WARMUP_MS\s+180000UL/);
console.log('PASS: staged scans, bus restoration, no TCA, missing devices, select timeout, warm-up, recovery, analog A/B/C evidence, color asymmetry, deferred callback.');
