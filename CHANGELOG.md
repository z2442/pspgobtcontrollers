# Unreleased

- Remember the controller model with its paired address so later connections
  can enable input without another metadata lookup. Read existing address-only
  configuration files and upgrade them after identifying the controller.
- Retry activation when a completed write produces no input, advancing the
  DualSense output sequence. Continue silent-stream recovery with a cooldown
  after the initial three attempts, keeping the working pairing intact.
- Let the native controller sampler consume already received input from a
  selected, initialized controller without waiting for the input worker.
  Preserve ordered fragments, CRC checks, timeout and lifecycle guards.
  Hardware latency improvement still needs confirmation.

# PSPGoBT v1.0

First stable release for using a DualSense with the PSP Go's built-in Bluetooth.
The user has confirmed working input, reconnection, game transitions, and the
input scheduling improvements on a PSP Go running ARK-5 with Sony firmware 6.60.
Firmware 6.61 is targeted by the guarded patches; hardware verification is limited
to the reported 6.60 setup.

## Features

- Pair and connect through the PSP's Bluetooth settings using a custom HID route.
- Forward decoded DualSense controls through the PSP's native DS3 external port.
- Preserve pairing across reconnects, reboots, and XMB/game transitions.
- Wake input processing on received reports, with higher worker priority and
  bounded batches that yield CPU time to games.
- Reduce periodic idle worker wakeups from 200 to 20 per second; speed up CRC
  validation and decode complete reports without an extra assembly copy.
- Release held input on disconnect, suspend, or expired reports.
- Compile out logging, session tracing, and firmware dumps in release builds.
- Build, test, and publish versioned release assets through GitHub Actions.

## Installation and upgrades

Copy `seplugins/pspgobt/pspgobt_probe.prx` to `ef0:/seplugins/pspgobt/` and enable:

```text
always, ef0:/seplugins/pspgobt/pspgobt_probe.prx, on
```

Restart the PSP after replacing the PRX. Keep the existing pairing and
`controllers.cfg`; re-pairing is not needed when upgrading from a working build.
For a new pairing, hold Create + PS until the DualSense flashes, then register it
in the PSP's Bluetooth settings.

Touchpad, microphone, motion, haptics, adaptive triggers, battery reporting,
Edge-specific controls, and USB input are not implemented.
