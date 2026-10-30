# Version 0.28: event-driven input and less work per report

The user confirms v0.27 works, including reconnects and game transitions, but
reports input delays/stalls and game slowdown under heavy CPU load. No new
hardware performance trace is available. This release addresses concrete
scheduling and processing costs; its game performance still needs a PSP test.

## Scheduling

Previously the input worker ran at priority `0x24` (36) and slept for 5 ms
whenever its queue was empty. A report arriving just after the sleep had to
wait for that delay and then for higher-priority game threads. The worker also
looked up both Bluetooth providers separately for every packet it consumed.

The worker now runs at priority `0x10` (16; lower numbers run first) and waits
on a binary semaphore signalled by received input or connection/cleanup work.
The 5 ms input polling delay is removed. A semaphore retains a notification
that arrives just before the wait; it is a wake token, not a packet counter.
Repeated notifications may saturate that token without removing queued data.
Kernel signalling preserves the caller's K1. Allocation/start failures unwind
the semaphore and power handler. Shutdown wakes the worker and retains its
semaphore until its thread has actually joined.

Read callbacks still perform only bounded copies and chain Sony's original
callback. They signal after the outermost callback finishes, so higher-priority
decoding does not preempt Sony's read repost/ownership handoff. Pending native
cleanup and setup notifications also wake the worker. No parsing, file I/O or
controller writes were moved into the callbacks or controller sampler.

Each pass consumes at most eight queued packets, in order, after verifying
the current provider pair. This amortizes provider lookups/maintenance over a
burst. After eight packets the worker sleeps for 1 ms even if more data is
queued, giving games and native service threads CPU time under a sustained
flood. Normal traffic blocks on the semaphore as soon as its queue drains.
No report coalescing or deliberate input-rate cap was added; short presses,
releases and report fragments retain their ordering.

The idle wait has a 50 ms timeout, reducing periodic idle wakeups from 200 to
20 per second. Existing link checks remain at 200 ms, passthrough restoration
at 100 ms, and input timeout at 250 ms. Transport/manager replacement detection
still precedes each batch; missing providers retain the existing rebind path.
The plugin does not change the PSP CPU clock or native Bluetooth thread priorities.

## Decoder

CRC32 uses two lookups per byte into a 16-entry/64-byte constant table instead
of eight bit iterations. CRC validation is retained for every full report.
For a 74-byte report body plus the HIDP prefix, Unicorn counted **4,728 MIPS
instructions** for the old bitwise function and **1,352** for the new function
(both built with PSPSDK `-Os -G0 -fno-pic`). That is about 71% fewer executed
instructions in the CRC calculation. This is an instruction-count comparison,
not measured PSP CPU utilization, latency, or a game frame-rate improvement.

A complete 79-byte HIDP frame now decodes directly from its immutable queued
copy, avoiding a second 78-byte copy into the fragment assembly buffer. Actual
fragments keep the previous assembly and CRC rules. Corruption, stale data,
wrong generations, disconnect and suspend continue to reject/release input.

## Validation and use

`make test`, `make test-mips`, and `make test-bridge-mips` cover host protocol
checks, compiled MIPS input/sink behavior, guarded native setup, reconnect,
manager events, transport replacement and shutdown. The scheduling suite adds
actual compiled burst decoding, press/release ordering, a split report across
the eight-packet boundary, notification-before-wait, nested callback wake
ordering, semaphore saturation, cleanup/shutdown wakes, startup failures,
semaphore lifetime, and CRC comparison against zlib with an instruction budget.
Kernel scheduling and game CPU load are mocked; hardware validation remains necessary.

Install `pspgobt-release-v0.28.zip`, replacing the existing PRX, and restart the
PSP. Keep the working pairing, `controllers.cfg`, and `always` plugin entry.
Test rapid presses/releases and analog movement during demanding gameplay,
then launch/exit a game and reconnect without re-pairing. Comparing v0.27 and
v0.28 in the same scene at the same CPU clock separates scheduling changes
from game/clock differences. Logging and dumps are disabled in the release.
The separate session-trace profile remains available for connection diagnostics;
it is not a CPU profiler and should not be used for the performance comparison.
