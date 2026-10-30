# Version 0.15: reconnect and reboot input setup

The user confirmed that v0.13/v0.14 read controls, then reported that input
stops after controller reconnect or PSP reboot until deleting and pairing
again. They confirmed that Bluetooth still shows the controller as connected.
No new debug log accompanied this report; the existing local log is from
v0.12. The changes below address concrete lifecycle gaps found in the code.

## Gaps and changes

The prepared-device mask existed only in RAM and was populated by the XMB
Connect wrapper. Native incoming connections do not necessarily call that
wrapper, and reboot/resume clears the mask. A registered controller could
therefore reconnect without the preparation that skips remote DID discovery
and enables the DualSense receive/initialization path.

Two verified native HID-setup call sites now queue device slots onto the
input worker. The worker reads the existing registered record before calling
Sony's original setup routine, so the manager event thread remains available
to service the metadata query. Pairing records are neither removed nor
rewritten. Matching DualSense sources retain the existing DID/discovery
bypasses and 128-byte staging. Other HID devices still run the original
native setup on the worker. Setup is now asynchronous at these two internal
sites; the public XMB pairing/connection APIs retain their calls and results.

One original setup caller ignores its result. The other disconnects on setup
failure. A queued bit preserves that distinction, and the worker invokes the
same verified native disconnect helper with `(device, 2, 0)` after an actual
checked setup failure. Guard checks cover both JAL targets, delay slots,
surrounding failure branch and cleanup-helper entry before any patch is made.

Every 200 ms, the worker checks the native flags for the observed `0x0200` and
`0x0400` controller profiles. This also finds an existing connected controller
when the worker starts. Unserviced links are retried if their stored record
is not ready; a failed record query does not launch remote DID discovery.
Duplicate setup events reuse an existing context and completed initialization.
The bridge never submits a fresh read into a potentially freed native entry.
Read failures invalidate readiness; normal firmware teardown still owns the
buffers and context. Unrelated connected profile bits are not auto-started.

There was a second lifecycle gap in the sink. `connected()` could update a
source's generation before the packet worker saw the change, leaving the old
active-source flag intact. That skipped the first-report sink-start path.
The source is now released before accepting a new generation, and suspend
also clears the active source. Starting an already-ready sink now re-registers
its port-1 transfer handler and re-enables passthrough without registering the
power handler a second time. Each new session also gets fresh controller
initialization state. When a context survives suspend, its established wide-
read ownership is retained; later native setup refreshes the route if that
context is replaced.

## Validation

Host tests check every reconnect instruction signature and reject mutated or
truncated images. The guard matches the user's local 6.60 BSMan capture.
Compiled MIPS checks cover native incoming setup without the XMB, already-
connected boot startup, duplicate events, disconnect/reconnect with unchanged
stored pairing, resume, delayed metadata availability, unrelated profile
refusal, and checked/unchecked native failure cleanup. A separate generation
check verifies that the previous sink is released and initialization runs for
the new session. Sink integration checks verify repeat handler registration
and passthrough with only one power-handler registration.

These checks mock transport/controller APIs and normal firmware teardown.
They do not establish that every on-device reconnect or suspend sequence is
fixed. No old context buffer is forcibly reused to hide a failed native read.
Provider replacement during game/POPS transitions retains the existing
stop-until-restart behavior and remains outside this hardware verification.
Release/debug exclusion checks continue to verify that the release has no
logger, dump paths or diagnostic write imports.

## Hardware test

Install the release PRX over the existing one and restart, preserving both
the plugin entry and working pairing. Reconnect using the controller, test
controls, power the controller off/on, reboot the PSP, and test suspend/resume.
Keep the existing pairing throughout these checks. If input still fails,
the optional debug build enables the same existing logs/captures for locating
the remaining stage. Version 0.15 hardware reconnect behavior is unverified.
