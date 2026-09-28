// Conservative serialized-packet envelope; no hardware or cloud access.
import fs from 'node:fs';
import assert from 'node:assert/strict';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const source=fs.readFileSync(path.join(root,'BioIoT_Azure_Integrated.ino'),'utf8');
const limit=Number(source.match(/#define MQTT_PACKET_SIZE\s+(\d+)/)[1]);
const example=JSON.parse(fs.readFileSync(path.join(root,'examples/diagnostic-report.json'),'utf8'));
const worst=structuredClone(example);
const addresses=Array.from({length:126},(_,i)=>'0x'+(i+1).toString(16).toUpperCase().padStart(2,'0'));
worst.i2c.main_bus.devices=addresses;
// null is longer than the actual error codes 0/2/4/5.
worst.i2c.main_bus.probe_codes=Array(126).fill(null);
for(const channel of Object.values(worst.i2c.tca9548a.channels)){
  channel.devices=addresses;
  channel.select_error_name='address_nack';
  channel.expected_probe_error_name='address_nack';
  channel.status='tca_channel_select_failed';
  channel.i2c_downstream_status='tca_channel_select_failed';
  channel.reason='warming_up';
  channel.remaining_ms_at_start=180000;
  channel.remaining_ms=180000;
  worst.assessment.findings.push('tca_ch'+channel.channel+'_expected_device_missing',
    'tca_ch'+channel.channel+'_unexpected_device_found','tca_ch'+channel.channel+'_select_failed',
    'tca_ch'+channel.channel+'_attribution_uncertain');
}
for(const h of Object.values(worst.history.devices)){
  for(const key of Object.keys(h))
    h[key]=key.endsWith('_utc')?'2038-01-01T00:00:00Z':4294967295;
}
for(const key of Object.keys(worst.history))if(key!=='devices')worst.history[key]=4294967295;
for(const ch of Object.values(worst.analog_mux.channels)){
  ch.raw_min=4095;ch.raw_max=4095;ch.raw_avg=4094.9375;ch.raw_span=4095;
  ch.voltage_avg=3.2999999;ch.sampled_at_ms=4294967295;ch.connection_confidence='plausible_signal';
}
worst.deviceId='d'.repeat(128);
worst.experiment_id='e'.repeat(256);
worst.uptimeMs=4294967295;
worst.report_id=4294967295;
const packetLength=value=>Buffer.byteLength(JSON.stringify(value))+255+7; // maximal topic capacity
assert.ok(packetLength(worst)<=limit, 'Conservative packet '+packetLength(worst)+' exceeds '+limit);
assert.match(source,/5 \+ 2 \+ strlen\(telemetryTopic\) \+ pendingDiagnosticPayload.length\(\) > MQTT_PACKET_SIZE/);
assert.match(source,/measureJson\(report\)/);
assert.match(source,/report\.overflowed\(\)/);
assert.match(source,/ESP\.getFreeHeap\(\) < 90000UL/);
console.log(JSON.stringify({examplePayloadBytes:Buffer.byteLength(JSON.stringify(example)),
  conservativePacketBytes:packetLength(worst),mqttBuffer:limit,
  note:'Synthetic envelope; arbitrary longer configuration still checked at runtime'},null,2));
