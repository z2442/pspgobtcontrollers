# Input available at sampling time

The latest released plugin was reported to lag behind PSP buttons and native
DualShock 3 input in both the XMB and games. That report does not establish the
cause or the size of the delay.

[RemoteJoy Minus](https://github.com/SourceK78/remotejoy-minus/blob/main/main.c)
keeps the latest remote state and merges it into successful controller reads.
Its `inject_remote_values` and buffer hooks avoid waiting for a later native
sample to make that stored state visible. This is an architectural reference,
not evidence of zero end-to-end latency.

PSPGoBT previously required the input worker to consume a received report
before its native DS3 port handler could see it. The native sampler now drains
already received reports from an established stream before producing its
48-byte external-controller frame. A report can therefore reach the current
sample even when the worker has not run since its arrival. The existing DS3
mapping, PSP controls, timestamps, native system-button handling and latches
remain on Sony's controller path.

The sampler processes at most the queue's 16 entries, in order, including
fragments. Each full report retains its CRC check. It performs no Bluetooth
commands, identity lookups, registration, logging, allocation or waits. The
worker retains initial selection/setup, connection events and provider
replacement. A busy worker excludes sampler decoding, preventing concurrent
fragment assembly and lifecycle changes. Input from old generations, closing
sessions, suspend or shutdown cannot take this path. Reports older than
250 ms are discarded.

There are no added counters, measurement logs or diagnostic profiles. Build
and install the quiet PRX, retaining the working pairing, `controllers.cfg`
and `always` entry; restart the PSP after replacing the resident plugin.

```sh
make test
make test-mips
make PSPGOBT_DEBUG=0 PSPGOBT_SESSION_TRACE=0 test-bridge-mips
make PSPGOBT_DEBUG=0 PSPGOBT_SESSION_TRACE=0 package
```

Compiled MIPS tests cover delivery without a worker pass, an entire queued
burst, a fragmented press followed by release, corruption and timeout
rejection, and deferral during lifecycle/worker changes. A sink integration
test verifies that a queued release changes the very next native frame.
Physical PSP validation is still required; this change does not prove that
all reported lag is resolved.
