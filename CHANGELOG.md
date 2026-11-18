# Unreleased

- Include Controller Mapping in release builds. Both `pspgobt_probe.prx` and
  `mapping_ui.prx` ship together; `PSPGOBT_MAPPING_UI=0` builds a separately
  named `-no-mapping` package without the XMB menu hooks.
- Fix the Controller Mapping freezes found in the captured Bluetooth module.
  The generic option list (`mlist_bt_config_option`) jumps directly into the
  second instruction replaced by the menu hook and then called through an
  unloaded register; it is now redirected to a stub that replays that
  instruction. Selecting a mapping row no longer rebuilds or reopens the menu:
  Sony's page close is asynchronous, and any reopen we started (during the
  action, or from the close-completion handler as tried on hardware) was
  overtaken by it, leaving an empty page or a freeze. Each row now records the
  next screen and the menu closes normally; pressing Triangle again opens that
  screen through the unmodified native Triangle path. Native Bluetooth actions
  and the back button are unchanged. Helper ABI 4; update both PRXs together.
- Reuse one mapping syscall stub, retire the previous helper after Bluetooth
  Settings unloads, and keep large save buffers off the XMB thread's kernel stack.
- Fix input lag after a reboot/reconnect. Sony's Bluetooth HID setup sends two
  Bluetooth module link commands (0x79 and 0x24) only to a DS3; supported
  controllers never took that branch. Hardware traces showed their reports
  arriving late and in bursts (up to ~35 ms behind, ~90 ms just after
  connecting) while the plugin itself added under 1 ms. Every supported
  controller now receives those same commands, byte for byte, once per
  connection after identification. Confirmed on a PSP Go with a DualSense.
- Session-trace builds also record Controller Mapping setup steps and the link
  command results, and commit the trace file every 5 seconds so records survive
  holding power.
- Remove the mapper's shared Bluetooth resource-lookup hook. Build custom rows
  through the native wide-string/vector routines. Make mapping reads use a
  paired-address snapshot without semaphore waits or Bluetooth metadata queries.
  Exercise native item construction, destruction, and the complete action
  dispatcher with the helper relocated into user RAM. Hardware validation of
  the reported freeze remains outstanding.
- Read Bluetooth's native nine-element profile array correctly when adding
  Controller Mapping. The field at +0x1c is unrelated state and normally zero;
  treating it as a vector count hid the option for every controller. Exercise
  Sony's constructor and array allocation code in the menu regression test.
- Export the mapping service as a user-callable syscall so the XMB helper can
  load and install its menu hooks. Check the built PRX export table in the
  loader regression test and reject packages with a missing service export.
- Add **Controller Mapping** to a supported controller's Bluetooth device menu.
  Edit buttons, choose the PSP analog-stick source, invert axes, restore defaults,
  and save separate profiles by Bluetooth address. Apply profiles to XMB and
  game input and restore them after reconnects/reboots. Save outside input
  callbacks with checksum validation and backup recovery. Package the automatic
  user-mode `mapping_ui.prx` helper alongside the main plugin. Captured firmware
  menu/action execution and persistence tests pass; hardware UI testing remains.
- Merge current controller state into native buffer/latch reads so presses
  and releases can reach a game without another native sample. Keep remote
  game controls out of the sampler's cached frames while these hooks are
  active; retain native PS handling, physical PSP controls and button masks.
- Add experimental original Nintendo Switch Pro Controller Bluetooth support:
  guarded model selection and pairing routes, regular report-mode setup,
  positional face-button mapping, digital triggers, calibrated sticks,
  player 1 light, and model persistence for reconnects. Host protocol and
  emulated PSP bridge tests cover setup, input and reconnects; live PSP Go
  pairing and input still need hardware verification.
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
