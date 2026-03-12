# Deep Rig Integration Strategy

Research document covering SWR metering, ATU control, filter bandwidth, and RF gain
across all CAT protocols POTACAT supports.

## Current State

POTACAT has three radio connection paths:

1. **CatClient** (lib/cat.js) -- Serial or TCP, Kenwood-style FA/MD/FW commands, auto-detects Yaesu 9-digit FA
2. **RigctldClient** (lib/cat.js) -- TCP to Hamlib rigctld, simple ASCII protocol (`f`, `F`, `m`, `M`, `L`, `U`)
3. **SmartSdrClient** (lib/smartsdr.js) -- FlexRadio TCP API port 4992, already has `setAtu()`, `setRfGain()`, `setSliceFilter()`, `tuneSlice()`

SmartSDR already supports all four requested features. The gap is in CatClient (serial/Kenwood) and RigctldClient (Hamlib).

---

## 1. Hamlib rigctld Capabilities

rigctld exposes levels and functions via a simple text protocol. These are the relevant commands for each requested feature.

### SWR Reading

```
\get_level SWR        -->  returns float (e.g., 1.5)
\get_level ALC        -->  returns float (0.0-1.0)
\get_level RFPOWER_METER       -->  returns float (0.0-1.0 of max)
\get_level RFPOWER_METER_WATTS -->  returns float (actual watts)
\get_level STRENGTH   -->  returns int dB relative to S9
```

**Protocol over TCP:**
```
l SWR\n        --> e.g., "1.500000\n"
l ALC\n        --> e.g., "0.250000\n"
```

**Important caveats:**
- `get_level SWR` only returns meaningful data during transmit. Polling during RX returns 0 or stale values.
- Not all backends implement SWR. Send `l ?\n` to query which levels the current rig supports -- returns a space-separated list of supported tokens.
- Hamlib normalizes SWR as a float (1.0 = perfect, 3.0 = 3:1). No conversion needed.
- The METER level can also be used: `L METER SWR\n` selects which meter the radio displays (some rigs only).

### ATU / Antenna Tuner

```
\set_func TUNER 1     -->  enable ATU
\set_func TUNER 0     -->  disable ATU
\get_func TUNER       -->  returns 0 or 1
```

**Protocol over TCP:**
```
U TUNER 1\n    --> enable
U TUNER 0\n    --> disable
u TUNER\n      --> query, returns "1\n" or "0\n"
```

**Caveats:**
- This enables/disables the tuner. It does NOT initiate a tune cycle on most backends.
- Some Hamlib backends map `set_func TUNER 1` to a tune-start command, but behavior varies.
- There is no dedicated "start tune" command in Hamlib. For radios that require a separate tune initiation (Yaesu AC002, Icom 1C 01 01), Hamlib's TUNER func may or may not trigger it depending on the backend.

### Filter / Passband Width

rigctld handles passband through the mode command:

```
\set_mode USB 2400    -->  set mode USB with 2400 Hz passband
\set_mode CW 500      -->  set mode CW with 500 Hz passband
\set_mode USB 0       -->  set mode USB with radio default passband
```

**Protocol over TCP:**
```
M USB 2400\n   --> set mode + passband
m\n            --> returns "USB\n2400\n" (mode then passband)
```

POTACAT's RigctldClient already uses this -- `setFilterWidth()` sends `M <mode> <hz>`.

Additionally, passband tuning is available via levels:
```
l PBT_IN\n     --> IF passband tuning inner (Hz offset)
l PBT_OUT\n    --> IF passband tuning outer (Hz offset)
L SLOPE_LOW <hz>\n   --> low-cut slope filter
L SLOPE_HIGH <hz>\n  --> high-cut slope filter
```

### RF Gain

```
\set_level RF 0.500   -->  set RF gain to 50% (float 0.0-1.0)
\get_level RF         -->  returns float 0.0-1.0
```

**Protocol over TCP:**
```
L RF 0.500\n   --> set to 50%
l RF\n         --> returns e.g., "0.750000\n"
```

POTACAT's RigctldClient already has `setRfGain(val)` which sends `L RFGAIN <val>`. Note: the correct token is `RF` for RF gain (receive), `RFGAIN` is not a standard Hamlib level. The existing code should be verified -- Hamlib 4.x uses `RF` as the level token for RF gain.

### Capability Discovery

Before using any level/func, query what the rig supports:
```
l ?\n          --> space-separated list of supported get_level tokens
L ?\n          --> space-separated list of supported set_level tokens
u ?\n          --> space-separated list of supported get_func tokens
U ?\n          --> space-separated list of supported set_func tokens
```

This is critical for graceful degradation -- POTACAT should probe capabilities on connect and only enable UI elements for features the current rig+backend supports.

---

## 2. Yaesu CAT Commands (FT-991A, FT-710, FTDX10, FTDX101)

Yaesu modern HF radios share a common CAT protocol: 2-letter ASCII commands terminated by `;`. POTACAT already handles FA, MD, SH, NB for Yaesu.

### SWR Reading

**Command: `RM` (Read Meter)**
```
Query:    RM1;          --> request SWR meter
Response: RM1PPP;       --> PPP is 000-255 (meter deflection)
```

Meter type parameter (P1):
| P1 | Meter        |
|----|-------------|
| 0  | S-Meter (RX)|
| 1  | PO (Power)  |
| 2  | ALC         |
| 3  | COMP        |
| 4  | VDD         |
| 5  | ID          |
| 6  | SWR         |

So `RM6;` reads SWR, response is `RM6PPP;` where PPP is 000-255. The mapping from PPP to actual SWR ratio is nonlinear and rig-specific. Approximate mapping for FT-991A:
- 000 = SWR 1.0
- 050 = SWR 1.5
- 080 = SWR 2.0
- 120 = SWR 3.0
- 200+ = SWR > 5.0

Like Hamlib, SWR is only meaningful during TX.

### ATU Control

**Command: `AC` (Antenna Tuner Control)**
```
Set:      AC00P;        --> P: 0=OFF, 1=ON, 2=TUNE START
Query:    AC;
Response: AC00P;
```

- `AC002;` starts a tune cycle (equivalent to pressing TUNE on the radio)
- `AC001;` enables the tuner (memorized match)
- `AC000;` bypasses the tuner

### Filter / Bandwidth

**Command: `SH` (Width) -- Yaesu-specific indexed values**
```
Set:      SH0nn;        --> nn is a 2-digit index (01-21 for SSB, 01-16 for CW)
Query:    SH0;
Response: SH0nn;
```

POTACAT already has `yaesuBwToIndex()` mapping Hz to index and `setFilterWidth()` using SH0. This works.

There is also `NA` (Narrow) for simple narrow/wide toggle on some models.

### RF Gain

**Command: `RG` (RF Gain)**
```
Set:      RG0nnn;       --> nnn is 000-255 (0=min, 255=max)
Query:    RG0;
Response: RG0nnn;
```

The `0` after RG is a VFO selector (main VFO). On the FT-710 and newer radios the format is the same.

### Notes on Yaesu

- All commands use `;` terminator (already handled by CatClient's buffer parser)
- The `0` prefix on SH0, RG0, MD0, NB0 is the VFO-A selector -- consistent across modern Yaesu
- FT-710 shares the same command set as FT-991A with minor additions
- FTDX101 adds some commands but the core set (RM, AC, SH, RG) is identical

---

## 3. Kenwood / Elecraft CAT Commands

Kenwood-protocol radios (TS-590, TS-890, Elecraft K3/K3S/K4, KX2/KX3) share a common protocol that POTACAT already speaks natively.

### SWR Reading

**Elecraft K3/K4 -- `BG` (Bargraph) and `TM` (Transmit Meter)**

K3/K3S:
```
Query:    BG;
Response: BGnn;         --> nn is 00-10 (bargraph segments)
```
The BG command returns the current bargraph reading. What it represents depends on the TX meter mode.

K4 transmit meter autoresponse:
```
TM;                     --> returns TMaaabbbcccddd;
                        --> aaa = ALC (bars)
                        --> bbb = CMP (dB)
                        --> ccc = FWD power (watts, or tenths in QRP)
                        --> ddd = SWR in 1/10th units (e.g., 015 = 1.5:1)
```

This is much more useful: SWR is directly available in 0.1 resolution.

**Kenwood TS-890/TS-590:**
```
RM;           --> returns RM1nnnn; (meter reading)
              --> first digit selects meter: 1=SWR/RF, 2=COMP/ALC
              --> nnnn = 0000-0030 (bargraph segments)
```

The TS-890 returns `RM1nnnn;` where nnnn is the current meter value (0-30 scale). Selecting SWR vs RF power display is done via `RM2;` / `RM1;`.

### ATU Control

**Kenwood TS-890/TS-590:**
```
AC;           --> query tuner state
AC0nn;        --> nn: 00=OFF, 11=ON (RX thru), 12=ON (tune start)
```
`AC011;` enables the tuner, `AC012;` starts a tune cycle.

**Elecraft K3/K4:**
```
SWTnn;        --> switch command, where nn corresponds to ATU button
              --> SWT20; = single tap ATU (toggle on/off)
              --> SWT21; = hold ATU (force re-tune)
```
Or use the `TU` command on K4:
```
TU3;          --> tap ATU once (on/off toggle)
TU4;          --> second tap within 5s (force tune cycle)
```
On K3/K3S: `SWT44;` is the TUNE button.

### Filter / Bandwidth

**`BW` command (Kenwood/Elecraft):**
```
Set:      BWnnnn;       --> nnnn = bandwidth in Hz / 10
                        --> e.g., BW0240; = 2400 Hz
Query:    BW;
Response: BWnnnn;
```

K4 format: `BW$nnnn;` where $ targets VFO B.

**`FW` command (Kenwood TS-series):**
```
Set:      FWnnnn;       --> nnnn = filter width in Hz
Query:    FW;
Response: FWnnnn;
```

POTACAT already sends `FW` for Kenwood radios in `setFilterWidth()`. The Elecraft BW command has a different scale (x10) -- this needs handling if Elecraft is detected.

### RF Gain

**`RG` command:**
```
Set:      RGnnn;        --> nnn = 000-250 (K3/K4 scale)
Query:    RG;
Response: RGnnn;
```

K4: `RG$nnn;` for VFO B. The range 000-250 maps to full RF gain range. On Kenwood TS-890, the scale is 000-255.

---

## 4. Icom CI-V Protocol (IC-7300, IC-7610, IC-7760, IC-705)

Icom uses a binary protocol (CI-V) with a fixed frame format. This is fundamentally different from the ASCII protocols above.

### Frame Format
```
FE FE <to> <from> <cmd> [<sub>] [<data>...] FD
```
- `FE FE` = preamble
- `<to>` = radio address (IC-7300 default: 0x94, IC-7610: 0x98)
- `<from>` = controller address (typically 0xE0)
- `FD` = end of message

### SWR Reading

**Command 0x15 (Read Meter)**
```
Send:    FE FE 94 E0 15 12 FD       --> read SWR meter (sub 0x12)
Reply:   FE FE E0 94 15 12 DD DD FD --> DD DD = BCD value 0000-0255
```

Sub-commands for 0x15:
| Sub  | Meter          |
|------|----------------|
| 0x02 | S-Meter        |
| 0x11 | PO (Power)     |
| 0x12 | SWR            |
| 0x13 | ALC            |
| 0x14 | COMP           |
| 0x15 | VDD            |
| 0x16 | ID             |

SWR values (BCD 0000-0255):
- 0000 = SWR 1.0
- 0048 = SWR 1.5
- 0080 = SWR 2.0
- 0120 = SWR 3.0

### ATU Control

**Command 0x1C sub 0x01 (Antenna Tuner)**
```
Send:    FE FE 94 E0 1C 01 01 FD    --> start tune
Send:    FE FE 94 E0 1C 01 02 FD    --> enable tuner (on)
Send:    FE FE 94 E0 1C 01 00 FD    --> tuner off/thru
Query:   FE FE 94 E0 1C 01 FD       --> read tuner state
Reply:   FE FE E0 94 1C 01 DD FD    --> DD: 00=off, 01=on, 02=tuning
```

### Filter / Passband Width

**Command 0x1A sub 0x03 (IF Filter Width)**
```
Send:    FE FE 94 E0 1A 03 DD DD FD --> set filter width
                                     --> DD DD = BCD filter index or Hz
Query:   FE FE 94 E0 1A 03 FD       --> read current filter
Reply:   FE FE E0 94 1A 03 DD DD FD
```

The filter system is model-specific. IC-7300 uses FIL1/FIL2/FIL3 selectable filters with adjustable width within each:
- Command 0x06 selects filter (01/02/03)
- Command 0x1A 0x03 reads/sets the passband width for the current filter

Passband tuning (PBT) shift:
```
Command 0x14 sub 0x07: PBT inner (0000-0255, center=128)
Command 0x14 sub 0x08: PBT outer (0000-0255, center=128)
```

### RF Gain

**Command 0x14 sub 0x02 (RF Gain)**
```
Send:    FE FE 94 E0 14 02 DD DD FD --> set RF gain
                                     --> DD DD = BCD 0000-0255
Query:   FE FE 94 E0 14 02 FD       --> read RF gain
Reply:   FE FE E0 94 14 02 DD DD FD
```

### CI-V Implementation Challenge

Icom CI-V is binary, not ASCII. This requires a fundamentally different transport layer:
- Framing: scan for FE FE preamble, read until FD terminator
- BCD encoding: values like `0x01 0x28` = 128 decimal
- Radio address varies per model (must be configurable or auto-detected)
- Collision detection: CI-V is half-duplex bus, though USB eliminates this
- Echo handling: radio echoes sent commands back on the bus

POTACAT does NOT currently have CI-V support. Adding it would be a significant new module.

---

## 5. FlexRadio SmartSDR API (Already Implemented)

For reference, the existing SmartSdrClient already supports:

### SWR Reading
```
sub meter all          --> subscribe to all meter streams
```
Meters arrive as UDP Vita-49 packets or status messages. SWR, forward power, reflected power are standard meters. Current code subscribes to `sub atu all` but does not parse meter data.

### ATU Control
```
atu start              --> initiate tune cycle
atu bypass             --> bypass tuner
```
Already implemented as `setAtu(on)`.

### Filter Width
```
slice set <n> filter_lo=<hz> filter_hi=<hz>
```
Already implemented in `setSliceFilter()` and `tuneSlice()`.

### RF Gain
```
slice set <n> rfgain=<dB>
```
Already implemented as `setRfGain(idx, dB)`. Range is typically -10 to +20 dB.

---

## 6. Architecture Proposal

### Recommended: Hybrid Approach (Option C)

Neither pure-rigctld nor pure-rig-specific is ideal. The recommended approach layers both:

```
                    +------------------+
                    |   POTACAT UI     |
                    | (SWR bar, ATU    |
                    |  btn, RF slider) |
                    +--------+---------+
                             |
                    +--------v---------+
                    |   RigController   |  <-- new abstraction layer
                    | (unified API)     |
                    +--+-----+------+--+
                       |     |      |
              +--------+  +--+--+  +--------+
              |           |     |           |
    +---------v--+ +------v-+ +-v----------+
    | CatClient  | |Rigctld | | SmartSDR   |
    | (Kenwood/  | |Client  | | Client     |
    | Yaesu)     | |        | | (existing) |
    +------------+ +--------+ +------------+
```

### RigController Unified API

```javascript
class RigController extends EventEmitter {
  // Capabilities (populated on connect)
  capabilities = {
    swr: false,          // can read SWR
    atu: false,          // can control ATU
    atuTune: false,      // can initiate tune cycle (not just on/off)
    filterWidth: false,  // can set filter bandwidth
    rfGain: false,       // can set RF gain
    rfGainRange: [0, 1], // min/max for RF gain
    txPower: false,      // can set TX power
    meters: [],          // list of available meters
  };

  // Events emitted
  // 'swr', value         -- SWR ratio (float, 1.0+)
  // 'power', watts       -- TX power in watts
  // 'alc', value         -- ALC (float 0-1)
  // 'atu-status', state  -- 'off' | 'on' | 'tuning' | 'error'
  // 'rf-gain', value     -- normalized 0-1
  // 'capabilities', caps -- when capabilities change

  // Methods
  getSwr()               // poll SWR (returns via event)
  setAtu(on)             // enable/disable ATU
  startTune()            // initiate ATU tune cycle
  setFilterWidth(hz)     // set passband width
  setRfGain(normalized)  // 0.0-1.0
  probeCapabilities()    // query what this rig supports
}
```

### Implementation by Transport

**RigctldClient additions:**
```javascript
// On connect, probe capabilities
_probeCapabilities() {
  this._write('l ?\n');   // supported get_level tokens
  this._write('L ?\n');   // supported set_level tokens
  this._write('u ?\n');   // supported get_func tokens
  this._write('U ?\n');   // supported set_func tokens
  // Parse responses to populate capabilities object
}

// SWR polling (only during TX)
_pollMeters() {
  if (this.capabilities.swr) this._write('l SWR\n');
  if (this.capabilities.txPower) this._write('l RFPOWER_METER_WATTS\n');
}

// ATU
startTune() {
  this._write('U TUNER 1\n');  // best we can do via rigctld
}

// RF gain
setRfGain(normalized) {
  this._write(`L RF ${normalized.toFixed(3)}\n`);
}
```

**CatClient additions (Kenwood/Yaesu):**

The CatClient already detects Yaesu vs Kenwood via `_isYaesu()`. Extend with:

```javascript
// SWR polling
_pollSwr() {
  if (this._isYaesu()) {
    this._write('RM6;');       // Yaesu SWR meter
  } else if (this._isElecraft) {
    // K4: use TM autoresponse (already streaming)
    // K3: BG; (bargraph, depends on meter mode)
  } else {
    // Kenwood TS-series: RM;
    this._write('RM;');
  }
}

// Parse SWR responses
// In _onData():
//   Yaesu: RM6PPP; --> PPP/255 * nonlinear curve
//   Kenwood: RM1nnnn; --> nnnn/30 scale
//   Elecraft K4: TMaaabbbcccddd; --> ddd/10 = SWR

// ATU
startTune() {
  if (this._isYaesu()) {
    this._write('AC002;');     // Yaesu: tune start
  } else if (this._isElecraft) {
    this._write('SWT44;');     // K3: TUNE button
    // or TU4; for K4
  } else {
    this._write('AC012;');     // Kenwood: tune start
  }
}

// RF gain
setRfGain(normalized) {
  const max = this._isElecraft ? 250 : 255;
  const val = Math.round(normalized * max);
  if (this._isYaesu()) {
    this._write(`RG0${String(val).padStart(3, '0')};`);
  } else {
    this._write(`RG${String(val).padStart(3, '0')};`);
  }
}
```

### Elecraft Detection

POTACAT currently only distinguishes Yaesu (9-digit FA) from Kenwood (11-digit FA). Elecraft K3/K4 uses 11-digit FA like Kenwood, but has different commands for some features. Detection options:

1. **Probe with K4-specific command:** Send `K4;` or `OM;` -- K4 responds with model info, non-Elecraft returns `?`
2. **User setting:** Add "Rig family" dropdown in Settings: Auto / Kenwood / Elecraft / Yaesu
3. **Hybrid:** Auto-detect with probe, allow manual override

Recommendation: Add a `rigFamily` setting with "Auto" default. On first connect, probe with `OM;` (Elecraft firmware query). If response starts with `OM`, set `_isElecraft = true`. Otherwise fall back to Yaesu/Kenwood detection via FA digits.

### Icom CI-V: Defer or Use rigctld

**Do NOT implement CI-V natively.** The binary protocol is complex and every model uses different addresses. Icom users should use rigctld, which handles CI-V internally and exposes the same ASCII interface. This gives POTACAT Icom support for free through the rigctld path.

If direct Icom support is later demanded, create a separate `CivClient` class in `lib/civ.js` with proper binary framing. But this is a large effort for incremental benefit over rigctld.

### SWR Polling Strategy

SWR is only meaningful during TX. Polling it during RX wastes bandwidth and returns stale/zero values. Strategy:

1. **TX state detection:** CatClient already handles TX;/RX; commands. Track TX state via a `_transmitting` flag.
2. **TX-triggered meter polling:** When TX state changes to true, start a fast meter poll (200ms interval). When TX ends, stop meter polling and emit a final SWR=0 to clear the display.
3. **For rigctld:** Poll TX state with `t\n` (get PTT) periodically, then poll SWR when PTT=1.
4. **For SmartSDR:** Meter subscriptions are push-based (status messages), no polling needed.

### UI Proposal

Add a collapsible "Radio" panel below the main toolbar (or as a status bar extension):

```
[SWR: 1.3] [PWR: 50W] [ATU: ON | TUNE] [RF: ████████░░ 80%] [FIL: 2.4kHz]
```

- **SWR bar:** Green/yellow/red gradient, only visible during TX
- **ATU button:** Toggle on/off, long-press or shift-click to force tune
- **RF gain slider:** Horizontal, 0-100%, live update
- **Filter width:** Dropdown with common presets (200, 500, 1000, 1800, 2400, 3000 Hz) plus custom

### Settings Changes

```javascript
// New settings
rigFamily: 'auto',          // 'auto' | 'kenwood' | 'elecraft' | 'yaesu' | 'icom'
showRigPanel: false,        // show the rig control panel
swrPollRate: 200,           // ms, during TX only
```

### Implementation Priority

| Phase | Feature | Effort | Impact |
|-------|---------|--------|--------|
| 1     | ATU control (all transports) | Small | High -- band changes trigger ATU |
| 2     | SWR display (rigctld + CatClient) | Medium | High -- visual safety feedback |
| 3     | RF gain control | Small | Medium -- convenience |
| 4     | Filter width enhancement | Small | Medium -- already mostly done |
| 5     | Elecraft detection + K4 TM parsing | Medium | Low -- niche user base |

Phase 1 is the quick win: `startTune()` on band change is a single command per transport, requires no polling or UI beyond a toolbar button.

---

## 7. Key Risks and Mitigations

**Risk: Polling SWR too fast causes CAT bus contention**
- Mitigation: 200ms minimum interval, pause SWR polling during tune commands (same pattern as _startPolling pause in tune())

**Risk: ATU tune while POTACAT is polling causes garbled responses**
- Mitigation: Pause all polling during ATU tune cycle, resume after 3-5s or after AC/1C status response confirms done

**Risk: rigctld backend doesn't support SWR for this rig model**
- Mitigation: Capability probing on connect; disable UI elements when unsupported

**Risk: Yaesu SWR values need nonlinear calibration per model**
- Mitigation: Use a generic approximation curve. Exact SWR numbers are less important than relative indication (good/caution/danger zones).

**Risk: Elecraft K3 vs K4 command differences**
- Mitigation: K4 is a superset of K3. Use K3 commands as baseline, add K4 features (TM autoresponse) when K4 detected.

---

## References

- [Hamlib rigctl man page](https://hamlib.sourceforge.net/html/rigctl.1.html)
- [Hamlib rigctld man page](https://hamlib.sourceforge.net/html/rigctld.1.html)
- [Hamlib rigctl ManKier reference](https://www.mankier.com/1/rigctl)
- [Hamlib rig.h header (level/func definitions)](https://github.com/Hamlib/Hamlib/blob/master/include/hamlib/rig.h)
- [Yaesu FT-991A CAT Operation Reference Manual](https://static.dxengineering.com/global/images/chartsguides/y/ysu-ft-991a_us.pdf)
- [Yaesu FT-710 CAT Operation Reference Manual](https://www.yaesu.com/Files/4CB893D7-1018-01AF-FA97E9E9AD48B50C/FT-710_CAT_OM_ENG_2306-C.pdf)
- [Elecraft K4 Programmer's Reference (rev C10)](https://ftp.elecraft.com/K4/Manuals%20Downloads/K4%20Programmer's%20Reference%20rev%20C10/K4ProgrammersReferencerev.C10.html)
- [Elecraft K3S/K3/KX3/KX2 Programmer's Reference](https://ftp.elecraft.com/KX2/Manuals%20Downloads/K3S&K3&KX3&KX2%20Pgmrs%20Ref,%20G5.pdf)
- [Elecraft K4 Programmer's Reference (PDF, rev D4)](https://ftp.elecraft.com/K4/Manuals%20Downloads/K4%20Programmer's%20Reference,%20rev.%20D4.pdf)
- [Icom IC-7610 CI-V Reference Guide](https://static.dxengineering.com/global/images/technicalarticles/ico-ic-7610_yj.pdf)
- [Icom IC-705 CI-V Reference Guide](https://www.icomeurope.com/wp-content/uploads/2020/08/IC-705_ENG_CI-V_1_20200721.pdf)
- [Icom IC-7300MK2 CI-V Reference Guide](https://icomuk.co.uk/files/icom/PDF/productAdditionalFile/IC-7300MK2_ENG_CI-V_0.pdf)
- [Icom IC-9700 CI-V Reference Guide](https://www.icomfrance.com/uploads/files/produit/not-IC-9700_ENG_CI-V_1-en.pdf)
- [Kenwood TS-890S PC Control Command Reference](https://www.kenwood.com/i/products/info/amateur/pdf/ts890_pc_command_en_rev1.pdf)
- [Kenwood TS-590S PC Control Command Reference](https://www.kenwood.com/i/products/info/amateur/pdf/ts_590_g_pc_command_e.pdf)
- [FlexRadio SmartSDR TCP/IP API Wiki](https://github.com/flexradio/smartsdr-api-docs/wiki/SmartSDR-TCPIP-API)
- [FlexRadio Community: Meter Subscription](https://community.flexradio.com/discussion/8032023/flex-tcp-ip-meter-subscription)
- [wfview -- Open Source Icom/Kenwood interface](https://wfview.org/)
- [991A-Commander (Yaesu CAT reference implementation)](https://github.com/rfrht/991A-Commander)
- [Icom CI-V Information (TR4W wiki)](https://github.com/n4af/TR4W/wiki/Icom-CI-V-Information)
- [IC-7300 TechNote: CI-V Controls Big Picture](https://www.g3nrw.net/ic-7300-files/IC-7300%20TechNote%20-%20CI-V%20Controls%20Big%20Picture%20v1.0.pdf)
