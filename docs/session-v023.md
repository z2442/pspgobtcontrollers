# Version 0.23: remove the erroneous disconnect on a connection-failure indication

The latest hardware report is that Settings cannot fully disconnect the
controller; a controller-initiated reconnect can succeed without input. No
v0.22 hardware trace is available. The local `probe.log` is still v0.16.

## Concrete correction

Version 0.22 interpreted manager state 2 as a request for another HID disconnect.
It invalidated the input session and queued Sony's synchronous disconnect helper
on the input worker. The captured helper at `sceBSMan + 0x2B10` can wait for
event-flag bit 2 with a null timeout. While it waits, input, initialization,
local cleanup and recovery on that worker cannot run.

The captured outgoing Connect helper at `+0x2728` also waits for bits `1|4`.
State 2 signals bit 4, and the Connect helper treats that bit as failure and
cleans up its pending connection entry. It therefore cannot be treated
unconditionally as an instruction to send another Bluetooth disconnect.

Version 0.23 delegates state 2 to Sony without setting the bridge's closing
gate or queuing another disconnect. Settings still calls the original native
disconnect API and retains its result. A matching state-3 indication still
runs Sony's finalizer, and the bridge serializes local read cleanup before
initializing a new session. No connection status is inferred from input silence.

## Correction to the v0.22 test model

Sony's state-3 path polls event-flag **mask 2** in OR mode. The old test mock
returned success even when no requested bit was set. A real poll then fails,
and Sony already calls its finalizer. The stale-entry path reproduced by the
corrected test requires bit 2 set and bit 8 clear. Tests now exercise both
the matching and nonmatching poll results, and several handshake states.

The change removes a demonstrated blocking path and an incorrect closing gate.
It does not establish that every reported radio/session failure is resolved.
The remaining Settings behavior and XMB/game transitions require hardware
verification, and the optional trace below is provided for that purpose.

## Release installation and verification

Install `pspgobt-release-v0.23.zip` at the existing PRX path, keeping the
`always` entry, current pairing and `controllers.cfg`. Fully power off both
devices once to retire an old blocked worker. Do not remove/re-pair the device.

Test Settings Disconnect followed by Connect, then controller power-off/on,
then XMB to game and back while connected. After a disconnect, verify that
the Triangle menu offers Connect; after reconnect, verify buttons and sticks.
Also test starting the PSP with an already-powered controller. The release
contains no diagnostic logger or firmware dumps.

## Optional session trace, separate from the release

If a failure remains, replace the same PRX with the one in
`pspgobt-session-trace-v0.23.zip`. Keep only one plugin entry. Start from both
devices powered off, connect using the existing pairing, and reproduce the
Settings disconnect/reconnect failure once. Copy
`ef0:/seplugins/pspgobt/session.log` back to the repository before trying more
tests. For a separate application-transition test, archive/delete the trace
first, then enter and exit one game with the controller left on. Restore the
release PRX afterward.

This build uses `PSPGOBT_DEBUG=0 PSPGOBT_SESSION_TRACE=1`. It does not enable
the old `probe.log`, firmware inventories, binary dumps or pairing-command
dumping. It records numeric session phases, return codes, slot numbers and
read readiness; no controller addresses, link keys, input bytes or firmware
contents. Callback producers only copy into a 128-record RAM ring. A separate
thread writes the trace even if a Bluetooth call on the input/UI worker stalls.
Each PRX instance is limited to 1024 event attempts; overflow drops records
without blocking input. Sequence gaps expose dropped events. The file appends
across application transitions, replacing an old trace if necessary to keep
the file below 256 KiB. An I/O error disables tracing. It creates no directory:
the normal installed plugin directory must exist.

Each text line contains seven hexadecimal words: low 32 bits of the system
microsecond clock, per-instance sequence number, kind, device slot, A, B, C.
BOOT starts a new instance; clock wrap and resets must not be treated as a
continuous elapsed-time clock across boots.

| Kind | Name | A / B / C |
| --- | --- | --- |
| 01 | BOOT | version (23 decimal) / firmware / reserved |
| 02 | EVENT | native state / profile flags / closing masks |
| 03 | EVENT_DONE | state / finalizer result / native connection flags |
| 04 | CONNECT | requested mode / reserved / reserved |
| 05 | CONNECT_DONE | result / actual mode / selection |
| 06 | DISCONNECT | session token / reserved / reserved |
| 07 | DISCONNECT_DONE | result / token / reserved |
| 08–09 | CLEANUP / CLEANUP_DONE | 0 or result / epoch / closing mask |
| 0A–0B | SETUP / SETUP_DONE | flags or result / epoch / selection or prepared mask |
| 0C | INIT | source index / generation / attempt |
| 0D | INIT_DONE | write result (79 on success) / generation before / generation after |
| 0E | READ | event (0 data, 1 submitted, 2 error, 3 oversize) / size or error / generation |
| 0F | INPUT | generation / source / initialized; first decoded valid report |
| 10 | TICK | linked / disconnect-intent / closing masks |
| 11 | POWER | power event / generation / closing mask |
| 12 | STOP | written-record count / reserved / reserved |
| 13–14 | PREPARE / PREPARED | epoch or query result / prepared mask or selection / 0 or cache hit |
| 15 | LINK | native flags / generation / readiness bits |

LINK readiness bits: bit 0 read-ready, 1 wide read active, 2 discovery bypassed,
3 source accepted, 4 initialization completed. READ logs the first data callback
per generation and submit/error events, not the continuous stream. INPUT means
decoding succeeded before sink publication; it is not proof of game acceptance.

## Validation

Host protocol/patch checks and compiled MIPS bridge, native sink, reconnect,
manager, shutdown and build-profile suites cover the release and trace builds.
The manager checks execute captured Sony code in both XMB and game layouts,
with mocked radio/kernel operations. Trace-specific checks cover RAM-only
callback production, K1 preservation, formatting, overflow/sequence gaps,
per-instance and file bounds, I/O failure and bounded cooperative shutdown.
These checks cannot substitute for the PSP radio test above.
