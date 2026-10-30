# Version 0.12: overlapping DID queries and a cleared context

The updated single-boot log identifies probe 0.12, firmware `06060010`, model 4.
Registration returns slot 1 and HID connection returns success. Startup then
records:

```text
bt-input prepare device=1 info=00000000 metadata=1
bt-input native DID begin device=1 event=1
bt-input connected device=1 flags=0202; checking HID report setup
bt-input identity device=1 info=00000000 identity=802F0141 vendor=0000 product=0000
bt-input identity selection=0 device=1 (1=numeric 2=paired-name after 802F0153)
bt-input native DID end device=1 event=1 result=802F0153
bt-input HID discovery begin device=0 source=0 bypass=0 event=2
bt-input HID discovery end device=0 bypass=0 event=2 result=80310102
```

There is no receive submission, initialization write, or forwarded input in
this run. The 128-byte receive change was therefore not exercised. While the
native setup query was outstanding, the worker started another DID query and
returned a different error. When setup reached the next hook, the same context
held device slot zero. That proves the context had been cleared, not why it
was cleared. The log alone cannot establish whether overlapping queries,
peer disconnection, a timeout, or another event caused it. No new interpretation
is assigned to `0x802F0141` or `0x80310102`.

## Version 0.13: prepared startup without remote DID discovery

The plugin already identifies a DualSense routing candidate before calling
the XMB's original connection function, using the slot's registered 80-byte
record, name and `0x0402` capabilities. Native pairing/connection results are
not replaced. Version 0.13 uses this prepared slot plus a live connected flag
to avoid remote DID discovery on both startup threads:

- At the one guarded internal native setup call, the wrapper requires a live
  prepared device and a matching native context. It clears the two local ID
  words to zero/unknown and returns the internal sentinel `-1` without calling
  the remote helper. The captured caller ignores the return and compares the
  two ID words to select DS3-specific setup. Zero prevents that branch from
  being selected by uninitialized stack contents. This is an intentional
  setup bypass, not an observed firmware query result. Nonmatching calls and
  public DID APIs retain their original behavior and outputs.
- On the input worker, a source prepared before connection is selected only
  if a fresh device record still matches, the bridge is running, Bluetooth
  still reports connected, and no suspend/power epoch change invalidated the
  preparation. It returns selection category 3 and explicitly logs that DID
  was not queried. Failed queries, changed metadata, disconnect and suspend
  refuse the source without launching a DID query. Unprepared sources retain
  the existing numeric policy and narrow `0x802F0153` fallback.

Category 3 relies on registered-device metadata and the native connected link.
It does not verify numeric VID/PID. A matching name is not authentication.
This is deliberate source routing for a peer paired by the PSP; no Sony IDs
or successful DID results are fabricated. Valid full reports still require
their CRC; basic reports still require the selected connected source. The
native DS3 external-port sink starts only after a valid decoded input report.

The change removes the observed overlap and startup wait for the selected
DualSense. It does not prove these queries caused the context loss or that
the transport will now deliver input. The native HID-service bypass, 128-byte
staging, bounded original-buffer copies, generation checks and read-before-
initialization ordering remain in place.

## Missing USB host capture

The log's existing module inventory names the provider `sceUSB_Host_Driver`.
Version 0.12's capture predicate matched `usbhost`, missing the underscore.
Version 0.13 also matches `usb_host`, with a host check using this exact
observed module name. On restart it should produce
`sceUSB_Host_Driver-<UID>.text.bin` locally. Capture contents remain excluded
from releases.

## Validation and next hardware test

Host protocol/capture checks pass. Compiled tests verify category 3 and
refusal after changed metadata, failed record query, disconnect, suspend or
power transition, with no remote DID call in any prepared case. The local
captured-setup integration check now injects a context-clear failure if the
remote helper is called: prepared startup avoids it and reaches the 128-byte
read. Local ID slots are prefilled with DS3 IDs to verify both are cleared and
the native DS3-only branch is avoided. Unprepared setup still calls the real
helper interface and retains its service failure. The full/basic report-to-
transfer-handler checks use category 3, and an additional check retains the
old fallback. These are mocked-kernel tests, not a physical controller test.

Keep the working pairing. Replace the PRX, archive the old log as
`probe-v012.log`, and restart with optional markers absent. Manually connect
through Manage Bluetooth Devices, test controls for around 15 seconds, and
return the new log plus any `sceUSB_Host_Driver-*.text.bin` capture. Look for
`native DID skipped`, `DID=not-queried selection=3`, HID discovery `bypass=1`,
and `receive-submit ... capacity=128`, then initialization/sink/forwarding.
Version 0.13 physical input remains unverified.
