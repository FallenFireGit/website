TI-84 Plus CE Internal Wi-Fi (ESP32-C3 Super Mini)
================

This guide puts an ESP32-C3 Super Mini *inside* a TI-84 Plus CE. Nothing sticks out of the case and nothing is plugged into the C3.

The C3 is soldered to the USB data lines behind the calculator's mini-USB port. The calculator talks to it as a USB host using the CE C toolchain's `srldrvce` serial library. The C3 handles Wi-Fi: scanning, joining, fetching web pages and NTP time.

```
 Calculator mini-USB pins              ESP32-C3 Super Mini
 ------------------------              -------------------
 3  D+   ------------------------------ GPIO19 (USB D+)
 2  D-   ------------------------------ GPIO18 (USB D-)
 4  ID   ---- drain [AO3400] gate ----- GPIO3   (+100k gate->GND)
                    source -> GND
 1  VBUS ---- 100k ----+--------------- GPIO4   (ADC)
                       47k -> GND
 5  GND  ------------------------------ GND

 Battery + -- 500mA polyfuse -- switch - 5V     (feeds the 3.3V LDO)
```

Why USB and not UART?
----------------

The non-Python TI-84 Plus CE has no usable UART:
- The 2.5 mm link port was removed on the CE.
- The only internal serial UART on these boards goes to the ARM Python coprocessor, and that only exists on Python Edition boards.

The USB bus is the one fast, well-documented, software-controllable interface. Other internal mods use it too, such as Ce-AI (ESP32-S3) and the Cemetech Pico W module.

The ESP32-C3 has no USB host controller. Its built-in USB-Serial-JTAG port is a USB *device* that shows up as a standard CDC-ACM serial port. So the roles are flipped: **the calculator is the host** and the C3 is the device. You don't need a plug or the C3's USB-C jack. GPIO18/19 *are* the C3's USB pins.

Parts
----------------

| Qty | Part | Notes |
|---|---|---|
| 1 | ESP32-C3 Super Mini | ~22.5 × 18 mm |
| 1 | AO3400 N-MOSFET (SOT-23) | 2N7002 also works; AO3400 switches harder at 3.3 V |
| 1 | 100 kΩ resistor | MOSFET gate pulldown |
| 1 | 100 kΩ + 47 kΩ resistors | VBUS sense divider (5 V → ~1.6 V) |
| 1 | 500 mA PTC polyfuse | in the battery feed |
| 1 | Tiny slide switch (optional) | hard power cut for the C3 |
| — | 30 AWG Kynar / magnet wire, Kapton tape | |

Mini-USB connector pinout
----------------

Looking at the connector on the calculator:

| Pin | Signal | Wire to |
|---|---|---|
| 1 | VBUS | 100k → GPIO4, with 47k from GPIO4 to GND |
| 2 | D- | **GPIO18** |
| 3 | D+ | **GPIO19** |
| 4 | ID | AO3400 drain (source to GND, gate to GPIO3 with 100k to GND) |
| 5 | GND | GND |

> **GPIO18/19 are not on the Super Mini's side pins.** They only go to the USB-C jack. Desolder the jack, then solder D+/D- to its footprint pads. The 4 middle pads alternate D-, D+, D-, D+. Pads 1 and 3 read ~0 Ω to each other, and so do pads 2 and 4. If you swap D+ and D-, nothing is damaged: the calculator just won't detect the C3, so swap the wires.
>
> Never plug the C3's USB-C into a PC while its 5V pin is wired to the battery. On most Super Minis the 5V pin is wired straight to USB VBUS.

> **Check every pad with a multimeter (continuity mode) against the connector pins before soldering.** Pad layout changes between hardware revisions. Solder to the connector's own legs, or to test pads you have *proven* connect to them.

Board notes: SG95N/F/T/NL-12 (2022 date code, revision M+)
----------------

These are the tap points for this board, from multimeter checks on a real unit. The mini-USB connector is J05, on the left edge.

| Signal | Where to solder | Why |
|---|---|---|
| D+ / D- | The pad that reads ~0 Ω to J05 pin 3 / pin 2. That's the ESD diode pad (D13G/D12G) or the connector-side end of B1/B2. | Each data line runs connector → ESD diode / C25G-C26G (to GND) → B1/B2 → R53G/R54G → ASIC. Tap on the **connector side**, never the ASIC side of R53G/R54G. |
| ID | J05 pin 4 leg directly (30 AWG, flux, fine tip) | The trace goes straight into a via, so there's no easier pad. A bridge to pin 5 (GND) is harmless: it acts like an OTG cable until you fix it. |
| VBUS sense | J05 pin 1, or a pad ~0 Ω to it | One nearby part reads ~5 Ω to VBUS. That's fine for a 100k divider. |
| C3 power (5V pin) | **VCBAY+** pad → polyfuse → switch | Large, labelled battery pad |
| C3 GND | **TP-GND1** | Don't use VCBAY- unless it reads ~0 Ω to TP-GND1. A charge or sense part sits on it (it reads ~1 Ω to a 3-pin part). |

To check polarity safely, measure the battery pack's own terminals out of the calculator. Or, with the pack in, measure from the VCBAY+ pad to TP-GND1. Both are large pads far apart. Expect 3.7–4.2 V.

You can't make the calculator act as USB host from software: `usbdrvce` takes its host/device role from the ID pin. That's why the ID wire is needed.

Build order
----------------

### 1. Flash the C3 (before it goes in the calculator)

Using PlatformIO:

```sh
cd firmware
pio run -e c3_bench -t upload     # USB-C plugged into your PC
pio device monitor --echo --rts 0 --dtr 0   # type PING, SCAN, GET http://example.com
```

The normal `c3_supermini` build detaches from USB until it sees the calculator (see *How the USB port is shared*), so a PC can't talk to it. The `c3_bench` build stays attached for testing. **Flash the normal build before installing:**

```sh
pio run -e c3_supermini -t upload
```

If the C3 doesn't show up as a COM port, hold BOOT, tap RST, release BOOT, then upload. Unplug and replug afterwards to start the new firmware.

Using the Arduino IDE instead:
- Install ESP32 core **2.0.17**.
- Board: "ESP32C3 Dev Module", USB CDC On Boot: Enabled, CPU 80 MHz.
- Copy `src/main.cpp` into a sketch and install the `WiFiManager` (tzapu) and `ArduinoJson` (bblanchon, v7) libraries.

On first boot with no saved network, the C3 opens a Wi-Fi access point called **TI84-Setup**. Join it from your phone and pick your network, which saves the credentials.

### 2. Build and send the calculator program

Install the [CE C toolchain](https://github.com/CE-Programming/toolchain/releases), then:

```sh
cd calc
make                     # -> bin/WIFI.8xp
```

Send `WIFI.8xp` to the calculator with TI Connect CE. You'll need an assembly-program launcher on newer OS versions (e.g. arTIfiCE + Cesium).

### 3. Bench test **before** soldering

1. Connect the calculator's mini-USB to the C3's USB-C with an OTG adapter or OTG cable. The OTG cable grounds ID, just like the MOSFET will.
2. Run `WIFI` and check that Status / Scan / Join / Fetch URL / Time all work.

If they do, the USB link and the software are good, and only the wiring is left.

### 4. Install

1. Remove the back screws (Torx T6) and open the case.
2. **Unplug the battery before soldering anything.**
3. Desolder the C3's USB-C jack (hot air or lots of flux). This saves about 3 mm of height and leaves room to fit. From now on, update firmware over Wi-Fi (OTA, see below).
4. Wire it up as shown in the table above:
   - Twist the D+/D- wires together.
   - Keep them under 5 cm.
   - Don't leave stubs.
5. Power comes from the **battery +** terminal, through the polyfuse and the optional switch, into the C3's **5V** pin.
   - That pin feeds the C3's 3.3 V LDO, which works fine down to about 3.4 V on the battery.
   - **Do not** take power from the calculator's 3.3 V rail. Wi-Fi transmit peaks are about 350 mA.
6. Wrap the C3 in Kapton tape. Leave the chip antenna (the end with the ceramic antenna) facing the top edge of the case, away from the battery and the metal LCD frame.
7. Close the case. Check it boots, charges, and that TI Connect still works over the mini-USB port.

How the USB port is shared
----------------

The C3 controls its own connection, so the stock mini-USB port keeps working:

| C3 state | ID pin | C3 USB pull-up | Meaning |
|---|---|---|---|
| **RELEASED** | floating | off (invisible) | Stock calculator behaviour; a PC/TI Connect works normally |
| **ARMED** | grounded | on | Ready: when `WIFI` starts, the calc becomes host and enumerates the C3 |
| **ACTIVE** | grounded | on | `WIFI` said `HELLO`; commands are being served |

How it moves between states:
- **RELEASED → ARMED** once VBUS has been off for 1 s.
- **ARMED → RELEASED** if VBUS shows up but nobody sends `HELLO` within 8 s. That means a PC cable was plugged in, so the C3 gets out of the way.
- **ACTIVE → RELEASED** on `BYE`, which `WIFI` sends when you quit.

The onboard LED shows the state: off = released, short blink = armed, solid = active.

Notes:
- Quit `WIFI` before plugging in a PC cable.
- If TI Connect doesn't see the calculator right away, wait about 10 s or replug the cable once.

Power
----------------

- The firmware runs at 80 MHz and turns the Wi-Fi radio off after 2 minutes without the `WIFI` program.
- Idle draw is still roughly 15–20 mA, so fit the slide switch if you want full battery life.

Firmware updates over Wi-Fi (OTA)
----------------

While `WIFI` is running and connected, the C3 shows up as `ti84-wifi.local` on your network:

```sh
pio run -e ota -t upload
```

Protocol
----------------

- One command per line: `CMD args\n`.
- The C3 answers with zero or more data lines, then exactly one `OK [info]` or `ERR <reason>` line.
- A data line that happens to start with `OK`/`ERR` is sent with a leading space.

| Command | Reply |
|---|---|
| `HELLO` | `OK TI84-WIFI 1.0` (marks the link ACTIVE) |
| `PING` | `OK PONG` |
| `STATUS` | version, Wi-Fi state, SSID, IP, RSSI |
| `SCAN` | one line per network: `RSSI *SSID` (`*` = secured) |
| `JOIN ssid<TAB>pass` | `OK <ip>` or `ERR could not connect` (credentials saved) |
| `FORGET` | erases saved credentials |
| `SETUP` | starts the `TI84-Setup` phone portal for 3 minutes |
| `GET <url>` | page text: HTML tags stripped, ASCII only, max 2 KB, then `OK HTTP <code>` |
| `TIME` | `YYYY-MM-DD HH:MM:SS` from NTP |
| `TZ <posix tz>` | sets the time zone, e.g. `TZ EST5EDT,M3.2.0,M11.1.0` |
| `KEY <api key>` | saves a Gemini API key on the C3 (`KEY` alone clears it) |
| `MODEL <name>` | sets the Gemini model; `MODEL` alone resets to `gemini-flash-latest` |
| `ASK <question>` | Gemini's answer as plain ASCII, wrapped to 26 columns |
| `BYE` | releases the USB bus |

All text replies are word-wrapped to the calculator's 26-column screen.

Gemini
----------------

Get a free API key at [aistudio.google.com](https://aistudio.google.com), then send it once from a serial monitor on the bench build:

```
KEY <your key>
```

The key is stored in the C3's flash, never in this repo. On the calculator, pick **8: Ask Gemini**. If the model is overloaded (HTTP 503) or rate-limited (429), the C3 retries once with `gemini-flash-lite-latest`.

Typing passwords on the keypad is a pain. Press [alpha][alpha] for lowercase, or use **6: Setup via phone** instead.

Known unknowns (confirm on your hardware)
----------------

- **Pad locations** differ between board revisions. Always trust the meter, not a photo.
- **`srldrvce` and the C3's composite USB descriptor.** The C3 exposes a CDC serial interface plus a vendor JTAG interface. `srldrvce` searches for the CDC interface (`SRL_INTERFACE_ANY`), which should work, and the bench test in step 3 confirms it before you solder. If it fails, the fallback is to open the CDC bulk endpoints directly with `usb_GetDeviceEndpoint` / `usb_ScheduleBulkTransfer`.
- **Whether the calculator OS turns on VBUS by itself** when ID is grounded while `WIFI` isn't running. If the LED keeps cycling between blink and off with no cable plugged in, that's what's happening. Open an issue with your hardware revision.

Safety and fine print
----------------

- Li-ion batteries can be dangerous. Never short the battery terminals, and always fit the polyfuse.
- Opening the calculator voids the warranty.
- Wireless devices are banned on standardized tests and in most classrooms during exams. Using this mod where it isn't allowed is on you.
