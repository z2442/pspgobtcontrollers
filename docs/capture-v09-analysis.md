# Version 0.9: initialization written, no forwarded reports

The supplied log contains two probe 0.9 boot inventories. The registration and
connection attempt in the latter run records:

```text
bt-api end op=register-scan event=1 result=00000001
bt-api end op=hid-connect event=1 result=00000000
bt-input connected device=1 flags=0202; checking HID report setup
bt-input identity device=1 info=00000000 identity=802F0153 vendor=0000 product=0000
bt-input identity selection=2 device=1 (1=numeric 2=paired-name after 802F0153)
bt-input init device=1 source=0 channel=2 bytes=79 result=0000004F generation=0 attempt=1
```

A later connection attempt returns `0x802F0140`. No input-sink startup,
read-queued, unhandled-report, or forwarded-report lines appear.

The identity fallback now runs and the CDC writer returns a byte count of 79.
That proves the firmware accepted the requested USB transfer length, not that
the controller processed the report. Receive generation `0` means no matching
initial submission was wrapped by this bridge before that initialization.
An earlier pending read could retain Sony's original callback. The log does
not yet prove that this is the cause of absent input.

## Version 0.10 pending-read attachment

The captured CDC read-submission implementation stores three words per
logical channel: callback at state offset `0x7C`, buffer at `0x80`, and context
at `0x84`, with a 12-byte channel stride. The state base is CDC text base plus
`0x23A0`. New guards check its address-construction instructions, channel
index calculation, pending-read test, and all three stores against the
supplied executable before this state can be used.

For a connected, selected DualSense, the bridge checks its native context and
current buffer entry. It replaces the pending callback only if all of these
match:

- The callback is Sony's original HID receive callback.
- The callback's saved context is this exact native device context.
- Its saved buffer is this context's current aligned, bounded receive buffer.

The replacement copies data then calls that same original receive callback.
Only the callback pointer changes; the driver keeps ownership of the pending
request, buffer, context, and completion result. The source generation advances
to invalidate old queued input. An already attached matching callback remains
unchanged. Empty slots, unrelated callbacks, mismatched contexts, and mismatched
buffers remain untouched. No new read is submitted into a native entry that
may already have been freed after a submission failure.

The new diagnostic line is:

```text
bt-input pending-read device=1 source=0 channel=2 native-mode=... pending=... entry=... context-match=... buffer-match=... adopted=...
```

Pending category `0` is empty, `1` is Sony's callback, `2` is the bridge, and
`3` is another callback. `adopted=1` confirms that ownership checks allowed
attachment; it does not establish a report completion. These observations
will distinguish an existing request from an absent request or mismatched
receive path without assuming the cause of the failure.

## Validation and test

Host tests check the CDC instruction signature and refusal on changed words,
wrong bases, null images, and truncated images. Six new tests execute the
compiled PSP attachment function in MIPS emulation, including KSEG0 memory
access. They verify matching attachment, idempotence, unchanged context/buffer
words and buffer contents, and refusal for unrelated callbacks, empty slots,
wrong contexts, and wrong buffers. All 14 compiled bridge tests and eight
existing XMB wrapper tests pass. The guard matches the supplied CDC executable
at base `0x88226700`. This still requires hardware input verification.

Keep the working pairing. Install version 0.10, archive the old log as
`probe-v09.log`, restart with the three optional markers absent, reconnect
once, and try the D-pad and sticks in the XMB for about 15 seconds. Return the
new log. Inspect `bt-input pending-read`, initialization, sink startup, and
forwarded/unhandled report lines together. If a subsequent connection fails,
retain that result too.
