# ESP32 digital gauge cluster

This project is source-available and free for personal and educational use.
Commercial use is NOT permitted.

### *****Please do not use these files to sell this to others.  I made these so that those wanting to modify their dash/cluster dont have to spend an arm and a leg to do so.***** ###

Original project video: https://youtu.be/t7H6pevep40

Original round-display cluster by [traviscea](https://github.com/traviscea). The
7in combined dash on this branch is by [smark88](https://github.com/smark88).

---

## 7in combined dash

One 800x480 touch screen that replaces both round gauges. It talks to the car
itself over OBD-II on the board's own CAN transceiver: standard mode 01 PIDs plus
GM mode 22 enhanced PIDs from the ECM and the transmission (TCM). Built for a GM
LT4 (E92 ECM) with an 8L90 automatic.

Hardware: [Waveshare ESP32-S3-Touch-LCD-7](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-7)
(tested on Rev 1.2). Runs at a steady 62 FPS.

### Pages

Swipe left and right between them. The dots at the bottom show which page you
are on.

1. **Gauges**
   - RPM bar across the top, 0-7000. From 6500 (stock redline) it flashes blue
     as a shift light.
   - MPH, engaged gear (from the TCM) and P R N D M.
   - Tiles: water, oil temp, IAT, oil psi, trans temp, boost (vacuum shows
     negative).
   - Bottom row: fuel level, ethanol % (flex fuel), odometer.
   - A tile flashes red when its reading is out of range, and critical faults
     take over the whole screen (see Warnings).
   - Red **NO CAN** over the RPM bar if the car stops answering for 2s.
2. **Min / Max**: lowest and highest water, oil temp, oil psi, IAT 1 and 2,
   trans, boost and knock retard, for this drive and the last one.
   - Nothing records until the engine has run 5s, so key-on, engine-off
     readings never count.
   - Saved as you drive, so the record survives key-off. At the next power-on
     it becomes "last drive".
3. **Trouble codes**: separate **READ CODES** and **CLEAR CODES** buttons.
   - Reads every code each module holds (UDS 0x19, as HP Tuners does), marked
     current / pending / history, plus the generic OBD stored and pending codes.
   - Clear asks for confirmation first. Most ECMs only accept it with the key on
     and the engine off.

### Warnings

A tile flashes red while out of range. It clears only once the reading has
recovered past the clear value, and stays lit at least 2s once tripped.

| Tile | Trips | Clears | Full-screen takeover |
|---|---|---|---|
| Oil psi | below 15 | above 18 | after 3.5s (priority 1) |
| Water | above 245F | below 240F | after 5s |
| Oil temp | above 290F | below 285F | after 5s |
| Trans | above 260F | below 255F | after 5s |
| IAT | above 170F | below 165F | no |

- Oil pressure only alarms with the engine running, ignores the first 3s after
  start, and must stay low for 1s first, so one stray reading is not a failing
  pump.
- The takeover covers whichever page is showing. Tap it to acknowledge; it
  returns if the fault clears and comes back.
- A reading that stops updating shows "--" rather than freezing on its last
  value.

### Wiring

| Board | Car |
|---|---|
| CAN H | OBD-II pin 6 |
| CAN L | OBD-II pin 14 |
| GND | OBD-II pin 4/5, shared with the car |
| UART USB-C | 5V from a 12V-to-5V converter (2A or more) on switched power |

- **Remove the CAN 120R jumper** on the board. The car's bus is already
  terminated.
- Twist the CAN H / CAN L pair.
- Don't run another device that polls the car at the same time.

### Setup and flashing

1. Install VS Code and the ESP-IDF extension, with **ESP-IDF v5.4.4**.
2. Set the board's **UART1/UART2 switch to UART1**. Otherwise nothing can be
   flashed through the UART USB-C port.
3. Plug the board in by its **UART** USB-C port. In car mode the other (native
   USB) port is switched off, because CAN shares its pins.
4. Pick the run mode: `idf.py menuconfig` -> **Dash (7in)** -> **Run mode**.
   The committed default is Car.
   - **Car**: polls the car and runs the dash.
   - **Demo**: a simulated engine for the bench. CAN never starts. Optionally
     walks each tile through its warning so the alarms can be checked.
   - **PID scan**: sweeps GM mode 22 identifiers on the ECM to find a value no
     published list has. See [main/canbus/pid_scan.h](main/canbus/pid_scan.h).
   - **Log every OBD reply**: prints each answer on the UART port, for
     comparing against HP Tuners.
5. Build and flash:

   ```
   idf.py set-target esp32s3      # once
   idf.py build
   idf.py -p <port> flash
   ```

   On macOS the port shows up as `/dev/cu.usbmodem...`.

**Odometer:** set `ODO_SEED_MILES` in
[main/odometer/odometer.h](main/odometer/odometer.h) to the car's mileage
before the first flash. A new seed value is applied once; after that the
odometer keeps counting from what it has stored.

### What it reads

All values are polled. Mode 01 goes to the ECM (0x7E0); mode 22 goes to the ECM
or TCM (0x7E2).

| Value | Request | Checked against HP Tuners |
|---|---|---|
| RPM, speed, water, IAT, fuel level | mode 01 `0C` `0D` `05` `0F` `2F` | yes |
| Boost | mode 01 MAP `0B`, scaled by `4F`, minus baro `33` | yes (vacuum reads negative) |
| Oil temp | mode 22 `1154` (the ECM's calculated value) | yes |
| Oil pressure | mode 22 `115C`, psi = (A - 37) x 0.625 | yes |
| Ethanol | mode 22 `0052` | yes |
| Trans temp | mode 22 `1940` (TCM) | yes |
| Gear | mode 22 `199A` (TCM) | yes |
| P R N D | mode 22 `2889` (TCM) | yes |
| Knock retard | mode 22 `11A6` | no |
| IAT 2 | mode 22 `0068`, byte C | no |

### Source layout

| Path | What's there |
|---|---|
| `main/main7.c` | Start-up, the 16ms update loop, odometer |
| `main/board7/` | LCD, LVGL port, touch (GT911), CH422G I/O expander |
| `main/canbus/` | CAN driver, OBD poller (`obd_poll.c`: the PID table), trouble codes, PID scan |
| `main/ui7/` | The three pages, swipe navigation, warning limits (`dash7.h`) |
| `main/demo/` | Bench simulator |
| `main/odometer/` | Odometer kept in flash |

---

## Original round display

The original 480x480 round-display firmware is kept in this tree, unchanged.
Build it with:

```
idf.py -DDASH_TARGET=round build
```

1. Install [visual studio code](https://code.visualstudio.com/Download)
2. Install the ESP-IDF extension(shown here: https://www.waveshare.com/wiki/ESP32-P4-WIFI6-Touch-LCD-3.4C#Introduction_to_ESP-IDF_and_Environment_Setup_.28VSCode_Column.29)
    * Open VSCode Package Manager
    * Search for the official ESP-IDF ide extension
3. Open this repo folder in VS code
4. Plug in Esp32s3/screen via usb c
5. Set target device to esp32s3(see "Description of Bottom Toolbar of VSCode User Interface" in the waveshare link above)
6. Build

7. Flash
