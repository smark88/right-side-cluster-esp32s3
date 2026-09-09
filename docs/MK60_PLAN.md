# MK60E1 ABS integration — plan

Status: **spec only, nothing built.** Every frame and feature below is gated on
a listen-only CAN scan of the actual car (see [Prerequisites](#prerequisites)).
The research is done, so once the scan confirms the bus, the build is mechanical.

This is the natural home for the work: the MK60E1 data would land on **this**
board (the right-side / S3 cluster and its planned 2.8" companion), not the
center P4 cluster. Cross-repo references below point at the sibling
`center-cluster-esp32-p4` repo, where the GM powertrain decoding and the
gauge-to-gauge bridge already live:

- `center-cluster-esp32-p4/main/canbus/protocols/GM_NOTES.md` — GM side
- `center-cluster-esp32-p4/main/canbus/can_bridge.h` — how a value reaches every screen

---

## Goal

Pull vehicle dynamics and brake-system data off a retrofitted **Continental/Teves
MK60E1** ABS module and show it:

1. **Max-G gauge** — lateral + longitudinal acceleration, peak-hold.
2. **ABS / stability indicator** — blinks blue while intervening, solid red on fault.
3. **Wheel speeds** — four corners, for a traction / slip display on the 2.8" screen.

Side effect worth having: the MK60E1 broadcasts **vehicle speed**, which may
remove the OBD `0x0D` speed poll and de-risk the swap (the ECM often has no road
speed of its own without ABS on the bus).

---

## Hardware facts

- MK60E1 speaks **BMW E90 BN2000 PT-CAN at 500 kbit/s**.
- Broadcasts **automatically** — no keep-alive/wake frame (unlike the E46 MK60,
  which needs a `0x610` handshake). Needs 12V on pin 29 to wake.
- It is the **E1**, not the E5: same message layout except the master-cylinder
  and per-wheel **brake-pressure bytes read empty**. Everything used below
  (speed, accel, yaw, wheel speed, ABS state) is unaffected.
- Needs a CAN transceiver on the ESP32-S3 (SN65HVD230). The S3 has a TWAI
  controller but no on-board transceiver.

---

## Verified CAN layout

From the reference DBC everyone cites, `mck1117/abs-docs` → `MK60e5_CAN.dbc`:

```
gh api repos/mck1117/abs-docs/contents/MK60e5_CAN.dbc --jq '.content' | base64 -d
```

Some community posts put yaw/accel on `0x12F` and wheel speeds on `0x1A0`.
**Both are wrong per this DBC** — accel and yaw are in `0x1A0`, wheel speeds are
in `0x0CE`. Trust the scan over either.

### 0x1A0 (416) `SPEED` — speed + all acceleration in one frame

| Signal | Bits | Type | Scale | Unit | Meaning |
|---|---|---|---|---|---|
| `V_VEH`            | 0\|12  | unsigned | 0.1   | km/h  | vehicle speed |
| `ACLN_VEH_LN_DSC`  | 16\|12 | signed   | 0.025 | m/s²  | longitudinal accel |
| `ACLN_VEH_ACRO_DSC`| 28\|12 | signed   | 0.025 | m/s²  | lateral accel |
| `ANGV_YAW_DSC`     | 40\|12 | signed   | 0.05  | deg/s | yaw rate |

Accel range ±51.175 m/s² = **±5.2 G**. Little-endian, 12-bit two's complement.

### 0x19E (414) `ST_DSC` — brake-system state (all passive)

| Signal | Bits | Meaning |
|---|---|---|
| `ST_CLCTR` | 0\|8 | live intervention bitmask: `1`=ABS `2`=ASR `4`=DSC/TCS `8`=HBA `16`=MSR `32`=EBV |
| `ST_ABS`   | 8\|2 | `0`=OK `1`=Fallback `2`=Malfunction |
| `ST_DSC`   | 10\|3 | `0`=DSC ON `1`=DSC OFF `2`=DSC FAULT `4`=DTC/MDM `5`=ABS FAULT `6`=under-volt |
| `BrakeSwitch` | 46\|1 | brake pedal |
| `BRP` | 48\|8 | brake pressure, bar (may be empty on the E1) |

The high-level fault state is **broadcast** — a warning light needs no polling.
Only detailed DTC *codes* would need UDS (`0x19` ReadDTCInformation to the ABS
tester ID, typically `0x6F1`/`0x712`), and a lamp doesn't need those.

### 0x0CE (206) `WHEEL_SPEEDS` — four corners

| Signal | Bits | Scale | Unit |
|---|---|---|---|
| `V_WHL_FLH` | 0\|16  | 0.0625 | km/h |
| `V_WHL_FRH` | 16\|16 | 0.0625 | km/h |
| `V_WHL_RLH` | 32\|16 | 0.0625 | km/h |
| `V_WHL_RRH` | 48\|16 | 0.0625 | km/h |

Signed, little-endian.

---

## Features

### Max-G gauge

```
g = sqrtf(lat*lat + lon*lon) / 9.81f;   // lat, lon in m/s² from 0x1A0
```

Hold the peak (reset on key cycle or a long touch). ±5.2 G full scale is ample.
Yaw rate is decoded in the same frame, free if a rotation display is ever wanted.

### ABS / stability indicator

| State | Condition | Colour | Style |
|---|---|---|---|
| Intervening | `ST_CLCTR & 1` (ABS) | **blue** | blink, ≥1 s min hold |
| Fault | `ST_DSC == 2 \|\| ST_DSC == 5` or `ST_ABS == 2` | **red** | solid |

Priority: **red wins** — `red if faulted, else blue if intervening, else off`.

- **Min hold is required.** An ABS burst can be 100–200 ms; without a hold it
  flashes for one frame and is invisible.
- **Traction is the likelier event on a 650 hp LT4.** Consider blinking on
  `ST_CLCTR & (1|2|4)` (ABS + ASR + DSC), or a separate mark for traction vs
  braking.

### Wheel speeds / traction screen

Four corners from `0x0CE`, on the planned **2.8" ESP32-S3 board**
(`ESP32-S3-Touch-LCD-2.8`, 240×320 ST7789). Slip = driven vs undriven average.
Separate firmware from the P4 gauges, but it subscribes to the bridge like any
other node.

---

## Bridge integration

Whichever board taps the MK60E1 bus decodes these frames and republishes them on
the bridge so the other screens get them without a second tap. The bridge is
currently full (`0x7F0`–`0x7F3`, 16 slots), so this needs a **5th frame `0x7F4`**
(and a 6th for all four wheels). Slots are append-only — never renumber, or older
firmware mis-decodes.

Suggested `0x7F4`: `g_current`, `g_peak`, `abs_state` (ST_CLCTR), `dsc_state`
(ST_DSC). Wheel speeds, if carried, in `0x7F5`.

---

## Prerequisites

Nothing here is safe to assume until a **listen-only CAN scan** of the actual car
confirms:

1. **The MK60E1 is on the bus the board taps.** These are BMW IDs on a GM car;
   the module may be on its own segment. If `0x1A0` / `0x19E` / `0x0CE` show in
   the scan, it's reachable.
2. **A yaw sensor is wired to the module.** The accel/yaw signals in `0x1A0` only
   populate if a separate sensor is connected. Many ABS-only installs have none —
   then there's no G to show, only speed and wheel speeds.
3. **No ID collision with GM Global A.** `0x0CE` / `0x19E` / `0x1A0` are low IDs;
   if the MK60E1 and the ECM share one physical bus, verify they don't clash.

---

## References

- `mck1117/abs-docs` — `MK60e5_CAN.dbc`, the byte layout above
- `peterakimball/mk60_can_tool` — Arduino MK60 translator, wake/handshake notes
- LotusTalk / MiataTurbo MK60 swap threads — E1 vs E5 differences
