# Input available when the game reads it

The earlier input change decoded received reports in Sony's controller
sampling callback. It still waited for that sampler to put the state into
the buffers that XMB and games read. A press arriving after a sample could
therefore wait for the next sample even if Bluetooth had already delivered it.
The user reports substantial delay compared with RemoteJoy Minus; this
boundary is a concrete software difference, not a measurement of total lag.

[RemoteJoy Minus](https://github.com/SourceK78/remotejoy-minus/blob/main/main.c)
merges cached remote state after successful native controller reads. PSPGoBT
now uses that approach through the captured 6.6x driver's common buffer-read
function and its peek/read latch functions.

## Read-time input

After Sony validates a read and returns its buffers, the hook refreshes queued
input from an established controller and merges the latest state. Normal
positive/negative, peek/blocking and multiple-frame reads keep their native
return values and timestamps. Native blocking reads still wait for their
normal sampling event. The plugin adds no additional sampling wait after
that event. Invalid or empty reads do not get rewritten.

PSP button mapping combines L1/L2 into the PSP L trigger and R1/R2 into R.
Physical PSP buttons remain active. Stick merging uses the axis with the
larger displacement and respects the native digital/analog sampling mode.
Native user and interception button masks also apply to injected controls.
Port-1 reads retain the full normalized external-controller frame, including
its axes and pressure fields.

While the read hooks are active, the registered sampling handler supplies
only PS and neutral game controls. This retains native system-button handling
while preventing an older remote press or analog value from lingering in the
sampler's buffers after release. Game buttons and sticks come from the latest
validated state at read time. Latch hooks maintain remote press/release edges,
with independent user and kernel read/clear state. Disconnect, suspend and
expired input release the cached remote controls.

## Work and lifecycle

The read hook may consume at most the existing queue's 16 packets, preserving
fragment order, CRC validation, source generation and timeout checks. It does
not perform selection, initialization, Bluetooth commands, logging or waits.
The worker retains that work. Read-time decoding is excluded while the worker
mutates source state, but remains available during configuration-file I/O and
port maintenance.
The worker also refreshes the PSP power timer during fresh remote button or
stick activity, retaining active-play behavior while game controls bypass
the sampler. Neutral or expired input does not keep the PSP awake.

Installation verifies the native registration accessor, controller state
location and all three function prologues before publishing trampolines and
patches. The copied instructions are relocation-free stack operations.
Unknown or changed entry points retain the previous native input path. A
changed port owner or hook also disables read-time injection and restores full
sampling behavior. Shutdown restores only entries still owned by this plugin.

## Installation and checks

Replace the PRX and restart the PSP. Keep the existing pairing,
`controllers.cfg` and `always` entry. Use the quiet build; no measurement
logging or diagnostic counters were added.

```sh
make test
make test-mips
make PSPGOBT_DEBUG=0 PSPGOBT_SESSION_TRACE=0 test-bridge-mips
make PSPGOBT_DEBUG=0 PSPGOBT_SESSION_TRACE=0 package
```

Compiled MIPS tests execute the actual captured XMB and game controller
functions. They change remote presses and releases after a native sample and
verify that the subsequent reads return the new values without another
sample. Checks cover all four normal buffer-read modes, multiple frames,
extended port data, timestamps, physical controls, sampling mode, button
masks, latch edges, disconnect/timeout/suspend, original errors, changed
instruction guards and foreign-hook preservation. The existing transport,
reconnect and controller protocol suites remain in place.

The code removes this extra software wait. Bluetooth delivery, game scheduling
and end-to-end responsiveness still require confirmation on a physical PSP Go.
