import fs from 'node:fs';
import vm from 'node:vm';
import assert from 'node:assert/strict';
import {body,convert,install,mathGlobals} from './firmware-harness.mjs';
const ino=fs.readFileSync(new URL('../BioIoT_Azure_Integrated.ino',import.meta.url),'utf8');
const model=fs.readFileSync(new URL('../CalibrationModel.h',import.meta.url),'utf8');
const ctx=vm.createContext({...mathGlobals,cal:{ph_m:-6.112459,ph_b:15.012668,do_m:.13351912}});
install(ctx,model,[['isOldPhDefault','m,b'],['isOldDoDefault','m'],['validCo2Calibration','c'],['evaluateCo2','adcVoltage,connected,c']]);
install(ctx,ino,[['calculatePh','voltage_V'],['calculateDoSaturationPct','voltage_V']]);
assert.ok(Math.abs(ctx.calculatePh(1)-8.900209)<1e-6);
assert.ok(Number.isNaN(ctx.calculatePh(0)));
assert.ok(Math.abs(ctx.calculateDoSaturationPct(.7489564)-100)<.00001);
assert.equal(ctx.isOldPhDefault(-6.1125,15.013),true);
assert.equal(ctx.isOldPhDefault(-6.11249,15.013),false);
const c=mathGlobals.Co2Calibration();
const adc=v=>v*8.5*(22/34);
assert.equal(ctx.evaluateCo2(adc(.220),true,c).quality,'below_reference_range');
assert.ok(Math.abs(ctx.evaluateCo2(adc(.19),true,c).value-1000)<.001);
assert.ok(ctx.evaluateCo2(adc(.18),true,c).value>ctx.evaluateCo2(adc(.19),true,c).value);
assert.equal(ctx.evaluateCo2(.09,true,c).quality,'out_of_range');
assert.equal(ctx.evaluateCo2(.09,false,c).quality,'disconnected');
assert.equal(ctx.evaluateCo2(1,true,{...c,enabled:0}).quality,'uncalibrated');
assert.ok(Math.abs(ctx.evaluateCo2(1,true,{...c,mode:1,a:12.34,b:-.5,user:1,provisional:0}).value-12.34*Math.exp(-.5))<1e-9);
assert.equal(ctx.evaluateCo2(adc(.19),true,{...c,user:1,provisional:0}).quality,'user_calibrated');

// Run the actual migration and persistence function bodies against in-memory Preferences.
function migration(initial={},failWrite=false) {
  const db=structuredClone(initial),logs=[];
  const prefs={
    begin:()=>true,end:()=>{},getBytesLength:k=>db[k]?20:0,
    getBytes:(k,target)=>{Object.assign(target,db[k]);return 20;},
    putBytes:(k,value)=>{if(failWrite)return 0;db[k]=structuredClone(value);return 20;}
  };
  for(const kind of ['Float','Bool','UInt','Long64']) {
    prefs['get'+kind]=(key,fallback)=>Object.hasOwn(db,key)?db[key]:fallback;
    prefs['put'+kind]=(key,value)=>{if(failWrite)return 0;db[key]=value;return kind==='Bool'?1:kind==='Long64'?8:4;};
  }
  const ctx=vm.createContext({...mathGlobals,prefs,cal:{version:0},co2Cal:[],o2Cal:[],
    calibrationStorageOk:true,calibrationFutureSchema:false,resetO2Filters:()=>{},
    Serial:{println:v=>logs.push(v),printf:()=>{}},
    PH_CAL_M_DEFAULT:-6.112459,PH_CAL_B_DEFAULT:15.012668,
    DO_CAL_M_DEFAULT:.13351912,TURB_CAL_M_DEFAULT:-2310.5,TURB_CAL_B_DEFAULT:5435.4,
    TEMP_CAL_M_DEFAULT:.9741,TEMP_CAL_B_DEFAULT:1.0038,CO2_CAL_A_DEFAULT:0,CO2_CAL_B_DEFAULT:0});
  install(ctx,model,[['isOldPhDefault','m,b'],['isOldDoDefault','m'],['validCo2Calibration','c']]);
  install(ctx,ino,[['loadCalibrationDefaults',''],['saveCalibrationToNVS',''],['loadCalibrationFromNVS','']]);
  ctx.loadCalibrationFromNVS();
  return {ctx,db,logs};
}
let m=migration({ph_m:-6.1125,ph_b:15.013,do_m:.1335,ver:9});
assert.equal(m.ctx.cal.ph_m,-6.112459);assert.equal(m.ctx.cal.do_m,.13351912);
assert.equal(m.db.schema,2);assert.equal(m.db.ver,9);
assert.equal(m.ctx.co2Cal[0].mode,0);assert.equal(m.ctx.co2Cal[1].enabled,1);
m=migration({ph_m:-5.9,ph_b:14.8,turb_m:-2222,do_m:.12,temp_b:2.3,
  co2_a:12.34,co2_b:-.5,co2_en:true,ver:11});
assert.equal(m.ctx.cal.ph_m,-5.9);assert.equal(m.ctx.cal.ph_user,true);
assert.equal(m.ctx.cal.turb_m,-2222);assert.equal(m.ctx.cal.do_m,.12);assert.equal(m.ctx.cal.temp_b,2.3);
assert.equal(m.ctx.co2Cal[0].mode,1);assert.equal(m.ctx.co2Cal[1].a,12.34);
assert.equal(m.db.co2_a,12.34);assert.equal(m.db.ver,11);
const again=migration(m.db);
assert.deepEqual(again.db,m.db,'Idempotent migration');
m=migration({ph_m:-6.1125,ph_b:15.013,ph_user:true});
assert.equal(m.ctx.cal.ph_m,-6.1125,'Explicit marker preserves even old default numbers');
m=migration({},true);
assert.equal(Boolean(m.ctx.calibrationStorageOk),false);assert.equal(m.db.schema,undefined);
m=migration({schema:99,ph_m:-5.2});
assert.equal(m.ctx.calibrationFutureSchema,true);assert.equal(m.ctx.saveCalibrationToNVS(),false);
m=migration({co2_en:false,co2_a:12,co2_b:-1});
assert.equal(m.ctx.co2Cal[0].mode,0,'Disabled old profile migrates to vendor');
m=migration({c1:{...c,zero_point_v:.225,user:1,provisional:0}});
assert.equal(m.ctx.co2Cal[0].zero_point_v,.225,'Partial migration retains independent blob');
m.ctx.cal.version=25;m.ctx.loadCalibrationDefaults();
assert.equal(m.ctx.cal.version,25);assert.equal(m.ctx.cal.ph_m,-6.112459);
assert.equal(m.ctx.cal.do_m,.13351912);assert.equal(m.ctx.co2Cal[1].provisional,1);

// Execute ring-buffer method, with all state explicitly bound to each instance.
const add=model.match(/float add\(float value\) \{([\s\S]*?)\n  \}/)[1];
const js=convert(add).replace(/\b(samples|count|next)\b/g,'this.$1');
const filter=()=>({samples:Array(10).fill(0),count:0,next:0,add:new Function('value','O2_FILTER_SAMPLES',js)});
const f1=filter(),f2=filter();
for(let i=0;i<10;i++){assert.equal(f1.add(10,10),10);assert.equal(f2.add(25,10),25);}
assert.equal(f1.count,10);assert.equal(f2.count,10);
assert.equal(f1.add(20,10),11);assert.equal(f2.add(15,10),24);

assert.match(ino,/DFRobot_OxygenSensor o2Sensor1;/);
assert.match(ino,/DFRobot_OxygenSensor o2Sensor2;/);
assert.match(body(ino,'readO2Percent'),/getOxygenData\(1\)/);
assert.doesNotMatch(ino,/getOxygenData\(10\)/);
assert.match(body(ino,'readO2Percent'),/connected = r.connected = driver.communicationOk\(\)/);
assert.match(body(ino,'processO2Calibration'),/driver.calibrate\(20.9f\)/);
assert.match(body(ino,'readAnalogMux'),/delay\(3\);[\s\S]*analogRead\(MUX_SIG_PIN\);[\s\S]*const int N = 16/);
assert.match(body(ino,'readAnalogMux'),/r.raw \* \(3.3f \/ 4095.0f\)/);
for(const fn of ['mqttCallback','requestO2Calibration','handleCalibrationCommand'])
  assert.doesNotMatch(body(ino,fn),/mqttClient.publish/);
console.log('PASS: pH/DO/CO2 maths, divider, reference quality, independent filters, NVS migration/custom/legacy/idempotency/failure/future schema, reset defaults, deferred calibration.');

// Actual CO2 setter: emulate C++ value-copy candidate and typed JsonVariant access.
ctx.co2Cal=[mathGlobals.Co2Calibration(),mathGlobals.Co2Calibration()];
ctx.structuredClone=structuredClone;
install(ctx,ino,[['calibrationNumber','value']]);
let setter=convert(body(ino,'setCo2Profile'))
  .replace('let candidate = co2Cal[index];','let candidate = structuredClone(co2Cal[index]);')
  .replace(/candidate\.(a|b|zero_point_v|reaction_voltage_v) = cmd\[("[^"]+")\];/g,'candidate.$1 = cmd[$2].as("float");');
vm.runInContext('function setCo2Profile(cmd,index){'+setter+'}',ctx);
const command=data=>{const doc=mathGlobals.JsonDocument();doc.set(data);return doc;};
assert.equal(ctx.setCo2Profile(command({zero_point_v:.225,reaction_voltage_v:.031}),0),true);
assert.equal(ctx.co2Cal[0].zero_point_v,.225);assert.equal(ctx.co2Cal[1].zero_point_v,.220);
assert.equal(ctx.co2Cal[0].provisional,0);
assert.equal(ctx.setCo2Profile(command({a:12.34,b:-.5}),1),true);
assert.equal(ctx.co2Cal[1].mode,1);assert.equal(ctx.co2Cal[0].mode,0);
const saved=JSON.stringify(ctx.co2Cal);
for(const invalid of [{reaction_voltage_v:0},{zero_point_v:null},{a:12,zero_point_v:.23},{a:"12"},{enabled:1}])
  assert.equal(ctx.setCo2Profile(command(invalid),0),false);
assert.equal(JSON.stringify(ctx.co2Cal),saved);
assert.equal(ctx.setCo2Profile(command({enabled:false}),0),true);
assert.equal(Boolean(ctx.co2Cal[0].enabled),false);

// Actual O2 acquisition body with transport mocks; independent state and filtering.
function oxygenModel({warm=false,raw=20.9,ack=true,select=true,transfer=true}={}) {
  const reading=()=>({observed:false,i2cDetected:false,connected:false,warming:false,valid:false,
    stabilizing:false,raw:NaN,filtered:NaN,value:NaN,sampledAt:0,wireError:0,readCount:0,quality:'not_sampled'});
  let acquired=0;
  const driver={begin:()=>transfer,getOxygenData:n=>{assert.equal(n,1);acquired++;return raw;},
    communicationOk:()=>transfer,lastWireError:()=>transfer?0:5,lastReadCount:()=>transfer?3:0};
  const simpleFilter=()=>({count:0,clear(){this.count=0;},add(v){this.count++;return v;}});
  const o=vm.createContext({...mathGlobals,O2Reading:reading,millis:()=>200000,o2IsWarmingUp:()=>warm,
    TCA_CH_O2_1:2,o2Readings:[reading(),reading()],o2Filter1:simpleFilter(),o2Filter2:simpleFilter(),
    o2Sensor1:driver,o2Sensor2:driver,o2Cal:[{gain:1,offset:0},{gain:1,offset:0}],
    i2cReadingDiagnostics:Array.from({length:4},()=>({selection:{errorCode:2},probe:{errorCode:2}})),
    prepareI2CReading:()=>select,probeI2CReading:()=>ack,finishI2CReading:()=>{}});
  let code=body(ino,'readO2Percent')
    .replace(/\b(O2Reading|O2Filter|DFRobot_OxygenSensor) &(\w+) =/g,'auto $2 =')
    .replace('r = O2Reading();','Object.assign(r, O2Reading());');
  vm.runInContext('function read(channel,address){let connected=false;let tcaChannel=channel;'+convert(code)+'}',o);
  o.read(2,0x73);
  return {r:o.o2Readings[0],ctx:o,acquired};
}
let ox=oxygenModel({warm:true});
assert.equal(ox.r.connected,true);assert.equal(ox.r.valid,false);assert.equal(ox.acquired,0);
assert.equal(ox.r.quality,'warming_up');
ox=oxygenModel({warm:true,ack:false});assert.equal(ox.r.quality,'communication_fault');
ox=oxygenModel({raw:31});assert.equal(ox.r.connected,true);assert.equal(ox.r.valid,false);assert.equal(ox.r.quality,'out_of_range');
ox=oxygenModel({raw:NaN});assert.equal(ox.r.connected,true);assert.equal(ox.r.valid,false);
ox=oxygenModel({raw:26});assert.equal(ox.r.valid,true);assert.equal(ox.r.quality,'above_nominal_range');
ox=oxygenModel({raw:20.9});assert.equal(ox.r.valid,true);assert.equal(ox.r.quality,'stabilizing');
assert.equal(ox.ctx.o2Filter2.count,0);
ox=oxygenModel({transfer:false});assert.equal(ox.r.connected,false);assert.equal(ox.r.i2cDetected,true);
assert.doesNotMatch(body(ino,'buildTelemetryJson'),/appendAlert\(alerts,[^\n]*invalid_measurement/);
console.log('PASS: independent CO2 setters, legacy a/b, rejected parameters, O2 warm-up ACK/invalid/read-failure/range semantics.');
