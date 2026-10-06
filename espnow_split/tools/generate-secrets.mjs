#!/usr/bin/env node
// Genera los archivos de secretos ESP-NOW (excluidos de Git) con claves aleatorias:
//   node tools/generate-secrets.mjs --gateway-mac 24:6F:28:AA:BB:CC \
//        --node-a-mac 24:6F:28:11:22:33 --node-b-mac 5C:CF:7F:44:55:66 [--channel 1]
//        [--ssid "MiRed"] [--password "clave"] [--force]
// Escribe BioIoT_NodeA_Sensors/node_secrets.h, BioIoT_NodeB_I2C/node_secrets.h,
// BioIoT_NodeB_ESP32/node_secrets.h (mismo contenido) y BioIoT_NodeC_Gateway/gateway_secrets.h.
// Cambiar solo la placa B no exige regenerar: basta con actualizar BIOIOT_NODE_B_MAC en C. Nunca imprime las claves. Cada enlace (A<->C,
// B<->C) tiene LMK y clave HMAC propias; la PMK y el SYSTEM_ID son comunes.
import { randomBytes } from "node:crypto";
import { existsSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const args = process.argv.slice(2);
const opt = (name, def = undefined) => {
  const i = args.indexOf(`--${name}`);
  return i >= 0 ? args[i + 1] : def;
};
const flag = (name) => args.includes(`--${name}`);

function parseMac(text, label) {
  const parts = String(text || "").split(/[:-]/);
  if (parts.length !== 6 || parts.some((p) => !/^[0-9a-fA-F]{2}$/.test(p))) {
    throw new Error(`MAC invalida para ${label}: use AA:BB:CC:DD:EE:FF (la imprime {"action":"pairing_info"})`);
  }
  const bytes = parts.map((p) => parseInt(p, 16));
  if (bytes.every((b) => b === 0)) throw new Error(`MAC nula para ${label}`);
  return bytes;
}
const hexList = (bytes) => bytes.map((b) => `0x${b.toString(16).padStart(2, "0")}`).join(", ");
const cString = (s) => JSON.stringify(String(s));
const key = (n) => [...randomBytes(n)];

process.on("uncaughtException", (e) => {
  console.error(`ERROR: ${e.message}`);
  process.exit(1);
});

const gw = parseMac(opt("gateway-mac"), "gateway");
const a = parseMac(opt("node-a-mac"), "nodo A");
const b = parseMac(opt("node-b-mac"), "nodo B");
const channel = Number(opt("channel", "1"));
if (!Number.isInteger(channel) || channel < 1 || channel > 13) throw new Error("--channel debe ser 1..13");
let systemId = 0;
while (systemId === 0) systemId = randomBytes(4).readUInt32LE(0);
const pmk = key(16);
const link = { a: { lmk: key(16), app: key(32) }, b: { lmk: key(16), app: key(32) } };

const header = `#pragma once
// GENERADO por tools/generate-secrets.mjs. SECRETO: no compartir ni subir a Git.
#define BIOIOT_SYSTEM_ID 0x${systemId.toString(16).padStart(8, "0")}UL
#define BIOIOT_INITIAL_CHANNEL ${channel}
#define BIOIOT_ESPNOW_PMK {${hexList(pmk)}}
`;
const nodeFile = (l) => `${header}#define BIOIOT_GATEWAY_MAC {${hexList(gw)}}
#define BIOIOT_LINK_LMK {${hexList(l.lmk)}}
#define BIOIOT_LINK_APP_KEY {${hexList(l.app)}}
`;
const gatewayFile = `${header}#define BIOIOT_NODE_A_MAC {${hexList(a)}}
#define BIOIOT_NODE_A_LMK {${hexList(link.a.lmk)}}
#define BIOIOT_NODE_A_APP_KEY {${hexList(link.a.app)}}
#define BIOIOT_NODE_B_MAC {${hexList(b)}}
#define BIOIOT_NODE_B_LMK {${hexList(link.b.lmk)}}
#define BIOIOT_NODE_B_APP_KEY {${hexList(link.b.app)}}
#define WIFI_SSID ${cString(opt("ssid", ""))}
#define WIFI_PASSWORD ${cString(opt("password", ""))}
`;

const targets = [
  [join(root, "BioIoT_NodeA_Sensors", "node_secrets.h"), nodeFile(link.a)],
  // El nodo B existe en ESP8266 y en ESP32: el mismo archivo en ambas carpetas (solo
  // se carga una de las dos placas; --node-b-mac es la MAC de la placa B en uso).
  [join(root, "BioIoT_NodeB_I2C", "node_secrets.h"), nodeFile(link.b)],
  [join(root, "BioIoT_NodeB_ESP32", "node_secrets.h"), nodeFile(link.b)],
  [join(root, "BioIoT_NodeC_Gateway", "gateway_secrets.h"), gatewayFile],
];
for (const [path] of targets) {
  if (existsSync(path) && !flag("force")) {
    throw new Error(`${path} ya existe; use --force para regenerar TODOS los enlaces juntos.`);
  }
}
for (const [path, text] of targets) writeFileSync(path, text, { mode: 0o600 });
console.log("Secretos generados (claves no mostradas):");
for (const [path] of targets) console.log(`  ${path}`);
console.log(`SYSTEM_ID y canal inicial ${channel}. Compilar y cargar los TRES firmwares con estos archivos.`);
