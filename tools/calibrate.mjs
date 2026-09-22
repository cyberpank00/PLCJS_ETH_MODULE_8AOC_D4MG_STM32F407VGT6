#!/usr/bin/env node
/**
 * calibrate.mjs — dialog-driven calibration / configuration tool for the
 * PLCJS 8AOC analog current-output module (8× 0–20 mA, DAC80508 + XTR111)
 * over Modbus TCP.
 *
 * Node.js 18+ only, no external npm packages (uses node:net / node:readline).
 *
 * The module commands  I = 10 · V_DAC / R_SET  (V_DAC = code/65535 · 2.5 V,
 * R_SET ≈ 1.1 kΩ). R_SET tolerance and DAC/reference error are removed per
 * channel with a linear pre-distortion applied to every setpoint:
 *
 *      I_cmd = gain * I_set + offset            (mA)
 *
 * Calibration: with neutral coefficients the tool commands >= 2 setpoints,
 * you read the true loop current on a reference ammeter, the tool fits
 * I_meas = a·I_cmd + b and stores gain = 1/a, offset = −b/a.
 *
 * Coefficients written to 540+ are a LIVE PREVIEW. Persisting them is a
 * write-once COMMIT (HR131 = 0xCA00 | ch) that locks the channel slot forever;
 * the tool asks for explicit confirmation before committing.
 *
 * Usage:
 *   node calibrate.mjs status                 [--ip A.B.C.D] [--port 502]
 *   node calibrate.mjs calibrate --ch N | --all
 *   node calibrate.mjs set --ch N [--ua 12000] [--enable 0|1] [--lo uA] [--hi uA]
 *                                 [--loss hold|safe|off] [--safe uA]
 *   node calibrate.mjs cycle                  (power-cycle the analog rail)
 */

import net from 'node:net';
import readline from 'node:readline';

/* ----------------------------- register map ----------------------------- */
const MB = {
  // input registers, grouped by quantity, 8 channels each
  IR_CURRENT: 300, IR_FLAGS: 316, IR_CODE: 324, IR_RAIL: 332, IR_LOSS: 333, IR_END: 334,
  IR_MODULE_ID: 125, IR_CAL_LOCK: 127,
  // compact holding block: group*8 + ch
  HR_SETPOINT: 0, HR_SCALE_LO: 8, HR_SCALE_HI: 16, HR_ENABLED: 24, HR_LOSS_MODE: 32, HR_SAFE_UA: 40, HR_SETPOINT_UA: 48,
  // calibration coefficients: 540 + ch*4 -> gain(2), offset(2)
  HR_CAL_BASE: 540, HR_CAL_STRIDE: 4,
  HR_LOSS_TIMEOUT: 100, HR_TRIG_SAVE: 117, TRIG_SAVE: 0xA5A5, HR_TRIG_REBOOT: 118, TRIG_ANALOG_CYCLE: 0xA0FF,
  HR_CAL_COMMIT: 131, CAL_COMMIT_BASE: 0xCA00,
};

const CHANNELS = 8;
const LOSS_NAME = ['hold', 'safe', 'off'];
const FAULT_NAME = { 1: 'EF (open loop/compliance/temp)', 2: 'MCP dead', 3: 'DAC dead', 4: 'rail off' };

/* --------------------------- Modbus TCP client -------------------------- */
class ModbusTCP {
  constructor(ip, port, unit = 1) { this.ip = ip; this.port = port; this.unit = unit; this.txid = 0; }

  connect() {
    return new Promise((resolve, reject) => {
      this.sock = net.connect({ host: this.ip, port: this.port }, () => resolve());
      this.sock.on('error', reject);
      this.sock.setNoDelay(true);
    });
  }
  close() { if (this.sock) this.sock.end(); }

  _txn(pdu) {
    return new Promise((resolve, reject) => {
      this.txid = (this.txid + 1) & 0xffff;
      const header = Buffer.alloc(7);
      header.writeUInt16BE(this.txid, 0);
      header.writeUInt16BE(0, 2);
      header.writeUInt16BE(pdu.length + 1, 4);
      header.writeUInt8(this.unit, 6);
      const frame = Buffer.concat([header, pdu]);

      let buf = Buffer.alloc(0);
      const onData = (chunk) => {
        buf = Buffer.concat([buf, chunk]);
        if (buf.length < 7) return;
        const len = buf.readUInt16BE(4);
        if (buf.length < 6 + len) return;
        cleanup();
        const fn = buf.readUInt8(7);
        if (fn & 0x80) { reject(new Error('Modbus exception ' + buf.readUInt8(8))); return; }
        resolve(buf.slice(8));
      };
      const onErr = (e) => { cleanup(); reject(e); };
      const to = setTimeout(() => { cleanup(); reject(new Error('timeout')); }, 3000);
      const cleanup = () => { clearTimeout(to); this.sock.removeListener('data', onData); this.sock.removeListener('error', onErr); };
      this.sock.on('data', onData);
      this.sock.on('error', onErr);
      this.sock.write(frame);
    });
  }

  async readInput(addr, qty) {
    const pdu = Buffer.alloc(5);
    pdu.writeUInt8(0x04, 0); pdu.writeUInt16BE(addr, 1); pdu.writeUInt16BE(qty, 3);
    const data = await this._txn(pdu);
    const n = data.readUInt8(0);
    const regs = [];
    for (let i = 0; i < n / 2; i++) regs.push(data.readUInt16BE(1 + i * 2));
    return regs;
  }
  async readHolding(addr, qty) {
    const pdu = Buffer.alloc(5);
    pdu.writeUInt8(0x03, 0); pdu.writeUInt16BE(addr, 1); pdu.writeUInt16BE(qty, 3);
    const data = await this._txn(pdu);
    const n = data.readUInt8(0);
    const regs = [];
    for (let i = 0; i < n / 2; i++) regs.push(data.readUInt16BE(1 + i * 2));
    return regs;
  }
  async writeMultiple(addr, regs) {
    const pdu = Buffer.alloc(6 + regs.length * 2);
    pdu.writeUInt8(0x10, 0); pdu.writeUInt16BE(addr, 1); pdu.writeUInt16BE(regs.length, 3);
    pdu.writeUInt8(regs.length * 2, 5);
    regs.forEach((r, i) => pdu.writeUInt16BE(r & 0xffff, 6 + i * 2));
    await this._txn(pdu);
  }
  async writeSingle(addr, val) {
    const pdu = Buffer.alloc(5);
    pdu.writeUInt8(0x06, 0); pdu.writeUInt16BE(addr, 1); pdu.writeUInt16BE(val & 0xffff, 3);
    await this._txn(pdu);
  }
}

/* ------------------------------ float codec ----------------------------- */
function regsToFloat(hi, lo) {
  const b = Buffer.alloc(4);
  b.writeUInt16BE(hi, 0); b.writeUInt16BE(lo, 2);
  return b.readFloatBE(0);
}
function floatToRegs(f) {
  const b = Buffer.alloc(4);
  b.writeFloatBE(f, 0);
  return [b.readUInt16BE(0), b.readUInt16BE(2)];
}

/* ----------------------------- helpers ---------------------------------- */
const rl = readline.createInterface({ input: process.stdin, output: process.stdout });
const ask = (q) => new Promise((res) => rl.question(q, res));
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function parseArgs(argv) {
  const a = { _: [] };
  for (let i = 0; i < argv.length; i++) {
    if (argv[i].startsWith('--')) { const k = argv[i].slice(2); const v = (argv[i + 1] && !argv[i + 1].startsWith('--')) ? argv[++i] : true; a[k] = v; }
    else a._.push(argv[i]);
  }
  return a;
}
async function readAll(mb) {
  const r = await mb.readInput(MB.IR_CURRENT, MB.IR_END - MB.IR_CURRENT);   // 300..333
  const out = [];
  for (let ch = 0; ch < CHANNELS; ch++) {
    out.push({
      cur: regsToFloat(r[ch * 2], r[ch * 2 + 1]),
      flags: r[MB.IR_FLAGS - 300 + ch],
      code: r[MB.IR_CODE - 300 + ch],
    });
  }
  return { ch: out, rail: r[MB.IR_RAIL - 300], loss: r[MB.IR_LOSS - 300] };
}

function linfit(points) {
  const n = points.length;
  let sx = 0, sy = 0, sxx = 0, sxy = 0;
  for (const [x, y] of points) { sx += x; sy += y; sxx += x * x; sxy += x * y; }
  const denom = n * sxx - sx * sx;
  const a = (n * sxy - sx * sy) / denom;
  const b = (sy - a * sx) / n;
  let maxErr = 0;
  for (const [x, y] of points) maxErr = Math.max(maxErr, Math.abs(a * x + b - y));
  return { a, b, maxErr };
}

const i16 = (v) => (v > 0x7fff ? v - 0x10000 : v);

/* ------------------------------ commands -------------------------------- */
async function cmdStatus(mb) {
  const id = (await mb.readInput(MB.IR_MODULE_ID, 1))[0];
  const lock = (await mb.readInput(MB.IR_CAL_LOCK, 1))[0];
  const cfg = await mb.readHolding(0, 56);
  const to = (await mb.readHolding(MB.HR_LOSS_TIMEOUT, 1))[0];
  const all = await readAll(mb);
  console.log(`Module ID: 0x${id.toString(16)}  rail=${all.rail ? 'ON' : 'OFF'}  comms-loss=${all.loss ? 'ACTIVE' : 'no'} (timeout ${to / 10} s)`);
  for (let ch = 0; ch < CHANNELS; ch++) {
    const s = all.ch[ch];
    const f = [];
    if (s.flags & 1) f.push('EN'); if (s.flags & 2) f.push('OUT'); if (s.flags & 4) f.push('FAULT'); if (s.flags & 8) f.push('LOSS');
    const fcode = (s.flags >> 8) & 0xff;
    console.log(`CH${ch}: set=${cfg[MB.HR_SETPOINT_UA + ch]} uA (i16=${i16(cfg[MB.HR_SETPOINT + ch])})  scale ${cfg[MB.HR_SCALE_LO + ch]}..${cfg[MB.HR_SCALE_HI + ch]} uA  ` +
      `cmd=${s.cur.toFixed(4)} mA  code=${s.code}  loss=${LOSS_NAME[cfg[MB.HR_LOSS_MODE + ch]] ?? '?'}/${cfg[MB.HR_SAFE_UA + ch]}uA  [${f.join(',')}]` +
      (fcode ? ` fault=${FAULT_NAME[fcode] ?? fcode}` : '') + ((lock >> ch) & 1 ? '  LOCKED' : ''));
  }
}

async function calibrateChannel(mb, ch) {
  console.log(`\n=== Calibrating CH${ch} ===`);
  const calBase = MB.HR_CAL_BASE + ch * MB.HR_CAL_STRIDE;
  const lock = (await mb.readInput(MB.IR_CAL_LOCK, 1))[0];
  if ((lock >> ch) & 1) { console.log('Slot is LOCKED (already committed) — skipping.'); return; }

  // Neutral coefficients so the commanded current is the raw hardware response.
  await mb.writeMultiple(calBase, [...floatToRegs(1), ...floatToRegs(0)]);
  await mb.writeSingle(MB.HR_ENABLED + ch, 1);
  const saved = (await mb.readHolding(MB.HR_SETPOINT_UA + ch, 1))[0];

  const points = [];
  console.log(`Connect the reference ammeter in the CH${ch} loop (AO${ch}+ → load → GND_ISO).`);
  for (;;) {
    const ans = await ask(`Setpoint to command in mA (e.g. 4, 12, 20) or "done" (need >= 2 points): `);
    if (ans.trim().toLowerCase() === 'done') {
      if (points.length >= 2) break;
      console.log('Need at least 2 points.'); continue;
    }
    const iCmd = parseFloat(ans.replace(',', '.'));
    if (!Number.isFinite(iCmd) || iCmd < 0 || iCmd > 22) { console.log('Invalid (0..22 mA).'); continue; }
    await mb.writeSingle(MB.HR_SETPOINT_UA + ch, Math.round(iCmd * 1000));
    await sleep(500);
    const m = await ask(`  Commanded ${iCmd} mA — enter the measured loop current in mA: `);
    const iMeas = parseFloat(m.replace(',', '.'));
    if (!Number.isFinite(iMeas)) { console.log('Invalid number; point skipped.'); continue; }
    points.push([iCmd, iMeas]);
    console.log(`  Recorded point ${points.length}: cmd=${iCmd} -> meas=${iMeas}`);
  }

  const { a, b, maxErr } = linfit(points);           // I_meas = a*I_cmd + b
  const gain = 1 / a, offset = -b / a;               // I_cmd = gain*I_set + offset
  console.log(`\nHardware: I_meas = ${a.toFixed(6)}·I_cmd + ${b.toFixed(5)}  (max residual ${maxErr.toFixed(5)} mA)`);
  console.log(`Correction: gain=${gain.toFixed(7)}  offset=${offset.toFixed(5)} mA`);

  await mb.writeMultiple(calBase, [...floatToRegs(gain), ...floatToRegs(offset)]);
  await mb.writeSingle(MB.HR_SETPOINT_UA + ch, saved);
  console.log(`Wrote coefficients (live preview) to holding regs ${calBase}..${calBase + 3}; setpoint restored.`);

  const ans = await ask(`\nCOMMIT CH${ch} to write-once Flash? This is IRREVERSIBLE. Type "COMMIT" to proceed: `);
  if (ans.trim() === 'COMMIT') {
    await mb.writeSingle(MB.HR_CAL_COMMIT, MB.CAL_COMMIT_BASE | ch);
    console.log('Committed and locked.');
  } else {
    console.log('Not committed (preview stays active until reboot).');
  }
}

async function cmdCalibrate(mb, args) {
  const channels = args.all ? [...Array(CHANNELS).keys()] : [parseInt(args.ch, 10)];
  if (channels.some((c) => !(c >= 0 && c < CHANNELS))) throw new Error('Specify --ch 0..7 or --all');
  for (const ch of channels) await calibrateChannel(mb, ch);
}

async function cmdSet(mb, args) {
  const ch = parseInt(args.ch, 10);
  if (!(ch >= 0 && ch < CHANNELS)) throw new Error('Specify --ch 0..7');
  if (args.lo !== undefined || args.hi !== undefined) {
    const curLo = (await mb.readHolding(MB.HR_SCALE_LO + ch, 1))[0];
    const curHi = (await mb.readHolding(MB.HR_SCALE_HI + ch, 1))[0];
    const lo = args.lo !== undefined ? parseInt(args.lo, 10) : curLo;
    const hi = args.hi !== undefined ? parseInt(args.hi, 10) : curHi;
    if (!(hi > lo)) throw new Error('--hi must be greater than --lo');
    const order = hi > curHi ? [[MB.HR_SCALE_HI, hi], [MB.HR_SCALE_LO, lo]] : [[MB.HR_SCALE_LO, lo], [MB.HR_SCALE_HI, hi]];
    for (const [reg, val] of order) await mb.writeSingle(reg + ch, val);
    console.log(`CH${ch} scale = ${lo}..${hi} uA`);
  }
  if (args.loss !== undefined) {
    const idx = LOSS_NAME.indexOf(String(args.loss).toLowerCase());
    if (idx < 0) throw new Error('--loss must be hold|safe|off');
    await mb.writeSingle(MB.HR_LOSS_MODE + ch, idx);
  }
  if (args.safe !== undefined) await mb.writeSingle(MB.HR_SAFE_UA + ch, parseInt(args.safe, 10));
  if (args.enable !== undefined) await mb.writeSingle(MB.HR_ENABLED + ch, parseInt(args.enable, 10) ? 1 : 0);
  if (args.ua !== undefined) await mb.writeSingle(MB.HR_SETPOINT_UA + ch, parseInt(args.ua, 10));
  await mb.writeSingle(MB.HR_TRIG_SAVE, MB.TRIG_SAVE);
  console.log('Configuration (and current setpoints as power-on values) saved.');
}

async function cmdCycle(mb) {
  await mb.writeSingle(MB.HR_TRIG_REBOOT, MB.TRIG_ANALOG_CYCLE);
  console.log('Analog rail power-cycle requested (outputs drop to 0 mA for ~0.7 s).');
}

/* ------------------------------- main ----------------------------------- */
async function main() {
  const args = parseArgs(process.argv.slice(2));
  const cmd = args._[0] || 'status';
  const ip = args.ip || '192.168.1.14';
  const port = parseInt(args.port || '502', 10);
  const unit = parseInt(args.unit || '1', 10);

  const mb = new ModbusTCP(ip, port, unit);
  console.log(`Connecting to ${ip}:${port} (unit ${unit}) ...`);
  await mb.connect();

  try {
    if (cmd === 'status') await cmdStatus(mb);
    else if (cmd === 'calibrate') await cmdCalibrate(mb, args);
    else if (cmd === 'set') await cmdSet(mb, args);
    else if (cmd === 'cycle') await cmdCycle(mb);
    else console.log('Unknown command. Use: status | calibrate | set | cycle');
  } finally {
    mb.close();
    rl.close();
  }
}

main().catch((e) => { console.error('Error:', e.message); process.exit(1); });
