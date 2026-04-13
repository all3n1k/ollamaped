# TFT Display Protocol Discovery Guide

## Quick Start

Run discovery scripts from your PC via USB-UART (COM7) at 9600 baud.

### Step 1: Map All Images
```bash
python tft_fast_map.py COM7 9600
```
Watch display. Type description when image changes. Press ENTER to skip.

### Step 2: Find Responses
```bash
python tft_response_listener.py COM7 9600
```
Look for any UART responses (ACKs, echoes, data).

### Step 3: Test Multi-Byte Commands
```bash
python tft_sequence_tester.py COM7 9600
```

### Step 4: Full Suite
```bash
python tft_discovery_suite.py
```

## What to Look For

### Single-Byte Commands
Many cheap vape/car TFT displays use simple single-byte commands:
- Each byte = one stored image/page
- Example: `0x01` = battery screen, `0x02` = settings, etc.

### Multi-Byte Protocols
Look for these header patterns:

**DMT Protocol (0xAA header)**
```
AA [CMD] [LEN] [DATA...] [optional terminator]
```

**DWIN T5L Protocol (0x5A 0xA5 header)**
```
5A A5 [LEN] [CMD] [ADDR_H] [ADDR_L] [DATA...]
```

**UN-series Protocol (0xEB 0x90 wake)**
```
EB 90 [CMD] [SUB_CMD] [PARAMS...]
```

**Custom Protocol**
```
[PREFIX] [CMD_BYTE] [INDEX/PAGE]
```

## Known Working Patterns for Vape Displays

Many Firerose/Elux/Geekvape displays use this pattern:

| Byte | Image |
|------|-------|
| 0x01 | Vape logo (default) |
| 0x02 | Golden Gate Bridge |
| 0x03 | Battery indicator |
| 0x04 | Settings menu |
| 0x05 | Temperature screen |

## Common Protocol Structures

### Image/Page Switching
```
Single byte:        [PAGE_INDEX]
DMT style:          AA 01 00  [page index]
DWIN style:         5A A5 05 82 00 84 00 [page]
```

### Brightness Control
```
DWIN:               5A A5 04 82 00 82 [0x00-0x64]
```

### Read Register
```
DWIN:               5A A5 03 83 [ADDR_H] [ADDR_L]
```

## After Decoding

1. Edit `TFT_Display.h` — fill in `showPage()` and `setBrightness()` with discovered commands
2. Add `#include "TFT_Display.h"` to your spider firmware
3. Use `TFT_Display tft(2, 3);` with SoftwareSerial on pins 2/3
4. Remember level shifter between ATmega (5V) and display (3.3V)

## Pinout Reminder

```
Display Pad      →    Level Shifter LV    →    ATmega2560
─────────────────────────────────────────────────────────
VCC (3.3V)       →    LV VCC              →    3.3V
GND              →    LV GND              →    GND
U_TX.RX          →    LV1 + LV2 (bridged) →    Pin 2 (TX) via HV1
                                                   Pin 3 (RX) via HV2
TFT_DET          →    GND (permanent)
```
