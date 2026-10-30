# Version 0.26: watch the Bluetooth transport separately

The user confirms pairing works in v0.25. Launching a game still stops input
until the controller is power-cycled, and reconnection is intermittent. The
DualSense remains lit as if connected. This narrows the hardware symptom but
does not identify which manager/read state the next application inherits.
The latest local `probe.log` is still v0.16; there is no handoff trace yet.

## Concrete lifecycle gap

The input worker previously watched only `sceBSMan`'s module UID. Its CDC
submission function, output writer, pending-read table and saved transport UID
could remain tied to the old `sceUsbBSMCDC_Driver` if that driver was reloaded
or relocated while the manager stayed loaded. The same manager's hook sites
also still contained our replacements, so simply calling the installer again
would fail its original-instruction guards.

The worker now checks the manager UID/base and CDC UID/read-state base before
using them. When either changes, it waits for executing bridge callbacks, then
restores only its own patches in a still-live manager. It clears stale queued
input and selection/initialization state and rebinds through the normal guards.
The existing worker is reused. A newly started worker waits while initial
binding publishes its provider IDs and contexts. Native import targets must match the replacement
driver before installation can succeed; a partially relinked image is retried.

For a transport-only replacement, a surviving manager still owns its context
and may have submitted another read already. The bridge keeps its staging-buffer
ownership bookkeeping and queues a local context rebuild after selection. An
apparently active native read is not sufficient to skip that rebuild. Sony's
local cancellation/queue cleanup must complete before fresh wide reads and
DualSense initialization. If the manager itself disappears or changes during
the transport gap, its old context bookkeeping is discarded instead.

Other plugins' modified instructions are preserved, and an image that no
longer passes the guards remains uninstalled. This change does not forcibly
take over an unrecognized callback, clear native connection flags by hand,
or send a speculative radio disconnect. The pairing and mode-9 routes from
v0.25 are retained.

This fixes a reproduced independent-provider replacement case. It does not
establish that the physical XMB/game failure uses that exact path. A new PRX
instance finding a stale callback, a manager with no native HID connection,
or an unsuccessful radio reconnect may need different handling.

## Release installation

Replace the PRX at `ef0:/seplugins/pspgobt/pspgobt_probe.prx` with the one in
`pspgobt-release-v0.26.zip`. Keep the working pairing, `controllers.cfg` and one
`always` entry. Fully restart the PSP after replacing the old resident PRX.
Connect in XMB, verify input, then launch a game with the controller left on.
Test returning to XMB as well. The release keeps both diagnostic flags off.

## Capture a remaining handoff failure

If a controller power cycle is still needed, use the optional
`pspgobt-session-trace-v0.26.zip` at the same PRX path:

1. Archive/delete the old `ef0:/seplugins/pspgobt/session.log`, then fully restart
   the PSP. Keep the existing pairing and configuration.
2. Connect in XMB and verify input. Launch one game while leaving the controller
   powered on. Wait at least five seconds after input stops.
3. Power-cycle the controller once if needed to record whether that recovers
   input, then exit the game. Copy `session.log` back to the repository.
4. Restore the quiet release afterward.

The trace is bounded and appends across PRX instances, so it can contain XMB,
game and return-to-XMB BOOT records. No firmware dumps, addresses, input bytes
or link keys are recorded. See [trace limits/format](session-v023.md) and
[v0.25 trace kinds](hid-mode-v025.md).

Additional hexadecimal kinds in v0.26:

| Kind | Name | A / B / C |
| --- | --- | --- |
| 1A | BIND | result / manager UID / transport UID |
| 1B | PROVIDER | manager changed / transport changed / current manager UID or 0 |
| 1C | CONTEXT | channel plus (native mode << 8) / callback and match bits / generation |
| 1D | EXIT | see below |
| 1E | RESET_BLOCKED | reason / context detail / stopping or callback detail |

CONTEXT's B low byte: 0 no callback, 1 Sony, 2 bridge narrow read, 3 bridge wide
read, 4 unrecognized. B bit 8 means the callback context matches this native
source, bit 9 means its buffer matches the saved native buffer, and bit 10 means
its buffer matches the current wide staging buffer. No raw pointer is logged.

RESET_BLOCKED reason 1 means a local reset precondition failed; B is the channel
and C is the stopping flag. Reason 2 means another callback/context holds the
channel; B says whether the context matches and C whether the callback is Sony's.

EXIT A=0/1 records the reboot shutdown call's wait flag, with callback depth in B
and local-cleanup completion in C. EXIT A=2 records hook retirement: B is its
success flag; C bit 0 is that result and bits 1/2 are outstanding wide-read flags.
The latter record is emitted only when that state changes.

LINK snapshots now also include learned paired slots whose native connection
flags are zero. Together with CONTEXT, this distinguishes a lit controller
without a native manager connection from a native connection with a stopped or
unrecognized read. BOOT's version is 26 decimal (`0x1A`).

## Validation

Compiled MIPS tests reproduce CDC disappearance/reappearance with an unchanged
manager; a UID change without an absent poll; relocation with native-import
relinking; delayed rebind during an executing callback; retained DMA ownership
and forced local recovery; manager replacement during a transport gap; and
preservation/refusal of another plugin's changed patch; and initial-binding
publication before worker polling. Existing host protocol,
registration/mode, bridge, sink, reconnect, shutdown and profile checks pass.
The optional trace suite also passes. These execute controlled native/kernel
boundaries and do not reproduce the complete PSP radio transition.
