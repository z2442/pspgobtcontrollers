# Version 0.21: serialize native Bluetooth session teardown

The user reports connected controllers with no input after reboot or an
application transition, and inability to disconnect/reconnect from Bluetooth
Settings. The latest available trace is still v0.16, so the physical state in
the latest releases is unknown. This change fixes a specific lifecycle gap
found in captured firmware; hardware confirmation is still required.

## Verified native path

In both captured XMB and game layouts of `sceBSMan`, the disconnect event
handler signals its waiting caller before running profile cleanup. It clears
the HID connection flag, then dispatches the device slot through a profile
cleanup table. The HID entry at module offset `0xDABC` points to the local HID
cleanup at `0xA2DC`. The indirect call at `0x2718` ignores the cleanup result.

That local helper unregisters its service descriptor, cancels the CDC read,
releases its queues, and clears the context's channel/device fields. Sony's
receive callback can attempt another read during cancellation. Until v0.21,
the bridge blocked reposts only for its own local resets and kernel shutdown,
not for normal native disconnect cleanup. Its worker could also be setting up
the same context while the manager cleaned it up.

Bluetooth Settings uses the one-device `sceBSMan` export `68327A27` to disconnect
HID. Previously only its Connect import was handled by the bridge.

## Ordered sessions

The guarded disconnect import records intent before calling Sony, neutralizes
the controller state and invalidates queued input. It preserves the original
call's result and caller K1. A failed request releases its own intent; a late
result cannot clear a newer session's closing state.

The guarded profile cleanup pointer now queues cleanup for selected controllers
on the same worker that owns HID setup, reset and initialization. The manager
thread remains free to service an already-running cached-record command. Its
verified caller ignores the helper result, so deferred work does not invent
a successful public Bluetooth disconnect. Other devices call the original
helper directly, preserving its result.

While closing, new setup, watchdog recovery, controller initialization output,
input publication and read reposts are blocked. Setup that was already running
can return, but an epoch change prevents it from publishing a ready session.
At the worker's next safe point, Sony's original cleanup cancels the old read
and releases the queues. Staging ownership is cleared only after successful
cleanup and a cleared native device field. Failure retains ownership and blocks
new setup, with at most three local cleanup attempts per native close event.
Cleanup waits through suspend; it can run as part of cooperative reboot exit.

A Connect request waits up to 250 ms for pending cleanup before entering Sony's
API. If cleanup is still blocked, it returns a failure rather than using the
DS3 connection mode against the closing context. Once cleanup finishes, normal
stored-device connection and the DualSense initialization report can run again.

The pointer and its dispatch instructions are checked against each module's
relocated addresses before installation. Shutdown restores the saved pointer
only if it still belongs to this instance. If shutdown has finished its provider
calls but an outstanding DMA kept hooks resident, a later native cleanup runs
directly with reposts still blocked by quiescing.

## Validation

Host protocol and signature checks and compiled MIPS suites pass. New checks
exercise the captured XMB and game manager table lookup/indirect call; intent
before native cleanup; rejected read repost; ordered teardown during in-flight
setup; failed and stale request results; already-disconnected success; bounded
cleanup failure with retained buffer ownership; subsequent fresh initialization;
unchanged unrelated-device results; K1 and native result preservation; and
guarded installation/restoration of the cleanup pointer.

These are controlled execution checks, not a full Bluetooth radio simulation.
They do not establish that the reboot or application-transition symptoms are
fully resolved on hardware. Release logging and firmware dumps remain compiled
out. No firmware captures or controller configuration are included in packages.

## Hardware check

Install the v0.21 release at the existing path, retaining the pairing,
`controllers.cfg` and `always` entry. Fully power off PSP and controller once
to retire any earlier stuck session. Connect normally, use Disconnect then
Connect in Bluetooth Settings, then launch and exit a game with the controller
left powered on. Also test a PSP reboot with the existing pairing. Removing
and registering the controller again should not be part of these checks.
