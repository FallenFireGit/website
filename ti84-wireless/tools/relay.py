"""Bench relay: TI-84 (USB device, via the laptop) <-> ESP32-C3 (bench build).

Lets the WIFI program be tested with both the calculator and the C3 plugged
into a PC, before the C3 is soldered in. The calculator enumerates as a CDC
serial port only while WIFI is running, so this waits for it and reconnects.

    python relay.py [C3_PORT]        # default: auto-detect Espressif 303A:1001
"""
import sys
import time

import serial
from serial.tools import list_ports

ESPRESSIF_VID = 0x303A


def find_c3():
    for p in list_ports.comports():
        if p.vid == ESPRESSIF_VID:
            return p.device
    return None


def find_calc(exclude):
    # Any USB serial port that isn't the C3 or a Bluetooth link.
    for p in list_ports.comports():
        if p.device in exclude or p.vid is None or p.vid == ESPRESSIF_VID:
            continue
        return p.device
    return None


def open_port(dev):
    s = serial.Serial()
    s.port = dev
    s.baudrate = 115200
    s.timeout = 0
    s.dtr = False  # DTR/RTS reset the C3 on USB-Serial-JTAG
    s.rts = False
    s.open()
    return s


def show(direction, data):
    for line in data.decode("ascii", errors="replace").splitlines():
        if line.upper().startswith("KEY "):
            line = "KEY ****"
        print(f"{direction} {line}")


def main():
    c3_dev = sys.argv[1] if len(sys.argv) > 1 else find_c3()
    if not c3_dev:
        sys.exit("C3 not found. Is the bench build flashed and the C3 plugged in?")
    c3 = open_port(c3_dev)
    print(f"C3 on {c3_dev}. Run WIFI on the calculator (Ctrl+C to quit).")

    calc = None
    while True:
        try:
            if calc is None:
                dev = find_calc({c3_dev})
                if not dev:
                    time.sleep(0.1)
                    continue
                try:
                    calc = open_port(dev)
                except serial.SerialException:
                    time.sleep(0.1)  # listed before Windows finished setting it up
                    continue
                calc.dtr = True  # some CDC stacks only send once the host asserts DTR
                print(f"Calculator on {dev}")

            moved = False
            data = calc.read(4096)
            if data:
                c3.write(data)
                show("calc>", data)
                moved = True
            data = c3.read(4096)
            if data:
                calc.write(data)
                show("  <c3", data)
                moved = True
            if not moved:
                time.sleep(0.005)
        except serial.SerialException:
            if calc is not None:
                print("Calculator disconnected; waiting for WIFI to start again.")
                try:
                    calc.close()
                except serial.SerialException:
                    pass
                calc = None
            else:
                raise
        except KeyboardInterrupt:
            break


if __name__ == "__main__":
    main()
