# Version 0.4 connection trace and version 0.5 registration experiment

The new log reports probe 0.4, Sony firmware `0x06060010`, model `4`, and
successful installation of every trace and the XMB HID routing patch. The
relevant runtime sequence is:

```text
bt-api end op=register-address event=1 result=00000001
device-info event=16 device=00000001 result=00000000 record80=1 field48=0402
xmb-hid-route hit=1 type=2->1 field48=0402; continuing native profile constructor
bt-connect event=1 device=1 mode=0
bt-api end op=hid-connect event=1 result=802F0140
```

This confirms that version 0.4 fixed the observed empty-handler classification
and reached the native HID connection boundary. The connection API returned
an error even though the user saw no numeric error on screen. The DualSense
continued flashing. Authentication, an active link, and report reception are
not established by this capture.

## What the connection error establishes

In the supplied `sceBSMan` executable, `F188A405` at text offset `0xA730`
selects profile flags `0x0202` for mode `0` and `0x0402` for a nonzero mode,
then calls the common connection helper at offset `0x2728`. That helper sends
command `0x68` and can wait for asynchronous completion. It constructs
`0x802F0140` at offsets `0x2A24`–`0x2A2C` when completion flag `0x4` is set.
The error itself does not identify a PIN, link-key, SSP, SDP, or protocol
failure, and is not a directly observed BSM error-table status.

The two mode values also affect later HID setup. Their full semantics are
not established, so version 0.5 preserves the original connection mode while
testing the earlier registration procedure. It does not claim that a normal
DualSense connection is already implemented.

## Registration is distinct from the input driver

Sony's [PSP manual](https://manuals.playstation.net/document/en/psp/current/settings/bl_manage.html)
documents a dedicated PS3-controller registration process involving a PS3 and
two USB cables, separate from normal Bluetooth-device scanning.
[BlueZ's Sixaxis plugin](https://github.com/bluez/bluez/blob/master/plugins/sixaxis.c)
contains the corresponding USB master-address setup. Sony's
[DualSense instructions](https://www.playstation.com/en-us/support/hardware/pair-dualsense-controller-bluetooth/)
describe discoverable Bluetooth pairing using Create + PS.

These references support the user's distinction between controller generations.
They do not prove that the particular PSP API used here programs a DS3 or
that `0x802F0140` specifically means a pairing mismatch. The captured PSP
code supplies a narrower, testable observation: the fallback scan object
uses address-only registration, whereas another existing scan class invokes
a full-record registration API with a separate parameter buffer.

The intended data path is:

```text
DualSense Bluetooth pairing/authentication and HID connection
    -> DualSense reports and initialization
    -> validated DualSense decoding
    -> PSP native DS3 external-input driver
```

The DS3 input driver can consume the adapter's normalized input without using
the DS3's USB-assisted pairing procedure. Decoding reports and driving the
native input sink are implemented separately; a receive transport connecting
them remains unfinished.

## Version 0.5 change

The fallback scan object contains an 84-byte scan record at object offset `4`
and supplies its address bytes from object offset `79`. Its registration
method at XMB text offset `0x13934` calls the address helper at `0x138E8`.
The full-record class's method at `0x13B28` instead passes its scan record
and parameter storage to the wrapper at `0x138C8`, which invokes NID
`8AC2F7B8`. Its provider helper sends commands `0x62` and `0x65`, with
completion handling. These are vendor commands, not identified standard HCI
opcodes.

Version 0.5 changes only the fallback method's call target at `0x1393C`.
Its existing delay slot supplies `object + 79`. The syscall wrapper validates
the 88-byte object bounds, its argument relationship, an 84-byte record
header, and a UTF-16 `DualSense` name followed by a space or terminator.
Matching records invoke the existing full-record API with an empty parameter.
The name is a route selector, not authentication.

The firmware copies 32 parameter bytes and appends a UTF-16 terminator to
form its 34-byte command payload. The plugin allocates a zeroed, terminated
34-byte buffer in user RAM and retains it until the synchronous registration
API returns, allowing the firmware's original K1 pointer checks to run.
The original scan record is unchanged. Nonmatching records retain address
registration. A failed full-record procedure returns its actual error;
there is no address-only success fallback.

The version 0.4 profile-routing patch remains active. A completed registration
can therefore proceed to the same native HID connection boundary, with its
mode and actual result logged. Input-port registration remains opt-in.

## Validation and hardware test

Both routing signatures match the supplied 6.60 XMB executable using its
original base `0x0A572000`. The latest log's XMB base is `0x0A571F00`; its
new binary was not supplied. Live guards check the actual relocated call
before patching. Unknown instructions or provider prologues are refused.
6.61 remains conditional on an identical instruction signature and untested
on hardware.

Host tests reject truncated/malformed scan records, Unicode/name mismatches,
registered-device records passed to the scan selector, changed instructions,
and wrong relocation bases. Eight MIPS emulation tests exercise the HID
wrapper and patched registration method, including original delay slots,
stack/return preservation, unchanged records, and negative return propagation.
The syscall implementations and Sony's Bluetooth hardware are not emulated.
The PRX compiles with warnings treated as errors.

For a fresh hardware test:

1. Replace the PRX, archive the old log as `probe-v04.log`, and restart with
   the three optional markers absent.
2. Delete only the controller entry created in the previous test. Reusing it
   would bypass the new scan-registration method.
3. Put the DualSense into pairing mode and register it through a fresh scan.
   If registered, try Connect from its Triangle menu when available.
4. Return the new `probe.log` and report registration, connection, and input
   separately. Full firmware dumps are unnecessary for this comparison.

Expect `bt-pair-route setup result=0`, `bt-pair-route target=DualSense`, and
a `register-scan` result. That result will show whether the full-record
procedure progresses. A subsequent connection error would require further
connection/authentication investigation rather than treating a saved entry
as working pairing.
