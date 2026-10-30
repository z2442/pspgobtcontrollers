# Version 0.10: no pending receive request

The new single-boot log identifies probe 0.10, firmware `06060010`, model 4.
The registration and connection calls succeed. Its last bridge observations
are:

```text
bt-input identity device=1 info=00000000 identity=802F0153 vendor=0000 product=0000
bt-input identity selection=2 device=1 (1=numeric 2=paired-name after 802F0153)
bt-input pending-read device=1 source=0 channel=2 native-mode=1 pending=0 entry=1 context-match=0 buffer-match=0 adopted=0
bt-input init device=1 source=0 channel=2 bytes=79 result=0000004F generation=0 attempt=1
```

There are no read-queued, unhandled-report, forwarded-report, or sink-start
lines. No callback was present to adopt at the time of inspection. The context
contained a nonzero entry pointer, but this does not establish that the entry
still belongs to a live request. A successful output write does not establish
that the controller processed the packet.

## Native setup before the first read

In the locally captured BSMan routine at text offset `0xA128`, the order is:

1. Allocate a device context and receive-buffer entry.
2. Call the internal DID helper at offset `0x58D8`, from call site `0xA1E4`.
3. Register the HID descriptor/callback interface with `sceHIDSvc_driver`
   `7C5E76AB`, from call site `0xA1FC` through import stub `0xCBF8`.
4. Notify the manager and submit the first CDC read at `0xA22C`.

The captured HID-service implementation submits a synchronous request to its
worker and waits for completion. That worker invokes the descriptor interface
to obtain device identity and a report descriptor, then initializes native
handlers. This is an additional discovery step before the receive loop and
is unnecessary for our own DualSense decoder.

The log is consistent with setup waiting before the first submission, but
does not identify the exact call that has not completed. A stale entry or
other earlier failure remains possible. Version 0.11 therefore preserves and
traces the DID helper's actual call and result as well as changing the
subsequent HID-service registration route.

## Version 0.11 change

Before the XMB's original connection call, the bridge queries the selected
slot's 80-byte record on that calling thread. A matching DualSense name and
`0x0402` capabilities select the discovery bypass for that slot. A failed or
different record clears previous selection. Suspend/resume also clears it;
a power transition during the query prevents selection from being restored.
No synchronous metadata query is added to the native manager event thread.

At the guarded service-registration call, the bypass requires the exact
native descriptor pointer, a valid slot matching that context, cached
DualSense metadata, a running/unsuspended bridge, and live connection flag
bit 1. For that route only, service registration is omitted and setup
continues into Sony's existing read submission. Normal devices call the
original service unchanged. The native DID helper, pairing, connection,
buffer allocation, receive callback/queues, and teardown remain in place.

Metadata is a routing guard, not authentication. Input publication still
requires the existing independent connected-source identity policy: matching
numeric Sony IDs on a successful DID query, or the narrow `0x802F0153` fallback
with matching metadata. The native DS3 external controller sink starts only
after a valid decoded input report. The bypass does not spoof the DID query
or force a Bluetooth pairing/connection result.

New bounded begin/end traces identify entry and return from native DID and
HID-service calls. First-read failures now retain their original signed
result in the worker queue and log it. No data parsing or file I/O is added
to the receive completion callback.

## Validation and hardware test

Host protocol/guard checks pass, including mutation and bounds tests for both
new call sites. All 30 compiled PSP bridge checks pass in MIPS emulation,
along with eight XMB wrapper checks. The bridge checks include discovery
refusal cases, arguments, K1, original errors, metadata replacement/query
failure, and power-generation changes. Two local integration checks execute
the captured Sony setup routine with the compiled hooks: selected DualSense
setup reaches a 64-byte receive request with the correct channel, buffer,
callback, and fifth argument; an unselected device retains the original
service error and never submits that read. Transport/service operations are
mocked, so these checks do not prove an on-device input fix. Firmware captures
remain local and are excluded from releases.

Keep the existing pairing. Replace the PRX, archive the old log as
`probe-v010.log`, and restart with optional markers absent. Open Manage
Bluetooth Devices and connect through the XMB to populate the selection
before setup. Try controls for around 15 seconds and return the full log.
Check `prepare`, native DID begin/end, HID discovery `bypass=1`, `read queued`,
initialization, and forwarded/unhandled report lines in order. A begin without
an end is useful even if no input appears. Version 0.11 hardware input remains
unverified.
