# PSPGoBT

Use a **DualSense or DualShock 4 controller with the PSP Go's built-in Bluetooth**. PSPGoBT
pairs through the PSP's Bluetooth settings and sends controller input through
the native DualShock 3 external controller interface.

**Current release: v1.0 supports DualSense.** Build this source for DualShock 4
support until a new release is published. See the [release notes](CHANGELOG.md).

## Requirements

- PSP Go / 05g.
- ARK-5 custom firmware, with underlying Sony firmware 6.60 or 6.61.
- A DualSense or Sony DualShock 4 controller.

The DualSense setup has been confirmed on a PSP Go running ARK-5 / 6.60,
including reconnects, XMB/game transitions, and the input performance changes.
The patches also target 6.61; hardware verification is limited to the reported
6.60 setup. DualShock 4 support in source builds has passed host protocol tests
and a PSP toolchain build, but still needs a live PSP Go test.

## Installation

1. Download `pspgobt-release-v1.0.zip` from [Releases](https://github.com/z2442/pspgobtcontrollers/releases).
2. Copy `seplugins/pspgobt/pspgobt_probe.prx` into `ef0:/seplugins/pspgobt/`.
3. Add or update this entry in `ef0:/seplugins/PLUGINS.TXT`:

   ```text
   always, ef0:/seplugins/pspgobt/pspgobt_probe.prx, on
   ```

4. Restart the PSP.
5. Put the controller into pairing mode: hold **Create + PS** on DualSense, or
   **Share + PS** on DualShock 4, until its light flashes. Register it in the
   PSP's **Bluetooth Device Settings**.

The controller should appear as a **HID** device. Keep the `always` entry so the
plugin loads in the XMB, games, and applications.

**Upgrading:** replace the PRX and restart. Keep your existing pairing and
`controllers.cfg`; you do not need to remove and pair a working controller again.
Leave development markers such as `enable-filter`, `enable-input`, and
`input-test` absent for normal use.

## Controls and behavior

- D-pad, face buttons, sticks, shoulders, and triggers are forwarded through the
  native DS3 input interface. PSP games use the PSP's native controller mapping.
- **Create/Share** maps to Select, **Options** to Start, and **PS** to the native PS button.
- Input processing wakes on received data, with bounded batches to give games
  CPU time. Source builds also let the native sampler decode queued input from
  an established stream immediately, without another worker pass. The plugin
  does not change the PSP CPU clock. See [input latency](docs/input-latency.md).
- Disconnecting, suspending, or receiving no valid report for 250 ms releases
  held input.
- Release builds generate no diagnostic logs or firmware dumps.

The plugin may save a small `controllers.cfg` file in its installation folder
so later plugin instances can recognize paired controllers. It contains up to
two Bluetooth addresses and a checksum, with no link keys or input history.

Touchpad, microphone, motion, haptics, adaptive triggers, battery reporting,
Edge-specific controls, and USB input are not implemented.

## Troubleshooting

- **Works in the XMB only:** check that the plugin entry starts with `always`
  and restart the PSP after editing it.
- **Already paired when upgrading:** keep the pairing and `controllers.cfg`,
  replace the PRX, and restart the PSP.
- **Missing `controllers.cfg`:** connect the paired controller in the XMB and
  wait two seconds so the plugin can learn it before launching a game.
- **Connection issues:** report your firmware, plugin version, and reproduction
  steps in [Issues](https://github.com/z2442/pspgobtcontrollers/issues).
  A session-trace build can capture connection events without firmware dumps;
  see the build commands below and the [trace procedure](docs/connect-v027.md).

## Building

Install [PSPDEV / PSPSDK](https://github.com/pspdev/pspdev), including
`pspsystemctrl_kernel`, and put its tools on `PATH`. Python 3 and a host C
compiler are required for packaging and host tests.

```sh
make test
make PSPGOBT_DEBUG=0 PSPGOBT_SESSION_TRACE=0 package
```

The quiet build produces `pspgobt_probe.prx` and
`dist/pspgobt-release-v1.0.zip`. Package versions are read from the
`PSP_MODULE_INFO` declaration in `src/plugin.c`.

For the MIPS tests, install `unicorn==2.1.4` and run these commands separately:

```sh
make test-mips
make PSPGOBT_DEBUG=0 PSPGOBT_SESSION_TRACE=0 test-bridge-mips
```

Tests using local Sony firmware captures skip when those files are absent.
Firmware captures are not distributed with the project or its releases.

Optional diagnostic builds:

```sh
# Bounded session.log, without firmware dumps.
make PSPGOBT_DEBUG=0 PSPGOBT_SESSION_TRACE=1 package

# Full development diagnostics and firmware dumps.
make PSPGOBT_DEBUG=1 PSPGOBT_SESSION_TRACE=0 package

# Restore the quiet release afterward.
make PSPGOBT_DEBUG=0 PSPGOBT_SESSION_TRACE=0 package
```

In v1.0 session traces, BOOT's version field is `00010000`
(`major << 16 | minor`).

## GitHub releases

The [Build and release workflow](.github/workflows/release.yml) tests and builds
on pushes and pull requests. **Actions → Build and release → Run workflow**
creates a downloadable build artifact without publishing a release.

After committing and pushing a version update, publish it with a matching tag:

```sh
git tag -a v1.0 -m "PSPGoBT v1.0"
git push origin v1.0
```

The workflow rejects tags that do not match the PRX version. Successful tag
builds publish the installable ZIP, standalone PRX, release notes, and
`SHA256SUMS` using GitHub's automatic token. It uses a pinned PSP toolchain
with a SHA-256 check and disables all release diagnostics. Only the publication
job has write permission. Reruns replace assets on the same release while
preserving its notes; immutable releases reject replacement.

## Development notes and credits

Implementation history and capture analyses are kept in [docs](docs/).
Start with the [research notes](docs/research.md),
[HID routing](docs/hid-mode-v025.md),
[transport handoff](docs/transport-v026.md),
[reconnect handling](docs/connect-v027.md), and
[input performance changes](docs/performance-v028.md).

The native input interface follows
[crozone's PSP-EmulatedControllerTest](https://github.com/crozone/PSP-EmulatedControllerTest).
See [LICENSE](LICENSE) and [third-party notices](THIRD_PARTY_NOTICES.md) for
licensing and attribution.
