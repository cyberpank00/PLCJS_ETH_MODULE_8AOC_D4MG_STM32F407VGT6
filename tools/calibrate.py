#!/usr/bin/env python3
"""
calibrate.py - dialog-driven calibration / configuration tool for the
PLCJS 8AOC analog current-output module (8x 0-20 mA, DAC80508 + XTR111)
over Modbus TCP.

Pure standard-library Python 3.8+ (socket, struct, argparse) - no pymodbus
required. Functionally identical to tools/calibrate.mjs.

The module commands  I = 10 * V_DAC / R_SET  (V_DAC = code/65535 * 2.5 V,
R_SET ~ 1.1 kOhm). Per-channel linear pre-distortion applied to setpoints:

    I_cmd = gain * I_set + offset        (mA)

Calibration: with neutral coefficients the tool commands >= 2 setpoints, you
read the true loop current on a reference ammeter, the tool fits
I_meas = a*I_cmd + b and stores gain = 1/a, offset = -b/a.

Coefficients written to 540+ are a LIVE PREVIEW. Persisting them is a
write-once COMMIT (HR131 = 0xCA00 | ch) that locks the channel slot forever;
the tool asks for explicit confirmation before committing.

Examples:
    python calibrate.py status --ip 192.168.1.14
    python calibrate.py calibrate --ch 0
    python calibrate.py set --ch 0 --ua 12000 --enable 1 --lo 4000 --hi 20000
    python calibrate.py set --ch 1 --loss safe --safe 3600
    python calibrate.py cycle
"""

import argparse
import socket
import struct
import sys
import time

# ------------------------------ register map ------------------------------
IR_CURRENT, IR_FLAGS, IR_CODE, IR_RAIL, IR_LOSS, IR_END = 300, 316, 324, 332, 333, 334
IR_MODULE_ID, IR_CAL_LOCK = 125, 127

HR_SETPOINT, HR_SCALE_LO, HR_SCALE_HI, HR_ENABLED, HR_LOSS_MODE, HR_SAFE_UA, HR_SETPOINT_UA = 0, 8, 16, 24, 32, 40, 48
HR_CAL_BASE, HR_CAL_STRIDE = 540, 4
HR_LOSS_TIMEOUT = 100
HR_TRIG_SAVE, TRIG_SAVE = 117, 0xA5A5
HR_TRIG_REBOOT, TRIG_ANALOG_CYCLE = 118, 0xA0FF
HR_CAL_COMMIT, CAL_COMMIT_BASE = 131, 0xCA00

CHANNELS = 8
LOSS_NAME = ["hold", "safe", "off"]
FAULT_NAME = {1: "EF (open loop/compliance/temp)", 2: "MCP dead", 3: "DAC dead", 4: "rail off"}


# --------------------------- Modbus TCP client ----------------------------
class ModbusTCP:
    def __init__(self, ip, port=502, unit=1, timeout=3.0):
        self.ip, self.port, self.unit, self.timeout = ip, port, unit, timeout
        self.txid = 0
        self.sock = None

    def connect(self):
        self.sock = socket.create_connection((self.ip, self.port), self.timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    def close(self):
        if self.sock:
            self.sock.close()

    def _txn(self, pdu):
        self.txid = (self.txid + 1) & 0xFFFF
        header = struct.pack(">HHHB", self.txid, 0, len(pdu) + 1, self.unit)
        self.sock.sendall(header + pdu)
        # read MBAP header
        buf = b""
        while len(buf) < 7:
            chunk = self.sock.recv(260)
            if not chunk:
                raise IOError("connection closed")
            buf += chunk
        length = struct.unpack(">H", buf[4:6])[0]
        while len(buf) < 6 + length:
            chunk = self.sock.recv(260)
            if not chunk:
                raise IOError("connection closed")
            buf += chunk
        fn = buf[7]
        if fn & 0x80:
            raise IOError("Modbus exception %d" % buf[8])
        return buf[8:6 + length]

    def read_input(self, addr, qty):
        data = self._txn(struct.pack(">BHH", 0x04, addr, qty))
        n = data[0]
        return list(struct.unpack(">%dH" % (n // 2), data[1:1 + n]))

    def read_holding(self, addr, qty):
        data = self._txn(struct.pack(">BHH", 0x03, addr, qty))
        n = data[0]
        return list(struct.unpack(">%dH" % (n // 2), data[1:1 + n]))

    def write_multiple(self, addr, regs):
        pdu = struct.pack(">BHHB", 0x10, addr, len(regs), len(regs) * 2)
        pdu += b"".join(struct.pack(">H", r & 0xFFFF) for r in regs)
        self._txn(pdu)

    def write_single(self, addr, val):
        self._txn(struct.pack(">BHH", 0x06, addr, val & 0xFFFF))


# ------------------------------ float codec -------------------------------
def regs_to_float(hi, lo):
    return struct.unpack(">f", struct.pack(">HH", hi, lo))[0]


def float_to_regs(f):
    return list(struct.unpack(">HH", struct.pack(">f", f)))



# ------------------------------- helpers ----------------------------------
def i16(v):
    return v - 0x10000 if v > 0x7FFF else v


def read_all(mb):
    r = mb.read_input(IR_CURRENT, IR_END - IR_CURRENT)  # 300..333
    ch = [{"cur": regs_to_float(r[c * 2], r[c * 2 + 1]),
           "flags": r[IR_FLAGS - 300 + c], "code": r[IR_CODE - 300 + c]} for c in range(CHANNELS)]
    return {"ch": ch, "rail": r[IR_RAIL - 300], "loss": r[IR_LOSS - 300]}


def linfit(points):
    n = len(points)
    sx = sum(x for x, _ in points)
    sy = sum(y for _, y in points)
    sxx = sum(x * x for x, _ in points)
    sxy = sum(x * y for x, y in points)
    denom = n * sxx - sx * sx
    a = (n * sxy - sx * sy) / denom
    b = (sy - a * sx) / n
    max_err = max(abs(a * x + b - y) for x, y in points)
    return a, b, max_err


# ------------------------------- commands ---------------------------------
def cmd_status(mb, _args):
    mid = mb.read_input(IR_MODULE_ID, 1)[0]
    lock = mb.read_input(IR_CAL_LOCK, 1)[0]
    cfg = mb.read_holding(0, 56)
    to = mb.read_holding(HR_LOSS_TIMEOUT, 1)[0]
    allv = read_all(mb)
    print("Module ID: 0x%04X  rail=%s  comms-loss=%s (timeout %.1f s)" % (
        mid, "ON" if allv["rail"] else "OFF", "ACTIVE" if allv["loss"] else "no", to / 10.0))
    for c, s in enumerate(allv["ch"]):
        flags = [n for b, n in ((1, "EN"), (2, "OUT"), (4, "FAULT"), (8, "LOSS")) if s["flags"] & b]
        fcode = (s["flags"] >> 8) & 0xFF
        lm = cfg[HR_LOSS_MODE + c]
        print("CH%d: set=%d uA (i16=%d)  scale %d..%d uA  cmd=%.4f mA  code=%d  loss=%s/%duA  [%s]%s%s" % (
            c, cfg[HR_SETPOINT_UA + c], i16(cfg[HR_SETPOINT + c]), cfg[HR_SCALE_LO + c], cfg[HR_SCALE_HI + c],
            s["cur"], s["code"], LOSS_NAME[lm] if lm < 3 else "?", cfg[HR_SAFE_UA + c], ",".join(flags),
            (" fault=%s" % FAULT_NAME.get(fcode, fcode)) if fcode else "",
            "  LOCKED" if (lock >> c) & 1 else ""))


def calibrate_channel(mb, ch):
    print("\n=== Calibrating CH%d ===" % ch)
    cal_base = HR_CAL_BASE + ch * HR_CAL_STRIDE
    if (mb.read_input(IR_CAL_LOCK, 1)[0] >> ch) & 1:
        print("Slot is LOCKED (already committed) - skipping.")
        return

    mb.write_multiple(cal_base, float_to_regs(1.0) + float_to_regs(0.0))  # neutral
    mb.write_single(HR_ENABLED + ch, 1)
    saved = mb.read_holding(HR_SETPOINT_UA + ch, 1)[0]

    points = []
    print("Connect the reference ammeter in the CH%d loop (AO%d+ -> load -> GND_ISO)." % (ch, ch))
    while True:
        ans = input('Setpoint to command in mA (e.g. 4, 12, 20) or "done" (need >= 2 points): ').strip()
        if ans.lower() == "done":
            if len(points) >= 2:
                break
            print("Need at least 2 points.")
            continue
        try:
            i_cmd = float(ans.replace(",", "."))
        except ValueError:
            print("Invalid number.")
            continue
        if not 0 <= i_cmd <= 22:
            print("Invalid (0..22 mA).")
            continue
        mb.write_single(HR_SETPOINT_UA + ch, round(i_cmd * 1000))
        time.sleep(0.5)
        try:
            i_meas = float(input("  Commanded %g mA - enter the measured loop current in mA: " % i_cmd).replace(",", "."))
        except ValueError:
            print("Invalid number; point skipped.")
            continue
        points.append((i_cmd, i_meas))
        print("  Recorded point %d: cmd=%g -> meas=%g" % (len(points), i_cmd, i_meas))

    a, b, max_err = linfit(points)            # I_meas = a*I_cmd + b
    gain, offset = 1.0 / a, -b / a            # I_cmd = gain*I_set + offset
    print("\nHardware: I_meas = %.6f*I_cmd + %.5f  (max residual %.5f mA)" % (a, b, max_err))
    print("Correction: gain=%.7f  offset=%.5f mA" % (gain, offset))

    mb.write_multiple(cal_base, float_to_regs(gain) + float_to_regs(offset))
    mb.write_single(HR_SETPOINT_UA + ch, saved)
    print("Wrote coefficients (live preview) to holding regs %d..%d; setpoint restored." % (cal_base, cal_base + 3))

    ans = input("\nCOMMIT CH%d to write-once Flash? This is IRREVERSIBLE. "
                'Type "COMMIT" to proceed: ' % ch).strip()
    if ans == "COMMIT":
        mb.write_single(HR_CAL_COMMIT, CAL_COMMIT_BASE | ch)
        print("Committed and locked.")
    else:
        print("Not committed (preview stays active until reboot).")


def cmd_calibrate(mb, args):
    channels = list(range(CHANNELS)) if args.all else [args.ch]
    if any(c is None or not (0 <= c < CHANNELS) for c in channels):
        raise SystemExit("Specify --ch 0..7 or --all")
    for ch in channels:
        calibrate_channel(mb, ch)


def cmd_set(mb, args):
    ch = args.ch
    if ch is None or not (0 <= ch < CHANNELS):
        raise SystemExit("Specify --ch 0..7")
    if args.lo is not None or args.hi is not None:
        cur_lo = mb.read_holding(HR_SCALE_LO + ch, 1)[0]
        cur_hi = mb.read_holding(HR_SCALE_HI + ch, 1)[0]
        lo = args.lo if args.lo is not None else cur_lo
        hi = args.hi if args.hi is not None else cur_hi
        if not hi > lo:
            raise SystemExit("--hi must be greater than --lo")
        order = ((HR_SCALE_HI, hi), (HR_SCALE_LO, lo)) if hi > cur_hi else ((HR_SCALE_LO, lo), (HR_SCALE_HI, hi))
        for reg, val in order:
            mb.write_single(reg + ch, val)
        print("CH%d scale = %d..%d uA" % (ch, lo, hi))
    if args.loss:
        mb.write_single(HR_LOSS_MODE + ch, LOSS_NAME.index(args.loss))
    if args.safe is not None:
        mb.write_single(HR_SAFE_UA + ch, args.safe)
    if args.enable is not None:
        mb.write_single(HR_ENABLED + ch, 1 if args.enable else 0)
    if args.ua is not None:
        mb.write_single(HR_SETPOINT_UA + ch, args.ua)
    mb.write_single(HR_TRIG_SAVE, TRIG_SAVE)
    print("Configuration (and current setpoints as power-on values) saved.")


def cmd_cycle(mb, _args):
    mb.write_single(HR_TRIG_REBOOT, TRIG_ANALOG_CYCLE)
    print("Analog rail power-cycle requested (outputs drop to 0 mA for ~0.7 s).")


def main():
    p = argparse.ArgumentParser(description="PLCJS 8AOC calibration tool (Modbus TCP)")
    p.add_argument("command", choices=["status", "calibrate", "set", "cycle"])
    p.add_argument("--ip", default="192.168.1.14")
    p.add_argument("--port", type=int, default=502)
    p.add_argument("--unit", type=int, default=1)
    p.add_argument("--ch", type=int)
    p.add_argument("--all", action="store_true")
    p.add_argument("--ua", type=int, help="setpoint, uA (0..22000)")
    p.add_argument("--lo", type=int, help="scale low threshold, uA (default 4000)")
    p.add_argument("--hi", type=int, help="scale high threshold, uA (default 20000)")
    p.add_argument("--loss", choices=LOSS_NAME, help="comms-loss mode")
    p.add_argument("--safe", type=int, help="comms-loss safe value, uA")
    p.add_argument("--enable", type=int)
    args = p.parse_args()

    mb = ModbusTCP(args.ip, args.port, args.unit)
    print("Connecting to %s:%d (unit %d) ..." % (args.ip, args.port, args.unit))
    mb.connect()
    try:
        {"status": cmd_status, "calibrate": cmd_calibrate, "set": cmd_set, "cycle": cmd_cycle}[args.command](mb, args)
    finally:
        mb.close()


if __name__ == "__main__":
    main()