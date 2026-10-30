# Version 0.22: complete the native manager's HID close lifecycle

**Superseded in part by [v0.23](session-v023.md).** State 2 also signals an
outgoing connection failure; v0.23 removes the automatic disconnect it triggered.
The state-3 poll requests mask 2: the stale-state reproduction below requires
bit 2 set and bit 8 clear. The original mock incorrectly always succeeded,
including when the requested bit was absent. See v0.23 for corrected validation.

The user reports v0.21 is better, but the controller can turn off while the PSP
continues to offer Disconnect. The XMB also identifies its type as PS3 controller.
No newer diagnostic log is available; the latest local log is still v0.16.

The classification discussion below is historical: [v0.24](generic-hid-v024.md)
corrects the type mapping (generic HID is 2, audio is 8) and attaches HID methods
while keeping generic HID type 2.

## Label and connection state

The captured XMB chooses the `msg_ps3_controller` resource for controller profile
type 1. Our guarded route promotes the selected DualSense from unknown type 2
to this native controller profile. The label therefore reflects the compatibility
route; it does not establish that DS3 Bluetooth pairing or connection mode was
used. The Connect wrapper continues to select generic HID mode 0 for DualSense.
Version 0.22 retains this label and the native DS3 input port.

The Connect/Disconnect menu tests the native `sceBSMan` export `CCED8471` and
offers Disconnect when its result is nonzero. That export calls the same native
connection-flags accessor the bridge uses. A stale Disconnect menu can therefore
reflect stale manager state, beyond the local read resources handled in v0.21.

## Captured manager behavior

At `sceBSMan` offset `0xA24`, the event reader passes a six-byte connection
indication to the dispatcher at `0x2044`. It contains a device slot, a state word,
and profile/mode flags. The dispatcher matches the slot and low profile byte
against its six active entries at manager state `+0xE4`, eight bytes per entry.

State 0 follows the connection-established path. State 2 signals a close request
through event-flag bit 4. State 3 calls the final close handler at `0x255C` when
the entry's event-flag bit 8 is set. When that handshake bit is clear, state 3
also signals bit 4 and leaves the native active entry and connected flag intact.

This latter path is reproduced by executing both captured XMB and game modules:
with a selected HID entry and no bit 8, a state-3 indication leaves the actual
`CCED8471` export returning `0x0202`. This is a concrete path that can account
for the user's menu observation. It does not establish which events or flag
values occurred on their PSP in the absence of a current trace.

The bridge bypasses Sony's HID service registration to decode DualSense reports
itself. It previously implemented its own report path without replacing these
service-side close steps. Captured HID-service control operation 6 invokes the
native `(device, 2, 0)` disconnect helper. The missing service lifecycle is the
working explanation behind this change.

## Bridge behavior

The new guarded event hook selects only a known DualSense HID session. Selection
uses prepared metadata, an owned bypassed context, or the learned address matching
the current paired slot. It performs no metadata commands or file I/O on the
event reader. Other controllers, other profiles, unmatched entries and other
states retain Sony's dispatcher.

For state 2, Sony's dispatcher still runs first. If a close is not already in
progress, the bridge invalidates input and queues the native service's disconnect
operation onto its worker. The reader remains free to receive a subsequent
indication and wake a waiting command. An existing Settings disconnect is not
duplicated. A native request failure releases only its own intent. Request
success alone does not manufacture a terminal indication or clear manager state.

For a real state-3 indication matching the selected active HID entry, the bridge
calls Sony's original finalizer with that exact event and entry. Sony clears its
connected flag and active entry, sends the native device/profile notifications,
and dispatches the local HID cleanup through the v0.21 worker queue. The bridge
does not patch the XMB's status result or manually clear manager-table fields.

A terminal indication arriving before the queued worker request cancels the
redundant request. A new real connection also supersedes an old queued close:
an owned old read is retired locally before setup, preserving the new Bluetooth
link and allowing fresh controller initialization.

Instruction guards cover the event call, dispatcher branches, active-entry layout
and finalizer field updates for both captured relocations. The saved call is
restored with the other hooks during cooperative shutdown, only while it still
belongs to this instance.

## Validation and hardware check

Compiled MIPS checks execute the original stale-state branch, the new event hook,
Sony's finalizer, its actual connection-flags accessor and the actual export used
by the XMB. Terminal-event tests verify a zero native connection result, cleared
active entry, device and profile notifications, and deferred local cleanup in
both module layouts. Further checks cover peer-request handling, command failure,
duplicate suppression, a new connection superseding an old close, unrelated
sources, absent entries and changed-instruction rejection. Existing protocol,
input, reconnection, handoff and quiet-release checks pass.

These controlled checks cannot verify physical radio behavior. If the firmware
never delivers a closing indication, v0.22 does not infer one from input silence.
Hardware confirmation of the reported symptom and application transitions is
still required.

Install the v0.22 release at the existing path with the `always` entry. Keep the
pairing and `controllers.cfg`. Power off PSP and controller once after upgrading.
Test Settings Disconnect and controller power-off separately. After the light
goes off and the native close completes, the Triangle menu should offer Connect.
Reconnect without removing the pairing, then test XMB to game and back. Release
logging and dumps remain compiled out.
