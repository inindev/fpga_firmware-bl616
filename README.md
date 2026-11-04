# TangCore firmware for BL616

Firmware for the on-board BL616 of Sipeed Tang boards. It shows the TangCore menu,
loads cores into the FPGA over JTAG, sends ROMs to the core and reads USB gamepads and
keyboards. See [doc/dev.md](https://github.com/nand2mario/tangcore/blob/main/doc/dev.md)
for how the firmware works with cores.

This is a fork of [nand2mario/firmware-bl616](https://github.com/nand2mario/firmware-bl616).
The `inindev` branch adds:

- Tang Nano 20K support: JTAG pins, no BL616 SD card (the TF slot is wired to the FPGA),
  2 Mbaud core UART (the BL616 has a 40 MHz crystal)
- the official Bouffalo SDK (CherryUSB 1.6) instead of nand2mario's patched SDK
- USB hubs with up to 7 ports; bigger hubs use their first 7 instead of crashing
  (needs the SDK patch in `patches/`)
- status messages on row 24 of the menu, above the logo and clear of TV overscan

Only the Tang Nano 20K has been tested on this branch.

## Boards

Choose the board with `TANG_BOARD` (default `console60k`):

| `TANG_BOARD` | Board | Cores in the menu |
|---|---|---|
| `nano20k` | Tang Nano 20K | NES, SNES |
| `console60k`, `console138k` | Tang Console 60K / 138K | NES, SNES, GBA, Genesis, SMS, PC/XT |
| `mega60k`, `mega138k` | Tang Mega 60K / 138K | NES, SNES, GBA, Genesis, SMS, PC/XT |
| `primer25k` | Tang Primer 25K | NES, SNES |

## Building

Linux or macOS. You need:

- **Toolchain:** T-Head GCC v2.6.1 (`xuantie-gcc-riscv-v2.6.1-linux-x86_64`, from
  [bouffalolab/toolchain](https://github.com/bouffalolab/toolchain)).
- **SDK:** the official [Bouffalo SDK](https://github.com/bouffalolab/bouffalo_sdk), tested at
  commit `237ed9378`, with the patch in [patches/](patches/README.md) applied (it fixes a
  crash with hubs of 8 or more ports). nand2mario's patched SDK no longer works with this
  branch. The SDK brings its own cmake in `tools/cmake/bin`.

```sh
git clone https://github.com/bouffalolab/bouffalo_sdk.git
git -C bouffalo_sdk checkout 237ed9378
git -C bouffalo_sdk apply $PWD/fpga_firmware-bl616/patches/cherryusb-hub-nports-clamp.patch

export BL_SDK_BASE=$PWD/bouffalo_sdk
export PATH=/path/to/xuantie-gcc-riscv-v2.6.1-linux-x86_64/bin:$BL_SDK_BASE/tools/cmake/bin:$PATH

make TANG_BOARD=nano20k
```

Output: `build/build_out/tangcore_bl616.bin`. SDK components are selected in `defconfig`.

## Installing

The BL616 runs Sipeed's FPGA Partner firmware at flash address 0x0, which keeps the
board's USB debugger working. TangCore runs beside it at 0x40000.

To flash the BL616, put it in update mode: unplug the board, hold the button (Nano 20K:
**UPDATE**, next to the HDMI connector; Console: **BOOT**, bottom left near a USB-C port),
plug it into the computer, release. It shows up as `Bouffalo CDC DEMO`.

**Tang Nano 20K:** write `build/build_out/tangcore_bl616.bin` to 0x40000 with Bouffalo's
flash tool (`BLFlashCommand` or the open-source `bflb-mcu-tool`). The Partner firmware
(`bl616_fpga_partner_20kNano.bin`) must already be at 0x0; boards that run
[FPGA-Companion](https://github.com/MiSTle-Dev/FPGA-Companion) on the BL616 have it. To go
back to FPGA-Companion, write its image to 0x40000 again.

**Tang Console 60K:** `make flash COMX=<port>` writes the Partner firmware and TangCore
(see `flash_prog_cfg.ini`). Get `bl616_fpga_partner_60kConsole.bin` from
[Sipeed](https://dl.sipeed.com/shareURL/TANG/Console/09_MCU_FW).

The firmware doesn't load a core at power-on: the menu is drawn by the monitor core, which
the FPGA loads from its own flash. Write it there with openFPGALoader, the board plugged into
the computer (the Partner firmware is then the USB debugger), e.g.
`openFPGALoader -b tangconsole -f monitor_console138k.bin` (Nano 20K: `-b tangnano20k`).

## USB drive

Cores and ROMs are read from a USB drive on the board's USB-C port. You need an OTG
adapter that also passes power. Use a hub to connect gamepads or a keyboard at the same
time. On the Nano 20K a USB microSD reader works; the board's own TF slot does not.

```
cores/<board>/monitor.bin    menu core, optional (only loaded from Cores)
cores/<board>/nestang.bin
cores/<board>/snestang.bin   ...
nes/                         .nes ROMs
snes/                        .sfc / .smc ROMs
```

`<board>` is the `TANG_BOARD` name, e.g. `cores/nano20k/`. Cores directly in `cores/`
are used when the board folder doesn't have them. The firmware picks the core from the ROM's
folder name.

On macOS, eject the drive before unplugging it, or Finder leaves `._` files next to the
ROMs, which show up in the menu as games. `dot_clean -m /Volumes/<drive>` removes them.

## Acknowledgements

- Firmware by [nand2mario](https://github.com/nand2mario)
- JTAG FPGA programming based on [openFPGALoader](https://github.com/trabucayre/openFPGALoader)
- Gamepad support based on Till Harbaum's [FPGA-Companion](https://github.com/harbaum/FPGA-Companion)
