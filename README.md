# U110 HexWizard

*An ESP32 maskrom programmer.*

Turn an **ESP32-S3** and a **SST39SF040** (512 KB, 5 V parallel flash) into a rewritable ROM that you fill
from a web page or over USB, in 10 to 20 seconds. Built to burn Roland U-110 sound cards
(see [U110 RomHex Studio](https://github.com/yuyoi/u110-romhex-studio)), but the programmer itself just writes a
512 KB image into the chip.

> **Status: working prototype.** It works on the bench (breadboard, one chip, one board type), but it is early:
> the pin map is one tested layout, only the ESP32-S3 DevKitC-1 (N16R8) has been tried, there is no read-back
> or verify in the firmware, and it needs a few manual quirks (see the power notes). Expect rough edges and
> changes. A proper PCB is planned.

**Verified on real hardware:** a full 512 KB card image was written and read back in a separate programmer
with **0 wrong bytes out of 524,288**, with no level shifters, no series resistors and no decoupling caps on the
breadboard. The parts are a few dollars: an ESP32-S3 DevKitC-1 (about $6), the flash chip, and jumper wires.

> **Made by JunkSmithWizard (JSW) together with Claude (Anthropic's AI).** Claude wrote the firmware; JSW built
> and debugged it on the bench (including finding the missing ground pin and the 5 V pin quirk below).

> **Warning: never leave the ESP connected to the chip while it is in a synth.** Burn with the chip **out** of the
> synth, then **unplug the chip from the breadboard / ESP wiring** before you put it in the card slot. If the ESP
> stays wired to the address and data lines, its pins load and clamp the synth's 5 V bus, add noise, and can
> back-power the board. Same rule the other way round: don't power the chip from both the ESP and the synth at once.
> This is a prototype workflow; the planned PCB will isolate the ESP from the card bus.

## What it does

- The ESP32-S3 drives the flash chip's address, data and write-enable lines directly from its 3.3 V GPIOs
  (the SST39SF040 needs only 2.0 V for a logic 1), while the chip itself runs from 5 V.
- **Write only.** It erases the whole chip, then programs every byte that isn't `FF`. It never reads the chip
  back, so verify a burn in your normal programmer (or just try the card).
- **Radio off while burning**: the WiFi radio is switched off during the erase and write, then comes back.
- **Every byte is programmed twice** (`PROGRAM_TRIES` in the sketch) to catch stray missed bits on a marginal supply.
- **Card info**: attach a short text (card name, tone names) to each image; the web page lists it.
- Keeps up to about 17 images (512 KB each) in the ESP's flash. Pick one and burn it.

## Parts

| Part | Notes |
|---|---|
| ESP32-S3 DevKitC-1 (N16R8 tested) | 16 MB flash; **flash it through its UART USB-C port** (the native USB pins GPIO19/20 are used as address lines) |
| SST39SF040 in a DIP-32 | 5 V, 512 K x 8 |
| 5 V supply for the chip | see the power notes below |
| About 28 jumper wires | breadboard is fine |
| Optional: 0.96" SSD1306 I2C OLED | shows IP, burn progress, last card and its tone names (see below) |

## Wiring

Pin map (also editable at the top of `firmware/sst_programmer/sst_programmer.ino`). The board is drawn rotated,
USB end up, so the header order lines up with the chip.

```
 ESP32-S3 LEFT header    SST39SF040 (top view)    ESP32-S3 RIGHT header
 (rotated, USB end up)   .------\__/------.       (rotated, USB end up)
 GPIO19    A18     1  |o               | 32  VDD     +5V
 GPIO20    A16     2  |                | 31  WE#     GPIO14
 GPIO21    A15     3  |                | 30  A17     GPIO13
 GPIO47    A12     4  |                | 29  A14     GPIO12
 GPIO48    A7      5  |                | 28  A13     GPIO11
 GPIO45    A6      6  |                | 27  A8      GPIO10
 GPIO0     A5      7  |                | 26  A9      GPIO9
 GPIO38    A4      8  |                | 25  A11     GPIO46
 GPIO39    A3      9  |                | 24  OE#     +5V
 GPIO40    A2     10  |                | 23  A10     GPIO3
 GPIO41    A1     11  |                | 22  CE#     GND
 GPIO42    A0     12  |                | 21  DQ7     GPIO8
 GPIO2     DQ0    13  |                | 20  DQ6     GPIO18
 GPIO1     DQ1    14  |                | 19  DQ5     GPIO17
 GPIO7     DQ2    15  |                | 18  DQ4     GPIO16
 GND       VSS    16  |                | 17  DQ3     GPIO15
                       '----------------'
```

> **Warning:** **disconnect the chip from the programmer (unplug it from this wiring) before you put it in the card slot.** An ESP left wired to the address and data lines loads and clamps the synth's 5 V bus, adds noise, and can back-power the board. Simplest: **lift the ESP off the breadboard so it has no connection at all.** Burn with the chip out of the synth; play with it out of the programmer.

- Pin 1 of the chip is **A18** (not VPP as on 27C040 EPROMs).
- **CE# to GND and OE# to +5 V** (we never read).
- Join the chip's GND (pin 16) to the ESP's GND. **Pin 16 must be connected**: unpowered chips are
  half-powered through their input diodes (we measured 2.5 V on VDD before finding it).
- Optional but good practice: 100 nF across pins 32 and 16.

### Power notes

- On many DevKitC-1 boards the **5V header pin is not connected to USB** until you bridge the small **IN-OUT**
  solder jumper next to it. Without that, the pin reads low. Either bridge it, or power the chip from a
  separate 5 V source with its ground joined to the ESP's.
- Never feed 5 V into that pin while USB is also connected and the jumper is bridged (two supplies on one rail).
- Program with the card **out** of any synth. The chip's VDD should read a solid 5.0 V (4.5 V is the very
  bottom of its range and cost us a few missed bits on one run).
- GPIO48 (or GPIO38 on some boards) drives the onboard RGB LED, which flickers white while the address bus
  moves. Harmless.

### Optional OLED display

A 0.96" SSD1306 (I2C, address 0x3C or 0x3D) shows the IP address, burn progress, the last burned card, and the
tone names attached to each stored card (long text scrolls). It needs the U8g2 library to compile.

| OLED | ESP32-S3 |
|---|---|
| VCC | 3V3 |
| GND | GND |
| SDA | GPIO4 |
| SCL | GPIO5 |

Optional push button from **GPIO6 to GND**: cycles through the stored cards on the display. If no OLED is
found, the firmware just runs without it.

## Build and flash

Arduino CLI with the `esp32:esp32` core (tested with 3.3.x):

```bash
arduino-cli compile --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB" firmware/sst_programmer
arduino-cli upload -p COMx --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB" firmware/sst_programmer
```

Use the board's **UART** USB-C port. `firmware/hello` is a bring-up sketch that prints chip info over USB.

## Use it

**Over USB** (no WiFi needed):

```bash
pip install pyserial
python tools/burn.py card.bin --burn        # send + erase + burn
python tools/burn.py card.bin               # just store it on the programmer
python tools/burn.py card.bin --burn --info "MY CARD: KICK, SNARE, HAT"   # with text for the web page
python tools/burn.py --list                 # stored images
python tools/burn.py --burn-stored card.bin # burn one already stored
python tools/burn.py --delete card.bin
python tools/burn.py --pin A5               # pin test: raise one line, meter the chip pin (--pin OFF to release)
```

**Over WiFi:** join the network `SST-PROG` (password `u110cards`; change it in the sketch), open
`http://192.168.4.1/`, upload a 512 KB `.bin`, press Burn. You can also enter your home WiFi on that page; the
ESP then joins your router as well (`http://sstprog.local/`). The page loses WiFi while a burn runs and
reconnects afterwards.

### USB protocol (921600 baud, line commands)

`PING`, `LIST`, `DEL name`, `PUT name size crchex` (then raw data in 4 KB blocks, each acked with `K`), `TXT name len` (then `len` raw bytes of info text), `BURN name`,
`STATUS`, `PIN A0..A18 | D0..D7 | WE | OFF`.

## Troubleshooting (what actually went wrong on the bench)

| Symptom | Cause |
|---|---|
| Chip unchanged or all `FF`, no error | The chip isn't seeing commands: check VDD (5 V), CE# (0 V), pin 16 (GND), WE# (3.3 V idle, drops with `--pin WE`) |
| VDD reads about 2.5 V | Chip powered only through its input diodes: the 5 V or GND wire is missing |
| 5V header pin reads low | DevKit IN-OUT jumper open |
| Upload fails with a Windows semaphore error | You used the native USB port; use the UART one |
| A handful of single missed bits | Marginal supply (4.5 V); use 5.0 V |

## Built one? Tell us

This is a prototype that has been tested on **one bench, one chip and one board type**. If you build any of it
(or try a different ESP32 board, another SST part, or a different card format), please
**[open an issue](https://github.com/yuyoi/esp32-maskrom-programmer/issues)** and tell us:

- what you built and which board and chip you used,
- what worked and what didn't (a photo of the wiring helps),
- the readback result from your chip programmer, if you have one.

Reports of failures are as useful as reports of successes.

## Related

- [U110 RomHex Studio](https://github.com/yuyoi/u110-romhex-studio): builds the U-110 card images this burns.

## License

MIT. See [LICENSE](LICENSE).

---

> "Barely affording I2C is like barely affording a warehouse of beer."
> — JSW
