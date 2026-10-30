# Version 0.25: HID controller mode in registration and saved-device loading

The user reports v0.24 displays an audio type and registration subsequently
reports that the device is unsupported. This release adds a separate internal
HID controller mode, numbered 9, covering registration, saved-device creation,
type display and native settings serialization. DualSense report decoding and
the native external-controller input bridge are retained.

## Why the previous route missed this

The registration profile constructor at XMB offset `0x150B0` receives the scan
object's type. A full-record scan object can return type 8. Version 0.24 selected
only types 1 and 2; a matching DualSense under type 8 could therefore continue
into Sony's constructor without HID methods. The registration worker's native
handler-count check at `0xDFF8` then takes the unsupported-device path.

Saved devices take a different route through `0x151A0`. That route reads a
stored type via `0x14D0C` and attaches its methods before querying metadata.
It does not call the registration constructor patched in v0.24. A stored audio
classification could consequently keep its audio methods and type label.
The previous tests covered the registration wrapper and individual methods,
but did not execute this complete saved-device path.

These are reproduced paths in the captured firmware. There is still no current
hardware trace establishing which one produced every reported failure.

## Custom mode

Recognition still requires the expected 80-byte metadata, the observed HID
capabilities `0x0402`, and the DualSense name boundary. Any existing native type
0–8 can be promoted to the plugin's mode 9 when these checks pass. A fresh
mode-9 object must also get its connection methods, so recognition is not just
a change of the displayed number.

During registration, the selected object receives Sony's ordinary HID method
kind 2 and returns mode 9 to its caller. Its nonempty handler vector passes
the original registration worker check. The unsupported-device branch remains
in place for objects without a usable handler. Native registration, metadata
and authentication errors are preserved; no failed Bluetooth operation is
converted into success.

For saved devices, a wrapper around the existing metadata call at `0x151DC`
classifies the returned record before the native loader continues. On a match,
it destroys any old method objects and their vector through Sony's original
`0x15018`, attaches exactly one HID handler, and records the slot and mode 9.
This also handles objects whose stored type was audio or PS3 controller.
Unrelated devices keep their original methods and labels.

If that metadata call fails, the wrapper passes no record to the decision
function. It may select only an address already learned in `controllers.cfg`
and matching the current paired-slot record. A stale prepared-device bit alone
is not sufficient. It preserves the metadata call's original result and does
not query the device again inside the decision syscall.

The type-label entry at `0x146C4` handles mode 9 explicitly through the HID
translation key `msg_bt_hid`; native types retain Sony's label dispatch. The
settings writer at `0x14D34` serializes mode 9 as native HID type 2 before
executing Sony's original writer. Plugin-specific enum values are not stored
in Sony settings. A later PRX instance recognizes the device again from its
metadata/address rather than depending on a stored custom enum.

All four hook locations, the saved-object register convention, the relevant
method factory, cleanup/vector-reset instructions and label key are checked
before installation. The four patches are prepared before any call site is
changed; an unsupported image is refused without a partial mode installation.

This is a generic HID connection mode with a DualSense report decoder. It does
not claim universal input mappings for arbitrary HID gamepads; those require
their own report decoding or a HID descriptor parser.

## Install and verify

Replace `ef0:/seplugins/pspgobt/pspgobt_probe.prx` with the PRX from
`pspgobt-release-v0.25.zip`. Keep one `always` entry and preserve
`controllers.cfg`. Fully power off the PSP and controller once after installing.

For a controller still registered, first open Manage Bluetooth Devices and
test its HID type, Connect/Disconnect and input. If the previous attempt left
it unregistered, scan and register with Create + PS, then verify input. Removing
and pairing again is not required merely to change an existing object's mode.
Finally test a controller reconnect and XMB/game transitions.

The release has logging and dumps compiled out. If the unsupported message
remains, use `pspgobt-session-trace-v0.25.zip` for one attempt and copy
`ef0:/seplugins/pspgobt/session.log` back to the repository. This bounded trace
includes patch installation results, registration results and mode decisions,
without addresses, report bytes or firmware dumps. Restore the release after
the capture. See [trace limits and format](session-v023.md).

Additional hexadecimal trace kinds in this release:

| Kind | Name | A / B / C |
| --- | --- | --- |
| 16 | MODE | prior type / selected / capabilities, or zero when using cache |
| 17 | REGISTER | route (0 full record, 1 address) / reserved / reserved |
| 18 | REGISTER_DONE | original result / route / reserved |
| 19 | INSTALL | HID-mode result / registration-hook result / API-hook result |

BOOT's version field is 25 decimal (`0x19`). A successful INSTALL has all three
results zero. An image mismatch is `FFFFFFFE` (-2).

## Validation

MIPS tests execute both captured XMB relocations through the actual saved-device
loader, native method destructor/vector cleanup and factory, label dispatcher,
registration constructor and handler-count rejection branch. They cover saved
audio/PS3/HID types, fresh objects, scan types 1/2/8/9, unchanged audio devices,
unrelated unsupported devices, and original metadata errors. Further tests
check kernel K1/metadata preservation, current-address cache selection, complete
installation and signature rejection, and serialization of all native types.

Host protocol/guard tests and the existing input, native sink, reconnect,
manager-session and shutdown suites also pass for the release and trace builds.
These checks validate the captured control paths with mocked kernel/radio calls;
the physical registration and reconnection results still need PSP confirmation.
