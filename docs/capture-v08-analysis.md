# Connected DualSense: input bridge and identity-query blocker

The user reports the DualSense stops flashing, the PSP shows Connected, and
controls do not respond. The version 0.6 and 0.7 logs confirm:

```text
bt-api end op=register-scan event=1 result=00000001
bt-api end op=hid-connect event=1 result=00000000
```

Version 0.7 installs its receive hooks, but no identity, report, or native-sink
startup lines follow. Version 0.8 adds connection-triggered initialization.
Its supplied log reaches a more specific boundary:

```text
bt-input connected device=1 flags=0202; checking HID report setup
bt-input identity device=1 info=00000000 identity=802F0153 vendor=0000 product=0000
```

The device-record query succeeds and the current connection flags include
HID. The numeric vendor/product query fails; version 0.8 consequently refuses
source selection before requesting initialization or forwarding input. This
is a plugin policy blocker in the observed run. It does not establish that
report initialization or the remaining transport will work once selected.
The precise firmware meaning of `0x802F0153` is not assigned here.

## Vita comparison and PSP implementation

At the recorded reference revision,
[ds34vita](https://github.com/MERLev/ds34vita/blob/a82f79f6f2bdd43bcf3ce01a8f431d45eb5b4b30/main.c)
identifies the controller at connection, sends an initialization/output
report `0x31`, and schedules/requeues HID reads. Its `ksceBt*` APIs cannot be
called on PSP. These are connection-flow and protocol references, not assumed
PSP interfaces.

The PSP implementation checks captured instructions before using these
native boundaries:

| Manager text offset | Use |
| --- | --- |
| `0x07E0` | Read the current device-slot connection flags |
| `0xA128` | Native HID context/read setup for a device slot |
| `0xA22C` | Initial asynchronous CDC read submission |
| `0x9F40` | Requeued asynchronous CDC read submission |
| `0x9EE0` | Original receive callback, preserved |

The two native contexts are at text base plus `0xDE78` and `0xDEA8` in the
captured image. The bridge checks their construction instructions too. CDC
export `A5E108BB` supplies asynchronous reads with five arguments, including
callback and context. Only calls with the original callback, a known context,
and the native 64-byte request size receive the bridge callback. Every other
call retains its original arguments.

The bridge copies completed buffers into a bounded queue and calls Sony's
original callback with unchanged arguments. It never owns or frees Sony's
read buffers. A worker decodes reports, performs file logging, and starts the
native DS3 port-1 adapter after accepted input. Queued packets expire after
250 ms; connection generations, receive errors, and power events invalidate
old state. A replaced/unloaded Bluetooth manager stops processing until a
restart. VSH/game/POPS transitions remain untested.

On connection, the worker checks identity without waiting for the first
input packet. If no matching native HID context exists after a short delay,
it requests the existing native setup for the still-connected selected slot.
It sends an aligned 79-byte HIDP output frame (`A2` plus the 78-byte report)
through CDC export `F8C6AFBD`, with a 200 ms timeout and at most three attempts
for negative write results. This interface's prologue, pointer alignment,
and argument layout were checked against the captured code. A successful
write result is not proof of delivery or controller acceptance.

The output format follows the existing protocol builder and
[Sony's Linux driver](https://github.com/torvalds/linux/blob/master/drivers/hid/hid-playstation.c):
report ID `0x31`, sequence/tag fields, and CRC covering the HIDP `A2` prefix.
No Vita or Linux driver implementation is incorporated.

## Version 0.9 source-selection change

A successful numeric identity query still requires Sony vendor `054C` and
DualSense product `0CE6` or Edge product `0DF2`, together with the expected
80-byte device record, name, and `0x0402` capabilities.

For the specifically observed query error `0x802F0153`, version 0.9 accepts
selection using the connected slot's record when the size header, capability
mask, and UTF-16 `DualSense` name match. The name must end or be followed by a
space. This fallback is logged as `selection=2`; numeric selection is `1`.
Unrelated errors, mismatched numeric IDs, names, or disconnected slots remain
rejected. The numeric query result is not rewritten and no vendor/product ID
is fabricated. Name matching is a route selector, not cryptographic device
authentication; the Bluetooth pairing/link is a separate prerequisite.

## Supported report framing

The portable decoder accepts exact 10-byte basic Bluetooth `0x01` reports,
using the layout documented by
[SDL's DualSense driver](https://github.com/libsdl-org/SDL/blob/main/src/joystick/hidapi/SDL_hidapi_ps5.c),
and full 78-byte `0x31` reports with CRC/hat validation. The PSP wrapper
handles HIDP `A1` input and explicit `B1` continuation. It also tests the exact
raw 64/15-byte USB read split of a full frame, accepting it only if the final
report CRC verifies. Hardware framing has not yet been captured at this
boundary. No speculative payload is fed into the input adapter without
passing its supported frame checks.

## Validation and next hardware run

Host tests cover metadata/numeric identity and the narrow fallback, basic
reports, fragmented full reports, corruption/truncation rejection, native
frame conversion, stale release, and instruction-guard mutations. The guards
match the supplied 6.60 manager executable at its original base `0x88238900`.
6.61 remains conditional on matching instructions and untested on hardware.

Eight existing MIPS emulation tests check the XMB routing wrappers. Eight
additional tests execute the compiled PSP bridge functions with mocked kernel
and CDC calls. They check all five read arguments, callback selection,
unchanged unrelated reads, error propagation, stack restoration, aligned
initialization output, independent CRC verification, and failed-write state.
They do not emulate Bluetooth hardware or prove that controls work.

Keep the working pairing, replace the PRX, archive the old log as
`probe-v08.log`, and restart with all three optional markers absent. Reconnect
the saved controller, then try D-pad, sticks, and face buttons in the XMB for
about 15 seconds. Return the new log. Expected diagnostic progression is:

```text
bt-input connected ...
bt-input identity selection=1 ...       (or selection=2)
bt-input native HID setup ...          (only if setup was needed)
bt-input init ...
bt-input native sink start ...
bt-input forwarded ...
```

An initialization error, absent receive completions, unhandled report framing,
and a native-sink registration error are distinct next boundaries; the log
should determine which remains.
