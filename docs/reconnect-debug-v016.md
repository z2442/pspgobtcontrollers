# Version 0.16: diagnose the stored-device reconnect failure

The user reports that v0.15 still works only immediately after pairing. The
stored connection shows Connected but provides no controls after reboot or
reconnect. The available local log is from v0.12, before successful input and
the subsequent lifecycle changes. Release builds intentionally produce no
new diagnostic trace.

Version 0.16 adds observations behind `PSPGOBT_DEBUG=1`; the selection,
transport and reconnect behavior remains that of v0.15. This is a diagnostic
build, not a confirmed reconnect fix.

## Capture the failing stored connection

1. Install the PRX from `pspgobt-debug-v0.16.zip` in the existing plugin path.
2. Archive/rename the previous `probe.log`. Keep the existing controller pairing
   and leave optional `enable-filter`, `enable-input`, and `input-test` absent.
3. Restart the PSP, reconnect the stored controller, and try controls for
   approximately 15 seconds. Do this before deleting or pairing the device
   again, so the failed stored state is retained in the trace.
4. Copy the new `probe.log` into the development folder. Record whether the
   PSP shows Connected and whether controls work in this debug build. Return
   the trace even if debug timing makes the symptom change.

The debug build also enables the existing firmware capture mechanism. Only
the current log is required for this comparison. Release builds continue to
exclude the diagnostic paths and create no logs or firmware dumps.

## New observations

`bt-input stored-record` logs the query result, record-size validity,
DualSense name-match boolean, capability word and final metadata selection.
It uses the record already queried by setup and does not request another
record, change that record or print its name, address or pairing material.
This distinguishes a stored record rejected by the current name/capability
guard from a record that is not available at all.

`bt-session` provides periodic snapshots of the native link flags, cached
record classification, prepared slot, matching context/channel/mode, entry
presence, pending CDC callback category and saved-context match, readiness,
wide-read route, generation, source acceptance, initialization attempts,
forwarded/unhandled counts, active source and packet queue/drop counters.
`rx` is 0 for no callback, 1 for Sony's callback, 2 for the legacy bridge,
3 for the wide bridge, or 4 for another callback. `tick` is the low 32 bits
of system time in microseconds; it can wrap during a long session.

Snapshots read only fixed module/bridge state; they do not follow the native
entry pointer, issue new device queries, perform remote DID discovery,
submit a read, write controller output or modify a session. They emit at
most once per second, capped at 20 per link-flag state. A line fits within
the existing bounded logger. Stored-record observations still occur during
ordinary setup attempts, even when that snapshot budget is exhausted.

The trace will distinguish metadata rejection, absent/failed receive setup,
initialization without input, rejected report framing and forwarded input
that does not reach the PSP sampler. Existing compiled transport, sink,
reconnect and build-profile tests validate that adding diagnostics retains
those code paths and that release artifacts exclude the new strings/logger.
They do not reproduce or solve the physical stored-device failure.
