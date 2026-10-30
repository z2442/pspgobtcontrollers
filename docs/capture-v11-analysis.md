# Version 0.11: discovery bypass reached, receive error before input

The supplied log confirms probe 0.11, Sony firmware `06060010`, model 4.
Registration and HID connection succeed. The new setup observations are:

```text
bt-input prepare device=1 info=00000000 metadata=1
bt-input native DID begin device=1 event=1
bt-input identity selection=2 device=1 (1=numeric 2=paired-name after 802F0153)
bt-input pending-read device=1 source=0 channel=2 native-mode=1 pending=0 entry=1 context-match=0 buffer-match=0 adopted=0
bt-input init device=1 source=0 channel=2 bytes=79 result=0000004F generation=0 attempt=1
bt-input native DID end device=1 event=1 result=802F0153
bt-input HID discovery begin device=1 source=0 bypass=1 event=2
bt-input HID discovery end device=1 bypass=1 event=2 result=00000000
bt-input read closed/failed source=0 generation=2 device=1 result=80300009
```

The native DID query did eventually return. The discovery bypass ran and
returned success, so the earlier hypothesis of an indefinitely blocked HID
service call does not explain this run. The bridge reached receive setup,
but no valid packet reached the decoder or external-port handler.

Generation 2 and the negative status are consistent with the first submission
advancing generation to 1 and a subsequent failed completion advancing it
again. A queued-start event from an earlier generation can be dropped by the
worker's freshness checks. Version 0.12 therefore also logs the first
submission's actual return and requested capacity directly from the setup
thread. This log does not establish the timing or meaning of `0x80300009`;
it must not be labeled as authentication failure, overflow, or cancellation
without further evidence.

## Version 0.12 receive staging and ordering

Sony's native buffer pool supplies 64-byte entries and requests 64 bytes per
read. A complete DualSense Bluetooth input is 78 report bytes plus its HIDP
header, totaling 79 bytes. A 64-byte request can only receive it in pieces
if the underlying firmware/USB endpoint permits that behavior. We have not
observed such pieces. Increasing capacity is an experiment directed at this
mismatch, not a proven interpretation of the reported error.

For a source whose HID discovery was bypassed, the guarded first/reposted
read wrapper now checks the native entry's buffer ownership and substitutes
a separate, per-source, 64-byte-aligned **128-byte staging buffer**. The
original CDC API receives that pointer and capacity; its existing cache
maintenance and transfer submission remain in use. Ordinary native sources
retain their original 64-byte requests. Sony's entries are never enlarged.

On completion, the bridge copies the entire bounded packet into its worker
queue. It copies at most 64 bytes into the original Sony entry and chains the
original callback with that bounded length and the original buffer pointer.
The native loop retains queue ownership and reposts through the same guarded
wrapper. Its generic HID descriptor parsing was skipped for this source;
only the bridge decodes the complete DualSense packet. CRC validation and
source selection still precede publishing input. Existing 64/15-byte fragment
assembly remains available if the transport completes reads in those pieces.

Zero/negative completions invalidate input generation and initialization
readiness while preserving the status passed to Sony. An impossible
completion larger than staging capacity is refused without copying and
closes the native read with an internal failure. Failure is not converted
into controller input or a successful connection result. The callback only
performs bounded copies; parsing, identity checks, output and logs run on
normal threads.

Initialization now waits until a read has been queued successfully or a
matching pending request has been attached. Previously the generation-0
79-byte output could be sent while native DID setup was still running.
Read failure/suspend clears readiness; a synchronous failed completion
during submission cannot be overwritten by its submission's success return.

The probe also captures the USB host module's executable text locally, using
the same bounded capture mechanism. It imports the transport underlying CDC
and can help locate this error if it persists. Captured executable code is
excluded from release packages and must remain local.

## Native DS3 transfer handler

The output implementation follows the user's
[PSP-EmulatedControllerTest example](https://github.com/crozone/PSP-EmulatedControllerTest):
`sceCtrl_driver_E467BEC8` registers an 8-byte handler/source pair on port 1,
and `sceCtrl_driver_6C86AF22(1)` enables passthrough. The handler supplies the
48-byte external-controller state with buttons, both sticks and pressures.
It preserves the sampler timestamp. This stage is already implemented and
starts on the first valid selected Bluetooth report.

New compiled integration checks inject independently constructed basic/full
HIDP fixtures at the receive completion boundary, then execute the actual
worker decoder, source identity policy, sink registration, state update and
transfer callback in MIPS emulation. They verify the two resolved NIDs,
port 1, handler size/callback, passthrough mask, Up/Cross/L1/PS, axes, trigger
pressures, timestamp and timeout release. A bad full-report CRC never
registers or injects input. Transport-error and oversize checks verify that
Sony's 64-byte entry and its following bytes remain untouched on failure.
These tests prove the software path under mocked kernel APIs, not that the
physical Bluetooth link is delivering packets or that XMB input is working.

## Hardware test

Keep the existing pairing, replace the PRX, archive the old log as
`probe-v011.log`, and restart with optional markers absent. Connect manually
through Manage Bluetooth Devices, then try the D-pad/sticks/face buttons for
around 15 seconds. Return `probe.log` and any new `sceUsbHost*.text.bin`
capture to the local development folder. Check `receive-submit` with
`capacity=128`, followed by initialization, native sink startup and forwarded
reports. Retain any actual failure status. Version 0.12 input remains
unverified on hardware.
