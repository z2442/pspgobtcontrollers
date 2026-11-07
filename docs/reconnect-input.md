# Reconnects that establish Bluetooth but provide no controls

The reported symptom is reliable input immediately after pairing, followed by
later connections that appear connected but provide no controls.

The Pico side of RemoteJoy Minus uses
[joypad-os](https://github.com/SourceK78/remotejoy-minus/blob/main/.gitmodules).
Its [DualSense driver](https://github.com/joypad-ai/joypad-os/blob/main/src/bt/bthid/devices/vendors/sony/ds5_bt.c)
performs activation from a task after a 100 ms settling interval and advances
the sequence tag on outgoing reports. Its
[Bluetooth host](https://github.com/joypad-ai/joypad-os/blob/main/src/bt/btstack/btstack_host.c)
tracks HID readiness separately from the underlying connection and recovers
stalled setup. PSPGoBT keeps Sony's pairing, authentication and transport;
the changes here address controller selection and activation on that transport.

## Remembering what was paired

Previously `controllers.cfg` stored addresses alone. Each new connection
queried the local name/capability record again to determine whether to send a
DualSense or DS4 activation packet. A transient lookup failure could prevent
setup or clear the model required to activate an already selected stream.

The cache now stores the verified model with each address. A matching current
Sony pairing record, including its slot and HID capabilities, can restore the
correct model without another metadata query. A removed or changed pairing
cannot select the cached peer. The normal connected, generation and lifecycle
checks remain required before accepting reports.

The configuration remains 32 bytes with two address slots and a checksum.
Version 2 stores the two model bytes at offsets 6 and 7. Version 1 files are
accepted with unknown models; a successful local metadata lookup fills in the
model and schedules the upgraded file for persistence. No controller model is
guessed from a legacy address-only entry. Previous plugins ignore the newer
cache format and must learn their address cache again.

## Activating and recovering input

A completed 79-byte write proves transport completion, not that the controller
has begun sending usable input. Previously that completion suppressed further
activation attempts. Now a stream with no valid reports may retry activation
after 100 ms, up to three writes per read generation. Failed writes retain a
200 ms retry delay. DualSense retries use successive sequence tags and valid
CRCs. Received controls are decoded and published before a worker activation
write can wait; valid input stops activation retries and clears recovery delay.

If input remains silent for two seconds, the worker retains Sony's guarded
local read cleanup/rebuild. After three fast recovery attempts, retries slow
to at most one every ten seconds. They no longer stop permanently while the
Bluetooth link remains connected. Valid input resets this recovery budget.
The plugin sends no additional radio disconnect or pairing command.

## Installation and checks

Replace the PRX and restart the PSP. Keep the existing pairing,
`controllers.cfg` and `always` plugin entry. The old configuration is learned
and upgraded automatically when its controller model is available. After a
working connection, allow two seconds before launching a game so the worker
can persist the model. No new logging, diagnostic counters or measurement
profile is added; use a quiet build.

Host tests cover both cache versions, model persistence, eviction, invalid
models and checksums. Compiled PSP tests cover a new plugin instance restoring
each Sony controller model with all metadata queries unavailable, subsequent
reconnects, activation retries and sequence tags, suppression of retries once
valid input arrives, and recovery after the initial reset budget is exhausted.
These reproduce concrete software failure paths; confirming the reported
intermittent symptom is resolved still requires a PSP Go test.
