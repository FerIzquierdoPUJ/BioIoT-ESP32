import fs from 'node:fs';
import vm from 'node:vm';
import assert from 'node:assert/strict';
import {install,mathGlobals} from './firmware-harness.mjs';
const source=fs.readFileSync(new URL('../BioIoT_Azure_Integrated.ino',import.meta.url),'utf8');
const model=fs.readFileSync(new URL('../CalibrationModel.h',import.meta.url),'utf8');
export function makeJsonContext(example) {
  const ctx=vm.createContext({...mathGlobals,
    DEVICE_ID:example.deviceId,EXPERIMENT_ID:example.experiment_id,
    calibrationStorageOk:true,EXPECT_CO2_1:true,EXPECT_CO2_2:true,EXPECT_O2_1:true,EXPECT_O2_2:true,
    cal:{version:0,schema:2,ph_m:-6.112459,ph_b:15.012668,ph_user:false,do_user:false,
      turb_m:-2310.5,turb_b:5435.3999,do_m:.13351912,temp_m:.9741,temp_b:1.0038,co2_a:0,co2_b:0,co2_enabled:false},
    co2Cal:[mathGlobals.Co2Calibration(),mathGlobals.Co2Calibration()],
    o2Cal:[mathGlobals.O2Calibration(),mathGlobals.O2Calibration()],
    o2Readings:[0,1].map(i=>({observed:true,i2cDetected:i===0,connected:i===0,warming:true,valid:false,
      stabilizing:false,value:NaN,raw:NaN,filtered:NaN,sampledAt:120000,wireError:i?2:0,readCount:0,
      quality:i?'communication_fault':'warming_up'})),
    o2Filter1:{count:0},o2Filter2:{count:0},O2_ADDR_1:0x73,O2_ADDR_2:0x73,
    diagnosticAddressText:n=>'0x'+n.toString(16).toUpperCase().padStart(2,'0'),
    writeDiagnosticUtc:(target,value)=>target.set(value?new Date(value*1000).toISOString().replace('.000',''):null)
  });
  install(ctx,model,[['validCo2Calibration','c'],['evaluateCo2','adcVoltage,connected,c']]);
  install(ctx,source,[['co2Mode','index'],['co2Source','index'],['o2CalibrationSource','index'],
    ['writeCo2Calibration','o,index'],['calibrationJson',''],
    ['writeCo2Reading','o,index,reading,detailed'],['co2Json','index,reading'],
    ['writeO2Reading','o,index,detailed'],['o2Json','index']]);
  return ctx;
}
const example=JSON.parse(fs.readFileSync(new URL('../examples/telemetry-v1.json',import.meta.url),'utf8'));
const ctx=makeJsonContext(example);
const calibration=JSON.parse('{'+ctx.calibrationJson()+'}').calibration;
const normal=structuredClone(example);
normal.calibration=calibration;
for(const key of ['ph','turbidity']) normal.sensors[key].quality='out_of_range';
normal.diagnostics.i2c={...normal.diagnostics.i2c,tca_detected:true,last_error:0,last_error_name:'ok',main_bus_isolated:true,consecutive_failures:0};
normal.alerts=['light_1_disconnected','light_2_disconnected','o2_gas_2_disconnected'];
for(let i=0;i<2;i++){
  normal.sensors['co2_'+(i+1)]=JSON.parse('{'+ctx.co2Json(i,normal.sensors['co2_'+(i+1)])+'}')['co2_'+(i+1)];
  normal.sensors['o2_gas_'+(i+1)]=JSON.parse('{'+ctx.o2Json(i)+'}')['o2_gas_'+(i+1)];
}
assert.equal(calibration.ph.source,'experimental_3point');
assert.equal(calibration.do.source,'experimental_2point');
assert.equal(normal.sensors.co2_1.provisional,true);
assert.equal(normal.sensors.o2_gas_1.connected,true);
assert.equal(normal.sensors.o2_gas_1.measurement_valid,false);
assert.equal(normal.sensors.o2_gas_2.quality,'communication_fault');
ctx.cal.ph_user=true;ctx.cal.do_user=true;
const custom=JSON.parse('{'+ctx.calibrationJson()+'}').calibration;
assert.equal(custom.ph.source,'user_calibrated');assert.equal(custom.ph.r2,undefined);
assert.equal(custom.do.source,'user_calibrated');assert.equal(custom.do.r2,undefined);
ctx.cal.ph_user=false;ctx.cal.do_user=false;
const diagnostic=JSON.parse(fs.readFileSync(new URL('../examples/diagnostic-report.json',import.meta.url),'utf8'));
diagnostic.calibration=calibration;
for(let i=0;i<2;i++){
  const ch=diagnostic.analog_mux.channels[String(i+1)];
  const r={raw:Math.trunc(ch.raw_avg),voltage:Math.trunc(ch.raw_avg)*3.3/4095,connected:ch.raw_avg>5&&ch.raw_avg<4090};
  const co=mathGlobals.JsonDocument();ctx.writeCo2Reading(co.to('JsonObject'),i,r,true);
  ch.measurement=JSON.parse(JSON.stringify(co));
  ctx.o2Readings[i]={...ctx.o2Readings[i],warming:false,connected:false,i2cDetected:false,quality:'communication_fault',sampledAt:diagnostic.uptimeMs};
  const ox=mathGlobals.JsonDocument();ctx.writeO2Reading(ox.to('JsonObject'),i,true);
  diagnostic['o2_gas_'+(i+1)]=JSON.parse(JSON.stringify(ox));
}
for(const ch of Object.values(diagnostic.i2c.tca9548a.channels)){
  ch.warming_up=false;ch.measurement_testable=true;ch.expected_address=ch.expected;
  ch.detected_addresses=[];ch.address_mismatch=false;ch.address_0x72_conflicts_with_tca=false;
}
const h=diagnostic.history;
for(const operation of ['probe','select','disable']){
  h['tca_'+operation+'_success']=0;h['tca_'+operation+'_failure']=operation==='select'?0:2;
  h['tca_'+operation]={...h.devices.i2c_tca,success_count:0,failure_count:operation==='select'?0:2};
}
diagnostic.assessment.findings=[...new Set([...diagnostic.assessment.findings,'o2_gas_1_communication_fault','o2_gas_2_communication_fault'])];
if(process.argv.includes('--emit')) console.log(JSON.stringify({normal,diagnostic}));
else console.log('PASS: source JSON serializers, provenance, provisional CO2, connected warm-up, communication faults and complete fixtures.');

