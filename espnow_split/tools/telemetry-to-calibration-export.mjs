#!/usr/bin/env node
// Respaldo alternativo: convierte un documento de telemetria v4 (Cosmos DB, con o
// sin envoltorio Body, o una linea del monitor serie) en el formato
// "bioiot-calibration-export" v1.
//   node tools/telemetry-to-calibration-export.mjs documento.json > export.json
// LIMITACIONES (se marcan en export_meta): la telemetria v4 imprime floats con ~6-7
// cifras (sin patrones _f32) y NO incluye reference/fecha/conteo de la calibracion
// en aire de O2 ni los flags turb/temp. Para migracion exacta use
// tools/BioIoT_CalibrationExport en la placa v4. Este conversor no modifica nada.
import { readFileSync } from "node:fs";

const input = JSON.parse(readFileSync(process.argv[2] || 0, "utf8"));
const doc = input.Body && typeof input.Body === "object" ? input.Body : input;
const c = doc.calibration;
if (!c || typeof c !== "object") {
  console.error("El documento no contiene 'calibration' (use una telemetria v4 con sensors).");
  process.exit(2);
}
const num = (v, name) => {
  if (typeof v !== "number" || !Number.isFinite(v)) throw new Error(`Campo numerico ausente/invalido: ${name}`);
  return v;
};
const co2 = (o, name) => ({
  zero_point_v: num(o.zero_point_v, `${name}.zero_point_v`),
  reaction_voltage_v: num(o.reaction_voltage_v, `${name}.reaction_voltage_v`),
  a: num(o.a, `${name}.a`),
  b: num(o.b, `${name}.b`),
  mode: o.mode === "legacy_exponential" ? 1 : 0,
  user: o.source === "user_calibrated",
  enabled: Boolean(o.enabled),
  provisional: Boolean(o.provisional),
});
const out = {
  format: "bioiot-calibration-export",
  format_version: 1,
  source: "v4_telemetry_document",
  device_id: doc.deviceId ?? null,
  exported_at_utc: doc.timestampUtc ?? null,
  legacy: { schema: c.schema_version ?? 0, ver: c.version ?? 0 },
  export_meta: {
    decimal_only_precision: "~6-7 cifras significativas (sin _f32)",
    o2_air_calibration_history_missing: true,
    turb_temp_user_flags_unknown: true,
  },
  node_a: {
    ph: { m: num(c.ph?.m, "ph.m"), b: num(c.ph?.b, "ph.b"), user: c.ph?.source === "user_calibrated" },
    turb: { m: num(c.turb?.m, "turb.m"), b: num(c.turb?.b, "turb.b") },
    do: { m: num(c.do?.m, "do.m"), user: c.do?.source === "user_calibrated" },
    temp: { m: num(c.temp?.m, "temp.m"), b: num(c.temp?.b, "temp.b") },
    co2_legacy: { a: num(c.co2?.a, "co2.a"), b: num(c.co2?.b, "co2.b"), enabled: Boolean(c.co2?.enabled) },
    co2_1: co2(c.co2_1 ?? {}, "co2_1"),
    co2_2: co2(c.co2_2 ?? {}, "co2_2"),
  },
  node_b: {
    o2_gas_1: { gain: num(c.o2_gas_1?.gain, "o2_gas_1.gain"), offset: num(c.o2_gas_1?.offset, "o2_gas_1.offset"), reference: 0, command_utc: 0, command_count: 0 },
    o2_gas_2: { gain: num(c.o2_gas_2?.gain, "o2_gas_2.gain"), offset: num(c.o2_gas_2?.offset, "o2_gas_2.offset"), reference: 0, command_utc: 0, command_count: 0 },
  },
};
process.stdout.write(JSON.stringify(out, null, 2) + "\n");
