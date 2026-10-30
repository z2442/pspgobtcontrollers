# Version 0.27: discard stale completion flags before a new HID Connect

The user confirms that v0.26 now stays connected, but occasionally reports
`802F0140` on reconnect. Sometimes the connection succeeds after the error;
sometimes it remains disconnected. There is no new hardware trace yet.

## Reproduced native path

The captured `sceBSMan` Connect helper at `+0x2728` reserves a free active entry.
If the entry still has an event-flag UID, it reuses that UID. The helper polls
its flags at `+0x2880`, checks incoming-operation busy bit 8, sends command
`0x68`, and, for a pending response, waits for success/failure bits `1|4`.
A matching bit 4 produces `802F0140` and releases the pending entry.

A completed close can leave the reused flag object with bits `2|4` set. Bit 2
marks close completion; bit 4 can be left by the preceding close/failure signal.
The close finalizer clears the active entry but retains its event UID. The next
Connect can send a new radio request and immediately consume the old bit 4,
reporting an error before that new request finishes. The old success bit 1 can
similarly satisfy a new wait too early.

Tests reproduce both the retained flags through the actual native close
finalizer and the resulting false `802F0140` through the actual Connect helper.
This is a demonstrated cause consistent with the report, not proof that every
physical `802F0140` has this cause. A fresh radio failure uses the same error.

## Change

One additional guarded hook runs at the helper's initial poll, after the entry
is reserved and before command `0x68` is sent. A small kernel adapter passes
the native reserved-entry pointer and device slot into the bridge.

The bridge clears only old bits `1|4` when all of these conditions hold:

- The record is selected as a DualSense through the existing metadata/address
  routing guard.
- The pointer is one of the six native active entries, its slot and event UID
  match, and its profile flags are generic HID `0x0202`.
- Native close-completion bit 2 is present, busy bit 8 is absent, and the
  per-device manager mask does not mark HID as connected.

The poll/check/clear sequence runs in a short interrupt critical section. It
performs no metadata commands, file I/O, radio requests or waits. Bit 2 and
unrelated flags are preserved, and Sony's poll then returns the updated flags.
An error from the native poll or clear is preserved.

The later native completion wait is untouched. A fresh success must arrive for
the new pending request to succeed; a fresh failure still returns `802F0140`.
The bridge adds no automatic retry and does not turn a failed request into
success. Connected/incoming-busy sessions and other controllers keep their
native behavior. The v0.26 transport/handoff changes are included.

Guards verify the reserved-entry register convention, initial poll, busy check,
later completion wait, failure-bit test, error constant and native clear-event
import call. Shutdown and provider rebinding restore the new call site only
while its instruction still belongs to this PRX instance.

## Install and verify

Replace the existing PRX with the one in `pspgobt-release-v0.27.zip`, keeping
the working pairing, `controllers.cfg` and `always` entry. Restart the PSP to
replace the resident module. Test repeated Disconnect/Connect cycles, a
controller power-off/on reconnect, and an XMB/game transition. Re-pairing is
not needed for this update. Release logging and firmware dumps are disabled.

If the error persists, use `pspgobt-session-trace-v0.27.zip` for one failing
reconnect, then copy `ef0:/seplugins/pspgobt/session.log` back to the repository.
The bounded trace adds hexadecimal kind `1F` (CONNECT_FLAGS), with old flags
in A, flags after clearing in B, and the native result in C. A typical stale
failure reset is `00000006 -> 00000002`. BOOT's version is 27 decimal (`0x1B`).
Existing [trace limits](session-v023.md) and [handoff fields](transport-v026.md)
still apply. Restore the release after the capture.

## Validation

Compiled MIPS checks execute both captured manager relocations through Sony's
real Connect helper, the compiled adapter and bridge, with mocked radio/kernel
operations. They demonstrate the original false failure, wait for a fresh
success after clearing, preserve a fresh failure, keep incoming-busy errors,
leave unrelated controllers/profiles/connected entries alone, preserve other
flag bits, and refuse changed instruction layouts. The actual native close
finalizer is included in the close-to-reconnect test. Existing registration,
input, native sink, handoff/provider, shutdown and build-profile checks pass.
Physical reconnect confirmation remains necessary.
