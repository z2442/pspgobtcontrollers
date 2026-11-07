# Nintendo Switch Pro Controller support

Source builds support the original Nintendo Switch Pro Controller over
Bluetooth. This is experimental until tested on a PSP Go. The published v1.0
release predates this support. Joy-Con, Switch 2 Pro, third-party controllers,
USB input, motion, rumble, battery reporting, and Capture are outside this
implementation.

Hold SYNC beside the USB-C port until the player lights cycle, then register
the controller in the PSP's Bluetooth Device Settings. Its Bluetooth name is
`Pro Controller`. Keep the plugin enabled with the `always` entry described
in the README.

## Selection and transport

The existing native pairing and generic HID routes select the exact UTF-16
name `Pro Controller`. The input route also requires the existing PSP HID
capability field `0x0402`. When numeric identity is available it must match
Nintendo `057e:2009`. A classified paired address is saved as model 3 in the
existing version 2 `controllers.cfg`, allowing reconnects without another
metadata lookup. Existing address-only and Sony-model records remain readable.
Older builds cannot read a cache containing model 3 and must relearn their
supported controllers if downgraded.

The worker sends HIDP `A2` output report `01` on the interrupt channel, with
a four-bit packet sequence, neutral rumble bytes, and these subcommands:

1. `03 30`: enable periodic full input reports.
2. `10`: read 18 calibration bytes at `0x603d`.
3. `10`: read 22 user-calibration bytes at `0x8010`.
4. `30 01`: enable the steady player 1 light.

Commands are spaced at least 100 ms apart and each gets up to three attempts.
Only a matching positive reply advances a pending command. A full `30` input
also confirms report mode. Missing optional replies do not block input;
missing mode activation is handled by the existing silent-stream recovery.
No SPI writes or commands that change pairing are sent.

Input report `30` must be exactly 49 bytes after the HIDP header. Setup replies
`21` accept 49 or 50 bytes, validate ACK, command, and the requested SPI address
and length, and never publish controls or refresh input freshness. Basic `3f`
reports are ignored because they only arrive on changes and cannot sustain
the plugin's 250 ms input timeout. Switch reports have no application CRC;
the decoder is restricted to the selected connected model and is never used
to infer identity. Full reports fit in one native 64-byte read.

## Controls and calibration

| Switch Pro | Native DS3 control |
| --- | --- |
| B / A / X / Y | Cross / Circle / Triangle / Square |
| Minus / Plus / Home | Select / Start / PS |
| L / R | L1 / R1 |
| ZL / ZR | L2 / R2, pressure 0 or 255 |
| Left / right stick click | L3 / R3 |
| D-pad / sticks | D-pad / sticks |

Each packed 12-bit stick axis is scaled around its calibrated center, with
separate positive and negative travel, Y inversion, clamping, and a 5% center
dead zone. Valid user calibration overrides factory calibration per stick.
Invalid or absent values keep factory calibration or nominal center 2048 and
travel 1434. Calibration and setup reset with each read generation, including
reconnects and transport rebuilds. Once setup finishes, full input can use the
native sampler path; setup replies are left for the worker.

## Validation and references

`make test` covers each button, axes, calibration precedence, invalid replies,
bounded retries, sequence wrap, identity, routing and model-cache persistence.
`make test-bridge-mips` runs the compiled bridge through setup, calibration,
worker/sampler input, cached reconnects, short writes and lifecycle guards.
Real PSP pairing, the capability field on that hardware, and XMB/game/suspend
transitions still require hardware verification.

This independent implementation uses the wire formats documented by
[Nintendo Switch Reverse Engineering](https://github.com/dekuNukem/Nintendo_Switch_Reverse_Engineering/blob/master/bluetooth_hid_notes.md)
and its [SPI calibration notes](https://github.com/dekuNukem/Nintendo_Switch_Reverse_Engineering/blob/master/spi_flash_notes.md).
The report-mode, calibration, identity and axis conventions were cross-checked
against the [Linux Nintendo HID driver](https://github.com/torvalds/linux/blob/master/drivers/hid/hid-nintendo.c)
and [SDL Switch driver](https://github.com/libsdl-org/SDL/blob/main/src/joystick/hidapi/SDL_hidapi_switch.c).
