# First hardware capture: 6.60 PSP Go

The uploaded `probe.log` reports `firmware=06060010 model=4`: Sony 6.60 on
05g. The user identified the CFW as ARK-5; its exact release is not recorded.
Version 0.1's module capture ran on the console. Version 0.2's new device-info
trace has been compiled and checked against this image, but has not run there.

## Capture integrity

Ten supplied text files match their logged sizes. The two sysconf instances
have different UIDs but identical contents. The log also records a successful
capture of `sceVshNetconf_bt_Module-056FE56D.text.bin`, which was not included
in the supplied folder.

Three important providers appear in the loaded-module inventory without
being captured by version 0.1: `sceBSMan`, `sceUsbBSMCDC_Driver`, and
`sceController_Service`. The name filter missed them. Version 0.2 explicitly
includes them and tests their inclusion. It excludes the probe itself and
unrelated `sceAmctrl_driver` crypto code.

The log records diagnostic mode with the discovery filter unchanged. There
is no native-input-test startup result in this log. It does not establish
whether a controller injection test succeeded.

## XMB discovery filter

Captured Bluetooth module base: `0x0A571F00`, text size `0x20C68`.
SHA-256 of this relocated capture:
`34bed0fa82cab4aa241f2dd1ef03a5d1a53a129bc3e6c57e00b929102d690717`.

The published `btfree` sites at text offsets `0x13D40` and `0x13D6C` contain
the expected original instructions. This establishes that the optional
filter patch matches this image; it does not establish a pairing fix.
The function at offset `0x13D30` filters/compacts 84-byte scan records.

## Separate unsupported-device path

At text offset `0x150B0`, the XMB reads an 80-byte device record through the
helper at `0x153FC`. That helper calls the imported `sceBSMan` NID
`CBD232D1` with a device identifier and output record pointer. The record
constructor at `0x153F0` sets its size field to `0x50`.

After a successful query, the relevant gate behaves as follows:

```c
/* Reconstructed control flow; field meanings are not fully identified. */
if (third_argument != 7 && record_u16_at_0x48 == 0)
    return 0x802F0135;
```

| Text offset | Observed operation |
| --- | --- |
| `0x150EC` | Load constant 7 |
| `0x150F0` | Skip the record-field check for third argument 7 |
| `0x150F8` | Read unsigned halfword at record offset `0x48` |
| `0x150FC` | Branch to return if the field is zero |
| `0x15100` | Complete error constant `0x802F0135` in the branch delay slot |

The XMB also recognizes `0x802F0135` in its result/UI handling. This is a
candidate for the user's unsupported-device message, not an observed trace
of that particular attempt. The value at `0x48` and the role of argument 7
must be checked on the console and against `sceBSMan` before bypassing it.

Version 0.2 wraps only this module's resolved `CBD232D1` import, after checking
ten caller/gate instructions. It calls Sony's original API and returns its
result unchanged. Up to 64 calls per boot log the result and successful
80-byte records' halfwords at offsets `0x06` and `0x48`. Those fields have
neutral labels until their semantics are established. It does not log peer
names, MAC addresses, link keys, or full runtime records. It does not bypass
the registration gate.

## Native controller boundary

The stock `scePadSvc` capture independently confirms the two NIDs used in
crozone's example: its startup calls `E467BEC8` with port 1 and a handler
pointer, followed by `6C86AF22` with mask 1.

Its copy callback at text offset 0 leaves the destination timestamp alone,
writes buttons at `+4`, the two sticks at `+8..+11`, and the final extended
word at `+44`. This supports the existing 48-byte frame layout. The stock
handler table at text offset `0x1D50` has size **12**, followed by two
callbacks. The current adapter follows the demo's 8-byte form with one
callback; compatibility of that shorter form still needs validation against
the controller-driver capture. The meaning of the stock second callback is
not assumed.

## Next hardware run

1. Keep this capture on the development computer. Replace the installed PRX
   with version 0.2 and rename the old `probe.log` to `probe-v01.log` on the
   PSP so the next log describes one boot.
2. Leave `enable-filter`, `input-test`, and `enable-input` absent for this
   baseline run. Restart and reproduce the registration rejection once.
3. Copy the new `probe.log` and all files it lists, especially
   `sceBSMan-*.text.bin`, `sceUsbBSMCDC_Driver-*.text.bin`,
   `sceController_Service-*.text.bin`, and `sceVshNetconf_bt_Module-*.text.bin`.
4. Check for `device-info trace setup result=0` and `device-info event=` lines.
   A negative setup code means the original query path was left unchanged;
   the new provider captures are still useful.

The next analysis needs to identify the record's flags/profile registration
and the PSP Bluetooth report transport. Pairing and live DualSense input
remain unimplemented.
