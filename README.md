# PSPGoBT

Use a DualSense through the **PSP Go's built-in Bluetooth**, presenting its
input through the native DS3 external controller port.
Target: PSP Go / 05g, ARK-5, underlying Sony firmware 6.60 or 6.61.

**The user confirms that v0.27 works**, with input delays or stalls under
heavy game load. Version 0.28 targets that scheduling and CPU overhead: the
input worker wakes on received data, runs at priority `0x10` instead of `0x24`,
and processes bounded batches before yielding. Idle maintenance wakes at most
20 times per second instead of 200. CRC validation uses a small lookup table,
and complete reports decode without an extra assembly copy. The working
pairing, reconnect and game-transition changes are retained. Performance on
hardware still needs testing. See [v0.28 performance notes](docs/performance-v028.md).
Release logging and firmware dumps remain compiled out.

## Install the release

1. Extract `pspgobt-release-v0.28.zip` and copy `seplugins/pspgobt/pspgobt_probe.prx`
   into `ef0:/seplugins/pspgobt/`.
2. Keep any existing working pairing. Set this PRX's entry in
   `ef0:/seplugins/PLUGINS.TXT` to the following, replacing an existing
   `vsh` entry for this PRX so it also loads in games and POPS:

   ```text
   always, ef0:/seplugins/pspgobt/pspgobt_probe.prx, on
   ```

3. Leave optional `enable-filter`, `enable-input`, and `input-test` markers
   absent for normal use, then restart the PSP.
4. Reconnect through **Bluetooth Device Settings → Manage Bluetooth Devices**.
   For a new controller, scan/register using **Create + PS** on the DualSense.

The filename and install path are unchanged. ARK's
[plugin runlevels](https://github.com/PSP-Archive/ARK-4/wiki/Plugins)
control whether the PRX is loaded in each application; a VSH-only entry
cannot maintain input once that PRX is unloaded on game launch.
The release creates no diagnostic logs or firmware dumps. Existing `probe.log`
and `*.text.bin` captures may be archived or removed; they are not needed for
normal use and are not removed automatically. If disconnect/reconnect still
fails, use the separate v0.28 session-trace build (following the
[trace procedure](docs/connect-v027.md)) and return
`session.log`. It does not enable firmware dumps. Full development diagnostics
remain available through `PSPGOBT_DEBUG=1`.

If `controllers.cfg` has not been learned yet, start the PSP with the controller off, then connect the
existing paired controller in the XMB and wait two seconds. This learns its
address in `ef0:/seplugins/pspgobt/controllers.cfg` (32 bytes). Do not remove or
pair it again. Later PRX instances can recognize that address from the PSP's
paired-device table even when the Bluetooth link is already open. This small
configuration file contains up to two controller addresses and a checksum;
it contains no link keys, PINs, input reports or firmware data. If it is absent
or invalid, the normal metadata selection path remains available.

After upgrading, keep the current pairing and test controller power-off/on,
a PSP reboot, suspend/resume, and launching a PSP game and a PS1 game.
The input bridge handles native incoming connections as well as the XMB
Connect action. See [reconnect notes](docs/connect-v027.md).

For the v0.28 test, replace the PRX and restart the PSP. Keep `controllers.cfg`,
the existing pairing and the `always` entry. Test a demanding game with rapid
presses/releases and analog movement, including the low CPU clock settings you
normally use. Check both sustained play and launching/exiting games while
leaving the controller powered on. No CPU clock override is applied by the plugin.

The XMB should now display the **HID** device type for the controller. The
plugin's internal mode has an explicit HID label and native HID connection
methods, including when the saved object previously had an audio type. Input still reaches the PSP's DS3 external controller port.
After a completed disconnect, the Triangle menu should offer **Connect**.
The working custom HID classification is retained.

Input source selection requires a live connected flag and either a known
controller address matching the current native paired slot/capabilities, or
an 80-byte cached record with the expected DualSense name and capabilities
(`selection=3`). This also works when a new PRX instance has no prepared mask
yet. The worker never issues remote DID discovery. Peer classification is a routing guard,
not authentication; native Bluetooth pairing still establishes the link.
Other sources are not selected by this input worker. Public identity APIs
and other devices' native setup remain unchanged. The internal
native setup call skips the DID helper only for a matching prepared context,
clears its local IDs to unknown, and returns an explicit internal "not queried"
sentinel. It never supplies fabricated Sony IDs. The working pairing procedure
and normal XMB parameter default remain enabled.

For a classified DualSense, the XMB Connect request uses generic HID mode 0,
and the local HID constructor uses the generic branch even if native link
flags include the alternate controller profile. The real link flags are not
rewritten. Input setup failure retries with backoff rather than calling Sony's
Bluetooth disconnect helper. Surviving or stopped read contexts are cleaned
up through the guarded local cancel routine and rebuilt with 128-byte staging.

The intended architecture is normal Bluetooth pairing and connection for the
DualSense, followed by DualSense report decoding into the native PSP DS3 input
driver. A guarded receive bridge now copies reports into a worker, decodes
them, and starts native port 1 after accepted input. Following the Vita
plugin's connection flow, it identifies the device before waiting for input,
sets up native HID reads when needed, and sends a CRC-signed `0x31` output
report. This path has compiled/emulated validation and the user's hardware
confirmation on 6.60 ARK-5.

## What is implemented

- A resident kernel PRX using ARK's chained module-start handler.
- Debug-only module inventory and executable-text/import/export captures for Bluetooth,
  USB host transport, HID, input, controller, and XMB system configuration modules.
- Guarded hooks for the XMB's `sceBSMan` device query, two registration APIs,
  connection call, and vendor/product query, up to 64 calls per API per boot.
  Debug builds trace these calls. The connection and pairing hooks remain
  active in release builds; original API arguments, output and results are preserved.
- An automatic, guarded XMB profile-routing patch. It requires a matching
  instruction signature, a native profile type or our controller mode,
  capabilities `0x0402`, and
  a UTF-16 name beginning `DualSense` followed by a space or terminator.
  The name is a routing guard, not proof of identity or authentication.
  User-mode wrappers select controller mode `9` during registration and
  saved-device loading, replace old method objects with Sony's HID method
  kind `2`, and handle the type label explicitly. Settings store native type
  `2`, never the plugin mode. Other devices keep their original handling.
- A guarded registration-method patch for DualSense fallback scan objects.
  It passes the original 84-byte scan record to the firmware's full-record
  registration API instead of the address-only API. A terminated buffer using
  the normal XMB pairing-parameter default stays in user RAM until the
  synchronous API returns.
  Other devices retain their original registration path; a failed full-record
  procedure returns its real error without creating an address-only fallback.
- Debug-only guarded traces of the registration helper's vendor commands `0x62` and
  `0x65`, up to 64 calls per boot. They record command metadata and returned
  status, preserving arguments, K1, delay slots, and results. Payloads,
  addresses, keys, and user-supplied PINs are not logged.
- An optional in-memory XMB discovery-filter patch, using `btfree`'s published
  6.6x offsets. Both original MIPS instructions must match before either is
  changed. Unsupported firmware, changed images, and partial patches are
  rejected. Cache maintenance follows a successful write.
- A portable full Bluetooth `0x31` report decoder with size, hat, and CRC checks,
  plus DS3 button/stick/pressure report encoding and a DualSense Bluetooth
  output report builder. Exact 10-byte basic `0x01` reports are also decoded.
- A guarded Bluetooth receive bridge, connection notification, native HID
  setup, and aligned initialization write with bounded retries. It retains
  Sony's read-buffer ownership and callback arguments. A worker handles
  decoding and logging; connection generations, report age, disconnect, and
  suspend clear stale input. An unloaded manager clears the session; a
  replacement is checked and rebound by the existing worker. Game/POPS
  transitions still require hardware validation.
- A resident worker starts even when Bluetooth modules are unavailable during
  the initial inventory. Provider installation is retried after they load.
  A queued read with no valid reports for two seconds receives up to three
  local recovery attempts, preserving the Bluetooth link. Valid input resets
  that budget. A full 79-byte output write is required to mark initialization
  complete.
- Kernel `module_reboot_before` and `module_reboot_phase` exports retire the
  outgoing instance. They block new input work, let the worker cancel owned
  local reads, restore only unchanged owned hooks, and detach only its own
  sampler handler. The early phase waits up to 250 ms for cooperative worker
  exit; the late phase does not issue firmware calls through the transport.
- A guarded HID-service discovery bypass for DualSense metadata cached before
  native HID setup. This avoids its synchronous descriptor discovery before
  the first read. Other devices retain their native service call and result.
  Metadata selects this route; publishing input still requires the separate
  connected-source policy. No Bluetooth pairing or connection result changes.
  Prepared DualSense startup skips native/worker DID discovery. Other devices
  retain their original native DID call and result.
- Separate 128-byte aligned DMA staging for selected DualSense reads, allowing
  an entire 79-byte full frame to reach the decoder. Sony's buffers retain
  their 64-byte capacity and receive only a bounded compatibility prefix.
  Their original callback still owns the queues and reposts each read.
- Native incoming-connection setup is queued onto the bridge worker, where
  the stored paired record is prepared before HID setup. A 200 ms check of the
  native controller-profile flags discovers existing connections and retries
  metadata that is not ready yet. Duplicate notifications reuse the context
  and successful initialization. Each new input generation reclaims the
  external-port transfer handler and passthrough.
- A native DS3 port-1 input sink following
  [crozone's example](https://github.com/crozone/PSP-EmulatedControllerTest).
  It converts decoded input into the 48-byte external-port payload, retains
  both sticks, and enables passthrough into ordinary PSP controller reads.
  Its sampler releases input after 250 ms without a valid report. Disconnect
  and suspend/resume clear cached input. The user's input test is confirmed;
  reconnect and suspend coverage across hardware/firmware remains limited.
- An opt-in local analog-to-D-pad test to check that native sink independently
  of Bluetooth pairing.
- Host tests, optional MIPS wrapper emulation, and offline capture tools.

The protocol core maps Create to Select, Options to Start, PS to the DS3 PS
button, and preserves both sticks, L1/R1, L2/R2, L3/R3, D-pad, and face buttons.
Touchpad, mic, sensors, haptics, adaptive triggers, battery reporting, Edge
buttons and USB input are not implemented.
DS3 sensor fields contain fixed nominal values; sensor emulation is not tested.
Full reports require CRC validation; basic reports rely on selection of the
connected source. Raw USB split-frame assembly is described in the capture
analysis. The v0.22 native manager-state changes remain unverified on physical hardware.

## Build

### GitHub Actions and releases

[Build and release](.github/workflows/release.yml) runs the host checks,
available MIPS tests, and a quiet release build on pushes and pull requests.
You can also run it from **Actions → Build and release → Run workflow** to
download a build artifact without publishing a release.

To publish the current version, commit and push the workflow and source, then:

```sh
git tag -a v0.28 -m "PSPGoBT v0.28"
git push origin v0.28
```

A successful tag build creates a GitHub Release with the installable ZIP,
the standalone `pspgobt_probe.prx`, and `SHA256SUMS`. The tag must exactly match
the version in `PSP_MODULE_INFO` in `src/plugin.c`; package names are derived
from that declaration. Future releases need that version updated before
tagging. CI explicitly disables debug logging and session tracing.

The workflow uses a pinned PSPDEV release with a SHA-256 check and the
repository's automatic `GITHUB_TOKEN`; no personal access token is needed.
Only the publication job has `contents: write`. Re-running a tag build updates
that release's assets while preserving its notes (immutable releases reject
replacement). Local Sony firmware captures are not uploaded or required;
capture-dependent tests skip when those files are absent.

### Local build

Install a current [PSPSDK](https://github.com/pspdev/pspsdk) with
`psp-config`, `psp-gcc`, and the `pspsystemctrl_kernel` import library on PATH.
There are no downloaded dependencies or proprietary firmware files in this
project.

```sh
make test
make
make package
```

The default is `PSPGOBT_DEBUG=0`: the PRX is `pspgobt_probe.prx` and
`make package` creates `dist/pspgobt-release-v0.28.zip`, its latest alias
`dist/pspgobt-release.zip`, and the copy layout under `dist/`.

For the optional bounded session trace without firmware dumps:

```sh
make PSPGOBT_DEBUG=0 PSPGOBT_SESSION_TRACE=1 package
```

This creates `dist/pspgobt-session-trace-v0.28.zip`. Restore the quiet release
with `make PSPGOBT_DEBUG=0 PSPGOBT_SESSION_TRACE=0 package`.

To enable all development logs, import/export inventories, firmware dumps and
pairing-command tracing:

```sh
make PSPGOBT_DEBUG=1
make PSPGOBT_DEBUG=1 package
```

This creates `dist/pspgobt-debug-v0.28.zip` and `dist/pspgobt-debug.zip`
(also the legacy `pspgobt-bringup.zip` alias). Switching the flag rebuilds the
affected objects automatically; `make clean` is not required. Run ordinary
`make package` again to restore the release binary at the normal output path.

The release binary contains no logger, dump paths, `sceIoMkdir` import or
formatting functions. `sceIoWrite` is used only for the bounded controller
configuration file. Read-only optional-marker checks remain.
`make protocol` also creates `build/libpspgobt_protocol.a` for future PSP-side
transport integration. Host tests can run without PSPSDK using `make test`.
Use separate invocations for the host tests and PSP targets.

`make test-mips` additionally requires the Python `unicorn` package. It runs
the generated MIPS wrapper, including the original call's delay slot, with a
simulated decision syscall and native constructor. It also exercises the
patched registration method's call/return sequence. It checks stack,
arguments, return path, and the bounded profile edit; it does not emulate
PSP Bluetooth authentication or native registration internals.

`make test-bridge-mips` requires PSPSDK and Unicorn. It executes the compiled
PSP read-submission and initialization functions with mocked kernel/CDC
calls, checking all five read arguments, error propagation, buffer alignment,
the complete output frame/CRC, and initialization failure handling. It also
checks discovery selection, refusal cases, K1 and argument preservation. When
the local BSMan capture is present, two integration checks execute its actual
setup routine with the compiled hooks and mocked transport/service calls.
Sony captures are not shipped; those two checks skip without the local file.
The sink integration checks execute a complete full/basic HIDP packet through
the compiled receive callback, worker decoder, port-1 registration/passthrough,
and transfer handler. They verify actual button/stick/pressure fields and
timeout release, CRC rejection, buffer bounds, and transport-error handling.
Build-profile checks verify diagnostic exclusion and execute module setup to
confirm essential pairing/connection/input hooks still install with logs off.
Use `make PSPGOBT_DEBUG=1 test-bridge-mips` to test the debug variant too.

## Debug diagnostics on ARK-5

This section requires a `PSPGOBT_DEBUG=1` build. Release builds do not generate
the logs or captures described below.

1. Copy `pspgobt_probe.prx` into `ef0:/seplugins/pspgobt/`.
2. Append this line to `ef0:/seplugins/PLUGINS.TXT`, preserving existing entries:

   ```text
   vsh, ef0:/seplugins/pspgobt/pspgobt_probe.prx, on
   ```

3. If upgrading, replace the PRX and rename the old log to `probe-v012.log`.
   Leave `enable-filter`, `enable-input`, and `input-test` absent for this run.
   Restart the PSP. Enable Bluetooth and open **Bluetooth Device Settings →
   Manage Bluetooth Devices**. Keep the existing working pairing and reconnect
   the controller; use **Triangle → Connect** when available. If no pairing
   exists, scan and register using **Create + PS** on the DualSense.
4. Try the D-pad, sticks, and face buttons in the XMB for around 15 seconds.
   Record whether the PSP shows Connected and whether any controls respond.
5. Copy `ef0:/seplugins/pspgobt/probe.log` to your development computer.
   Copy any new `sceUSB_Host_Driver-*.text.bin` capture into the local development
   directory too; it can help trace the transport error if it persists.
   Captures are bounded to 128 relevant module
   instances per boot. `device-info event=` lines record the XMB query's
   result and record fields, not raw Bluetooth traffic. A successful trace
   setup is logged as `device-info trace setup result=0`.

No configuration marker is needed for Bluetooth input. Look for `bt-input
bridge setup result=0`, identity `selection=3` for the prepared route, then
`bt-input init`, `bt-input native sink start`, and `bt-input forwarded` lines.
Setup `-2` means an instruction mismatch, `-3` a provider mismatch, and `-7`
an active local-test marker. A returned write count alone does not prove
that reports reached the sink. See
[latest capture analysis](docs/capture-v12-analysis.md) for the observed
failure and next checks.

The `bt-input pending-read` line records the pending callback category and
whether its context/buffer match. `adopted=1` means its existing request now
chains through the bridge. No fresh request is submitted into a potentially
freed native buffer. That line is diagnostic progress, not proof of input.

For version 0.15, reconnect from the controller or the XMB. The native setup
events and startup poll queue `bt-input prepare` before HID setup. Look for
`native DID skipped`, `DID=not-queried`, `HID discovery`
with `bypass=1`, and `receive-submit ... capacity=128` before
initialization/forwarding. A begin
without its corresponding end identifies a stage that has not returned.
Return the complete log even if the last stage is earlier than discovery.

Registration, Bluetooth connection, and controller initialization are separate
stages. Continued flashing alone does not identify the failed stage. Return
the new log even if no error code appears; the hooks record
whether the XMB called the native HID connection API and what it returned.

Each `bt-api trace setup op=... result=0` indicates an installed hook.
`bt-api begin` / `bt-api end` lines report the observed operation and its
original result; `bt-connect` records the connection-mode argument. Negative
setup values leave that import unchanged (`-9` means the provider code did
not match). A route hit followed by `bt-connect` / `hid-connect` results will
show whether the native HID connection stage was reached. Version 0.3 stopped
logging record offset `0x06`, which proved to be the first peer-name character.

The first load of a captured module may pause while files are written.
Captures contain Sony executable code; keep them local rather than committing
or redistributing them. The probe copies executable text, not runtime data or
the pairing database. Executable text can include constants and strings.
Log addresses and UIDs identify the relocated image for disassembly.

By default, the XMB filter is unchanged. To compare against the known filter
bypass, create an empty file named `ef0:/seplugins/pspgobt/enable-filter`, then
restart and repeat registration. `probe.log` records whether the guarded
patch was applied or refused. A successful filter patch is not evidence of a
working HID connection. Disable other Bluetooth patch plugins for this
comparison; otherwise their changes may already be present in a capture.

To remove the probe, disable its line in `PLUGINS.TXT` and restart. It refuses
live unloading because another plugin can retain its module-start callback.
The routing and optional filter patches exist only in RAM and disappear on
restart without the plugin enabled.

## Test the native DS3 input hook

After installing the PRX, create an empty file named
`ef0:/seplugins/pspgobt/input-test` and restart. With a successful hook, moving
the PSP's own analog stick should navigate the XMB as D-pad input.
`probe.log` records the sink startup result (`0` means registration succeeded).
Returning the stick to center releases the emulated directions. Remove the
marker and restart to finish the test. This checks the controller hook; it
does not test Bluetooth or DualSense input. The test claims DS3 port 1, so a
real DS3 or another external-controller plugin must not share that port.

For transport development, the `enable-input` marker registers the native
sink without synthetic input. The kernel-only `PSPGoBT_driver` exports
`pspgobtSubmitReport(report, size)` and `pspgobtDisconnect()`. Submit accepts
only CRC-valid 78-byte full Bluetooth reports with their report ID included.
The Bluetooth bridge has an internal decoded-state path and automatically
starts the sink for accepted controller input. `enable-input` can still
register a neutral sink early. `input-test` disables the Bluetooth bridge
and takes precedence when both markers exist.

## Inspect captures

```sh
python3 tools/inspect_dump.py captures/*.text.bin > captures/summary.json
python3 tools/analyze_probe.py captures --output captures/report.json --disassemble build/captures
```

This computes file sizes, SHA-256 hashes, and the known XMB filter instruction
states. A match is only a candidate for the published filter patch, not a
firmware identity or compatibility proof.

Disassemble a text capture with the `base` from its matching `module` log line:

```sh
psp-objdump -D -b binary -m mips:allegrex -EL --adjust-vma=0xBASE capture.text.bin > capture.asm
```

Replace `0xBASE` with the actual hex address. Captures are relocated; use each
capture's own UID/base pair and do not compare absolute addresses across boots.

## Remaining implementation

1. Validate the identity fallback, report initialization, and actual receive
   framing on the user's PSP; inspect numeric identity query failure further.
2. Validate native input ownership with Sony's `padsvc`, read completion,
   disconnect/reconnect, and suspend/resume on real hardware.
3. Validate extended input,
   pressure fields, latches, HOME behavior, and idle handling on hardware.
   A DS3 wire report is not interchangeable with the external-port payload.
4. Verify pairing, release on disconnect/timeout, reconnect after restart,
   XMB-to-game transitions, POPS, and suspend/resume on real hardware.

The Vita references establish the controller protocol, but their `ksceBt*`
calls and taiHEN hooks are Vita interfaces. No PSP equivalents are assumed.
Crozone's demo supplies a concrete use of `sceCtrl_driver_E467BEC8` and
`sceCtrl_driver_6C86AF22`, beyond uOFW's incomplete reconstruction. The adapter
resolves those exports dynamically; the normal diagnostic mode does not call
them. A successful build still needs verification on the user's ARK-5 console.

See [research notes](docs/research.md) for source links and revisions.
