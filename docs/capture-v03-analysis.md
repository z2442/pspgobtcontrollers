# Version 0.3 hardware trace and version 0.4 routing patch

The updated log reports Sony firmware `0x06060010` and PSP model `4`, under
the user's ARK-5 setup. All five XMB Bluetooth API traces installed. The
important sequence is:

```text
bt-api begin op=register-address event=1
bt-api end op=register-address event=1 result=00000001
device-info event=25 device=00000001 result=00000000 record80=1 field48=0402
```

The address-registration API returned device slot `1`. The subsequent query
succeeded and supplied nonzero capabilities. No traced HID connection,
scan-registration, or identity query followed in this capture. This does not
establish an authenticated, persistent pairing or working input. In
particular, the BSM status `0x45` discussed in the previous analysis was not
observed here.

## Local unsupported-device path

The following analysis uses the previously supplied version 0.2 executable
`bluetooth_plugin_module-04FE4C4D.text.bin`, with its own base `0x0A572000`.
The new log's module has UID `04FF4C4F` and base `0x0A571F00`; its relocated
binary was not supplied. Applying that newer base to the older binary would
produce misleading addresses. The patch uses offsets and checks the live
instructions, including the relocated call target, before writing.

The older executable shows a second route to the compatibility message,
separate from the zero-capabilities rejection:

1. The fallback scan object uses a type getter at text offset `0x13950`
   returning `2`, and an address-registration method. Its constructor installs
   this fallback object's vtable in the default scan classification branch.
2. After registration, the profile setup at offset `0x150B0` queries the
   80-byte record. It stores the scan object's type in the 44-byte profile at
   offset `+4`, then calls the profile constructor at text offset `0x14E0C`.
3. That constructor selects a DUN handler for type `3`, audio handlers when
   capabilities intersect `0x000D`, and a HID handler for type `1`.
   Type `2` with capabilities `0x0402` matches none of those choices and
   leaves the handler vector empty.
4. The registration worker at text offset `0xDF9C` checks the handler count
   at profile offset `+32`. A zero count causes cleanup and worker state `1`.
   The completion UI maps that state to
   `msg_error_register_device_notsupported` even when the registration API
   returned a valid slot.

This static path fits the successful slot/query sequence and the user's
reported compatibility error. The original runtime trace did not expose the
scan type or handler count directly; version 0.4 tests this diagnosis by
changing the profile selection at that boundary.

## Version 0.4 patch

The patched call is at text offset `0x1512C`, immediately before native
profile construction. A user-mode wrapper calls a kernel decision syscall,
restores the original arguments, stack, and return address, then jumps to
Sony's original constructor. Its original delay slot, `move a1,sp`, remains
intact. Sony's C++ constructor continues to execute in user mode.

The decision changes only the profile's type from `2` to `1` when all these
conditions hold:

- The 80-byte device record has its expected size header.
- Its capability field at offset `0x48` is exactly `0x0402`.
- Its UTF-16 name starts with `DualSense`, followed by a space or terminator.
- The existing profile type is `2`.

The name is a selection guard, not an authentication check. Device slot,
peer name, capabilities, API results, and the native constructor's result
are preserved. Other profiles continue through the original constructor.
No Bluetooth manager status is turned into success. The existing discovery
filter patch and emulated input sink remain separate, opt-in features.

Installation accepts firmware 6.60/6.61 only when the captured constructor
and call-site instruction signature matches. The signature was checked
against the supplied 6.60 executable; 6.61 hardware has not been tested.
Unknown or already modified call sites are refused. The allocated wrapper
and patch remain resident until restart.

## Validation and next capture

Host tests cover the bounded profile edit, unchanged unrelated records,
truncated buffers, Unicode/name/capability mismatches, instruction-signature
mutation, and relocated call targets. Four Unicorn MIPS execution tests run
the generated wrapper with the actual caller delay slot and a simulated
syscall/native constructor. They verify argument restoration, stack and
return-address preservation, the original constructor result, and the
precise profile edit. The guard also accepts the supplied 6.60 executable.
The PSP PRX compiles with warnings treated as errors.

On hardware, `xmb-hid-route setup result=0` confirms installation and
`xmb-hid-route hit=1` confirms that a matching record took the new route.
Bounded `xmb-hid-route skipped=` lines identify records that remained
unchanged. Reaching `bt-connect` and a `hid-connect` result would establish
progress into the existing HID connection stage. It would still leave
DualSense report reception, decoding, native input integration, disconnect,
and reconnect behavior to implement or validate.

Replace the PRX, archive the old log, leave all three optional marker files
absent, restart, and attempt one registration. If it stays registered, try
connecting and pressing buttons. Return the new `probe.log` and describe
registration, connection, and input separately; another full firmware dump
is unnecessary for this comparison.

## Hardware feedback after version 0.4

The user reports that the PSP now registers the controller as a DualShock 3,
but the DualSense continues flashing in pairing mode. No error code was
reported. This supports progress past the UI's unsupported-device rejection;
it does not yet establish authentication, an active HID link, or initialized
controller input. The subsequently supplied version 0.4 log confirms a route
hit and connection error `0x802F0140`; see the
[version 0.4 analysis](capture-v04-analysis.md).

The native HID handler's connect method is at text offset `0x15514` in the
older captured XMB image. It forwards the device identifier to the traced
`F188A405` connection API with mode `0`. Thus the existing version 0.4 hooks
already cover this next boundary; no additional patch is needed to observe
the attempt. Highlighting a registered device and pressing Triangle opens
the management menu described in Sony's
[Bluetooth manual](https://manuals.playstation.net/document/en/psp/current/settings/bl_manage.html).
If Connect is offered, attempt it while the controller is discoverable, then
return the log even if there is no visible error. The results should determine
whether to investigate connection initiation, its return error, or subsequent
controller initialization and report handling.
