# AW920K (Dell/Alienware tri-mode wireless) lighting protocol

Decoded from a USB capture (USBPcap + Wireshark) of Alienware Command Center
driving an **AW920K wireless keyboard** through its USB receiver, on
**2026-07-15**. Device: USB `413c:2126`, vendor HID interface `usagePage 0xff00`,
report lengths 65/65 (Windows convention, i.e. 64 bytes on the wire).

This is **not** the generic Chicony v8 protocol the SDK assumed. The differences
that made the v8 path silently fail on this device:

- **Report ID is `0x00`** (unnumbered). The first *data* byte is `0x0e`.
  The v8 path used report ID `0x01`.
- Color/effect payloads go out as an **interrupt OUT report** (endpoint `0x04`);
  the `0x51` "commit" and the brightness/power commands go as **HID feature
  reports** (control SET_REPORT, `wValue=0x0300` → Feature, report ID 0).

All payloads below are the 64 on-wire bytes (zero-padded on the right). Byte 0
is the literal `0x0e`.

## Commands

### Static color (interrupt OUT / output report)
```
0e 05 01 01 RR GG BB 00 00 00 00 BR 01 01 01
            ^^ ^^ ^^             ^^
            R  G  B              brightness (0x00..0x0a)
```
- byte[3] = `0x01` = static-color mode
- byte[4..6] = RGB
- byte[7..10] = second color (0 for static)
- byte[11] = brightness, `0x00`=off .. `0x0a`=full (same 0..10 scale as v8)
- byte[12..14] = `01 01 01`

Observed: RED `ff 00 00`, BLUE `00 00 ff`, a green preset `17 ff 15`.

### Commit / latch (feature report)
```
0e 05 01 51
```
Sent by AWCC immediately before each color/effect output report.

### Global brightness — "Go Light / Go Dim / Go Dark" (feature report)
```
0e 17 LV
```
- `LV = 0x00` → Go Dark (off)
- `LV = 0x03` → Go Dim
- `LV = 0x0a` → Go Light (full)

This is orthogonal to color: it only scales/zeroes brightness and preserves the
last color. This maps directly onto the app's top-level Go Light/Dim/Dark modes.

### Spectrum effect (interrupt OUT / output report)
```
0e 05 01 08 ff 00 00 00 00 00 19 0a 01 02 03
            ^^                   ^^ ^^
            speed?               |  brightness
                                 tempo(0x19)
```
- byte[3] = `0x08` = spectrum/rainbow mode
- byte[14] `03` = colour count (spectrum cycles 3+)

### Save preset (feature report) — NOT needed
```
0e 19 01 01
0e 19 02 02
```
Emitted only when the AWCC "Save Preset" button is pressed. Persists the current
look into the keyboard's onboard memory. The app applies colors live and does
**not** send these.

### Reactive / on-key-press
AWCC layers a base command `0e 05 01 09 00 FF FF 00 00 00 06 0a 01 02 01`
(byte[3]=`0x09`) together with a static color. Not yet fully decoded; the app
currently approximates on-key-press as always-on for this device.

## Application sequence (per apply)
AWCC repeats, for each change:
1. feature `0e 05 01 51` (commit/begin)
2. output `0e 05 01 09 ...` (base layer — appears optional)
3. feature `0e 05 01 51`
4. output `0e 05 01 01 RR GG BB .. BR ..` (the color)
5. (feature `0e 19 ..` only if Save Preset pressed)

The app sends the minimal working subset: commit → color output → commit.
