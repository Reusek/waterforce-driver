#!/usr/bin/env python3
"""
Probe whether the AORUS WATERFORCE X (1044:7a4d) accepts a fixed fan/pump
speed. Run with sudo. Takes about 4 minutes.

The fixed-RPM command (0xE6) was ignored when sent alone. Two untested ideas:
  1. it needs the 0xB6 "apply" command after it, like the profiles do
  2. the cooler must first be in a "custom" mode. The known fan profile codes
     are 00 02 04 05 06 07, so 01 and 03 are the likely custom-mode codes.

Each test asks for a speed well BELOW the quietest profile (fan 1000 RPM,
pump 2000 RPM), so a working command shows up as a clear drop. Everything is
set back to the balance profiles at the end, and also if the script is
interrupted.
"""
import glob
import os
import select
import subprocess
import sys
import time

REPORT_LEN = 6144
FAN, PUMP = 0, 1
E6_CHANNEL = {FAN: (0x01, 0x01), PUMP: (0x04, 0x02)}
E5_SELECTOR = {FAN: 0x01, PUMP: 0x02}
TARGET = {FAN: 1000, PUMP: 2000}
NAME = {FAN: "fan", PUMP: "pump"}


def find_hidraw():
    for dev in glob.glob("/sys/class/hidraw/hidraw*"):
        try:
            uevent = open(os.path.join(dev, "device/uevent")).read().upper()
        except OSError:
            continue
        if "00001044:00007A4D" in uevent:
            return "/dev/" + os.path.basename(dev)
    sys.exit("cooler hidraw device not found")


def send(fd, *head):
    buf = bytearray(REPORT_LEN)
    buf[0] = 0x99
    buf[1:1 + len(head)] = bytes(head)
    os.write(fd, bytes(buf))
    time.sleep(0.03)


def status(fd):
    # drop stale reports, then ask for a fresh one
    while select.select([fd], [], [], 0)[0]:
        os.read(fd, REPORT_LEN)
    send(fd, 0xDA)
    deadline = time.monotonic() + 2
    while time.monotonic() < deadline:
        if select.select([fd], [], [], 0.2)[0]:
            r = os.read(fd, REPORT_LEN)
            if len(r) > 14 and r[0] == 0x99 and r[1] == 0xDA:
                return {"fan": r[2] | r[3] << 8, "pump": r[5] | r[6] << 8,
                        "fan%": r[8], "pump%": r[9], "temp": r[13]}
    return None


def e6(fd, ch, rpm):
    pkt = [0xE6, *E6_CHANNEL[ch], 0x00]
    for marker in (0x1E, 0x32, 0x41, None):
        pkt += [rpm >> 8, rpm & 0xFF]
        if marker is not None:
            pkt.append(marker)
    send(fd, *pkt)


def profile(fd, ch, code):
    send(fd, 0xE5, E5_SELECTOR[ch], code)
    send(fd, 0xB6)


def commit(fd):
    send(fd, 0xB6)


def reset(fd):
    profile(fd, FAN, 0x00)
    profile(fd, PUMP, 0x00)


def show(fd, label, wait=8):
    time.sleep(wait)
    s = status(fd)
    if s is None:
        print(f"{label:44s} no status reply")
    else:
        print(f"{label:44s} fan={s['fan']:5d} rpm {s['fan%']:3d}%   "
              f"pump={s['pump']:5d} rpm {s['pump%']:3d}%   coolant={s['temp']}C")


def tests(ch):
    t = TARGET[ch]
    sel = lambda c: (lambda fd: profile(fd, ch, c))
    send_e6 = lambda fd: e6(fd, ch, t)
    return [
        (f"E6 {t}, then B6", [send_e6, commit]),
        (f"mode 01, then E6 {t} + B6", [sel(0x01), send_e6, commit]),
        (f"mode 03, then E6 {t} + B6", [sel(0x03), send_e6, commit]),
        (f"E6 {t}, then mode 01", [send_e6, sel(0x01)]),
        (f"E6 {t}, then mode 03", [send_e6, sel(0x03)]),
    ]


def main():
    if os.geteuid() != 0:
        sys.exit("run with sudo")

    cc_active = subprocess.run(["systemctl", "is-active", "-q", "coolercontrold"]).returncode == 0
    if cc_active:
        subprocess.run(["systemctl", "stop", "coolercontrold"])
        print("== coolercontrold stopped for the test")

    fd = os.open(find_hidraw(), os.O_RDWR)
    try:
        reset(fd)
        show(fd, "baseline (both balance)")
        print("-- pump: unused profile codes (only 00 balance and 04 max are known)")
        for code in (0x01, 0x02, 0x03, 0x05, 0x06, 0x07):
            profile(fd, PUMP, code)
            show(fd, f"pump profile code {code:02X}")
        reset(fd)
        for ch in (FAN, PUMP):
            print(f"-- {NAME[ch]}: asking for {TARGET[ch]} RPM")
            for label, steps in tests(ch):
                for step in steps:
                    step(fd)
                show(fd, label)
                reset(fd)
                show(fd, "   back to balance", wait=6)
    finally:
        reset(fd)
        os.close(fd)
        print("== restored balance profiles")
        if cc_active:
            subprocess.run(["systemctl", "start", "coolercontrold"])
            print("== coolercontrold started again")


if __name__ == "__main__":
    main()
