# Version 0.20: retire the outgoing kernel instance

The user reports that v0.19 still fails when swapping between games and XMB.
The DualSense acts connected, but input stops and another connection attempt
does not recover it. No newer trace is available. This does not prove which
hardware boundary is stuck.

Inspection found that the PRX exported start/stop but no kernel reboot callbacks.
Its ordinary stop function deliberately refuses unloading because code and
buffers may still be referenced. That does not implement loadexec retirement.
The outgoing instance retained live controller-handler pointers, patched input
call sites and CDC callback/buffer ownership without an explicit exit protocol.

The kernel callback mechanism and dispatch ordering are documented by
[uOFW's module manager](https://github.com/uofw/uofw/blob/master/src/kd/modulemgr/rebootModule.c).
It dispatches kernel reboot-before and reboot-phase entries separately from
ordinary module stop. Version 0.20 exports both standard NIDs (`2F064FA6` and
`ADF12745`) using their four-argument kernel ABIs.

## Cooperative retirement

The early callback stops processing new module starts, requests bridge shutdown,
neutralizes input and prevents metadata/setup work or further selected read
reposts. The worker owns provider cleanup; the reboot callback does not race it
with another native setup/cancel call and does not force-terminate a thread
inside firmware. It waits at most 250 ms for the worker to finish.

At a worker safe point, after any executing receive callback has returned, the
bridge uses the guarded local HID cancel/queue cleanup for owned contexts. This
does not call the Bluetooth disconnect or pairing routines. Sony's cancellation
callbacks retain their buffer cleanup, with read reposts refused during exit.
It then restores each saved call instruction only if the currently loaded
provider is the original UID/base and the instruction is still this instance's
replacement. A later plugin's modified site is preserved.

Hook retirement waits for executing callbacks and pending transfers that still
reference the bridge's callbacks or staging buffers. A failed or slow early
shutdown leaves those references in place for normal driver reboot teardown;
it does not pretend they have drained. The late callback can finish guarded RAM
retirement after cooperative provider cleanup, but never re-enters transport
providers that may already have run their reboot shutdown.

The sink is also retired. Captured registration/setter signatures locate the
driver state. Port 1 is detached only when both its handler and source pointers
still belong to this instance. The passthrough setter clears only port 1's bit,
preserving other bits. Replaced handlers or unknown accessors are not modified.
Future start/submission requests to the outgoing instance are rejected.

## Validation and limits

Compiled MIPS checks cover the actual standard export NIDs, before/phase dispatch,
cooperative context cancellation without a Bluetooth disconnect, own-hook
restoration, preservation of another plugin's call site, worker timeout,
executing callbacks, outstanding staging transfers and a single worker join.
Captured XMB and game controller tests also execute sink shutdown, verifying
detachment, preserved port-2 passthrough, foreign-handler preservation, refusal
of changed accessor signatures and rejected restart after shutdown. Existing
protocol, lifecycle, cache and quiet-release checks pass.

These tests do not reproduce the full physical firmware transition or prove
that v0.20 fixes the reported stuck connection. Shutdown ordering, transport
cancellation timing and radio behavior still require hardware confirmation.

Install the release, fully power off the PSP and controller once, then reconnect
the existing paired controller after XMB startup. Keep the learned configuration
and `always` entry. Test XMB to game and game to XMB without powering off the
controller between them. This build keeps logs and dumps compiled out and uses
the existing 32-byte controller configuration unchanged.
