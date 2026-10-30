# Research notes

Version 0.22 follows the user's report that v0.21 is better, but a powered-off
controller still leaves Disconnect in the XMB menu. Captured manager execution
reproduces a terminal close indication leaving the active entry intact when its
service handshake bit is clear. The bridge now supplies native HID close-request
handling and runs Sony's own finalizer on a real matching terminal indication.
Tests execute the actual XMB connection query and verify cleared native state
and native notifications in both module layouts. Physical behavior still needs
confirmation. The PS3 controller type label comes from the compatibility route.
See [v0.22 manager session notes](manager-session-v022.md).

Version 0.21 serialized selected HID teardown and setup on the worker, blocked
read reposts during normal manager cleanup, and added Settings disconnect intent
handling. The user reports improvement but persistent native connected state.
See [v0.21 session notes](session-v021.md).

Version 0.20 follows the report that swapping modes leaves the controller
acting connected, with no input and no successful reconnect. Inspection found
missing kernel reboot exports: the outgoing PRX did not explicitly retire its
reads, hooks or sampler handler. Cooperative before/phase cleanup is now
implemented and checked in emulation, including ownership and timeout cases.
Physical transition behavior remains unconfirmed. See
[v0.20 handoff notes](handoff-v020.md).

Version 0.19 addresses the user's more specific report: input works in both
modes but does not survive the transition, and an already connected controller
cannot be adopted when the PRX starts. It caches previously classified peer
addresses for passive paired-slot matching, starts a provider-retry worker
independently of installation, and adds bounded silent-stream recovery. Actual
captured XMB/game controller code passes reports through standard user reads
in emulation. Physical transition reliability remains unconfirmed. See
[v0.19 notes](reconnect-v019.md).

Version 0.18 follows the user's confirmation that v0.17 still works only after
fresh pairing. It forces generic local HID setup for selected DualSense devices,
removes the input worker's remaining remote DID query, recovers surviving/stopped
local input contexts, and preserves Bluetooth links after local setup failures.
See [v0.18 reconnect notes](reconnect-v018.md) for evidence and validation limits.
Hardware confirmation is pending; the available local log remains v0.16.

## User hardware and symptom

- The user confirmed working pairing, connection and controls with v0.13.
  Version 0.14 retains that path and adds release/debug build profiles.
  Release defaults to no logging or executable dumps; debug uses
  `PSPGOBT_DEBUG=1`. Earlier observations below document bring-up history.
- The user subsequently reported a connected controller without input after
  reconnect/reboot until re-pairing. Version 0.15 adds native setup-event
  scheduling, startup/link discovery and per-session handler restoration.
  See [reconnect notes](reconnect-v015.md). Hardware verification is pending.
- Version 0.15 did not resolve the user's stored-device reconnect failure.
  Version 0.16 adds debug-only stored-record classification and passive
  session snapshots. It retains v0.15's transport/selection behavior pending
  a current hardware trace; the local log is still from v0.12. See
  [diagnostic instructions](reconnect-debug-v016.md).
- PSP Go with ARK-5. The first probe capture confirms Sony firmware 6.60 and
  hardware model 4 (05g). Exact ARK build remains unknown.
- Before version 0.4, the DualSense appeared in a scan but registration
  reported "device not supported." With the routing patch, the user reports
  successful registration as a DualShock 3, while the DualSense keeps
  flashing. The version 0.4 trace now confirms a native HID connection attempt
  returning `0x802F0140`. Authenticated pairing, an active link, and input are
  unverified.
- Version 0.5's full-record registration returns `0x802F0134` in the supplied
  log. The firmware maps BSM status `0x46` to that code. Version 0.6 corrects
  the empty pairing parameter to the normal XMB constructor's default and
  adds command-level status tracing. Its supplied log confirms successful
  registration and connection. The user reports stable controller lights and
  the PSP showing Connected.
- Version 0.7 installs receive hooks but records no forwarded input. Version
  0.8 reaches the connection worker and records `flags=0202`, with numeric
  identity query failure `0x802F0153`. Version 0.9 tests a bounded selection
  fallback for this observed error; actual input remains unverified.
- Version 0.9's new log confirms `selection=2` and a 79-byte initialization
  write returning `0x4F`, with receive generation still `0`. Version 0.10
  tests callback attachment to an already pending native read when all three
  ownership checks match. Actual input remains unverified.
- Version 0.11 confirms the discovery bypass runs, followed by receive
  failure `0x80300009` and no decoded input. Version 0.12 tests 128-byte
  staging, receive-before-initialization ordering, and the complete compiled
  report-to-transfer-handler path. The transport error's meaning and hardware
  input remain unverified. See [version 0.11 analysis](capture-v11-analysis.md).
- Version 0.12 records overlapping native/worker DID queries, worker error
  `0x802F0141`, and a cleared native context before read submission. Version
  0.13 skips both remote queries for prepared DualSense startup and fixes the
  actual `sceUSB_Host_Driver` capture name. See
  [version 0.12 analysis](capture-v12-analysis.md). Physical input is unverified.
- No physical PSP is connected to the development environment. Build success
  and synthetic protocol tests cannot establish hardware compatibility.

## References

- [btfree, JJS/Yoti, PSP Archive](https://github.com/PSP-Archive/btfree),
  `main.c`: published XMB filter patch and module name. It changes the class
  acceptance predicate; it does not implement a new HID driver.
  The probe uses only the two instruction substitutions, with exact checks.
  It does not port the old callback hook, syscall trampoline, or its stale
  6.20 original-function offset.
- [ds34vita, MERLev/xerpi/Hydr8gon](https://github.com/MERLev/ds34vita),
  revision `a82f79f6f2bdd43bcf3ce01a8f431d45eb5b4b30`, `main.c`: DualSense
  full-report layout, CRC framing, and Vita HID event/transfer architecture.
  The Vita implementation depends on its own Bluetooth request allocator,
  `ksceBtHidTransfer`, controller hooks, and taiHEN.
- [Linux hid-playstation](https://github.com/torvalds/linux/blob/master/drivers/hid/hid-playstation.c):
  authoritative Sony driver implementation of the 78-byte Bluetooth reports,
  full-mode common payload, button layout, CRC prefixes, output tag, and LED
  fields. The portable implementation here was written from those protocol
  facts; it does not incorporate the Linux driver.
- [SDL's DualSense HIDAPI driver](https://github.com/libsdl-org/SDL/blob/main/src/joystick/hidapi/SDL_hidapi_ps5.c):
  basic Bluetooth report layout and 10-byte report dispatch. The basic decoder
  uses those protocol facts with this project's existing normalized mapping.
- [ARK-5](https://github.com/PSP-Arkfive/ARK-5),
  `SystemControl/src/sctrl_hen.c`, `SystemControl/src/plugin.c`,
  `XMBControl/src/plugins.c`: chained module handler and Go plugin config path.
- [ARK-4 WPA2 patch](https://github.com/PSP-Archive/ARK-4/blob/main/core/compat/psp/wpa2.c):
  the desired PRX architecture has precedent, but WPA2 modifies protocol
  handling as well as capability reporting. Its offsets and Wi-Fi assumptions
  do not transfer to Bluetooth.
- [PSPLibDoc](https://github.com/pspdev/psplibdoc), `kd/input_05g.csv` and
  `kd/hidsvc.csv`: real exported NIDs, predominantly unnamed. An exported NID
  alone gives no argument signature, callback contract, or buffer layout.
- [uOFW controller header](https://github.com/uofw/uofw/blob/master/include/ctrl.h)
  and [controller reconstruction](https://github.com/uofw/uofw/blob/master/src/kd/ctrl/ctrl.c):
  native external-input ports and transfer-handler research leads.
- [Sony PSP Bluetooth manual](https://manuals.playstation.net/document/en/psp/current/settings/bl_manage.html):
  stock device-registration UI.
- [PSP-EmulatedControllerTest, Ryan Crosby](https://github.com/crozone/PSP-EmulatedControllerTest),
  revision `b615e12e28a54571f0aea25b9a5d955014cae676`, supplied by the user:
  concrete port-1 handler registration using NID `E467BEC8`, passthrough using
  `6C86AF22`, and analog-to-D-pad validation. This establishes an interface
  for the opt-in native sink. It does not implement Bluetooth registration
  or transport. Its `sizeof(handler)` size convention and reserved-byte
  values are retained; sensors and pressure fields still need validation.

## Confirmed boundaries

Reference checkout revisions used during initial development:

| Repository | Revision |
| --- | --- |
| btfree | `730216b018cdb6351394713e0f59d4c62bfc2039` |
| ARK-5 | `3c024a54840a780cb658194b86941d5774aa029c` |
| ARK-4 | `7c03c3735503dc3fad999f4687b890fefef6467d` |
| PSPLibDoc | `5e15a3cff6c3642f3217b93aeb2f6b759cee1b02` |
| uOFW | `28306bb5bd4fcdcc0c1d469bf1f3bdb885dde3e9` |

`bluetooth_plugin_module` filter sites for the published 6.6x patch:

| Text offset | Expected instruction | Replacement |
| --- | --- | --- |
| `0x13D40` | `0x240E0005`, `li t6, 5` | `0x240EFFFF`, `li t6, -1` |
| `0x13D6C` | `0x104E0003`, `beq v0, t6, +3` | `0x144E0003`, `bne v0, t6, +3` |

The user's first 6.60 firmware capture matches both original instructions.
A mismatch on other images must be investigated, never bypassed by weakening
the instruction guards. See [capture analysis](capture-analysis.md) for the
separate unsupported-device path and next diagnostic build.

The [version 0.2 capture analysis](capture-v02-analysis.md) records the complete
provider captures, successful query tracing, nonzero selected-device mask,
and the reason to trace registration/connection returns in version 0.3.

The [version 0.3 capture analysis](capture-v03-analysis.md) records a positive
address-registration result, the XMB's empty profile-handler rejection, and
the guarded native-HID routing patch introduced in version 0.4.

The [version 0.4 capture analysis](capture-v04-analysis.md) records the confirmed
route hit, asynchronous connection failure, and full-record registration
experiment introduced in version 0.5. Normal Bluetooth pairing/connection and
the native DS3 input adapter are separate parts of the intended architecture.

The [version 0.5 capture analysis](capture-v05-analysis.md) records the failed
full-record procedure, the parameter-default correction, and the guarded
pairing-command traces introduced in version 0.6.

The [version 0.8 capture analysis](capture-v08-analysis.md) covers the successful
link, identity-query blocker, Vita initialization comparison, and input bridge.

The [version 0.9 capture analysis](capture-v09-analysis.md) records the successful
initialization write and the guarded pending-read attachment in version 0.10.

Portable parser contract: 78 bytes starting with `0x31`; axes at offsets 2–7,
buttons at 9–11, CRC at 74–77. Validate CRC over `0xA1` followed by bytes 0–73.
No HIDP transaction byte is present in the passed buffer. Output similarly
excludes HIDP `0xA2` from its buffer but includes it in the CRC calculation.

DS3 wire encoding is an input report with report ID `0x01`, three digital button
bytes at 2–4, sticks at 6–9, and pressures at 14–25. This encoding still needs
validation against the actual PSP input sink. It must not be copied directly
into PSP `SceCtrlData` or used to claim that a DS3 is connected. The native
sink instead populates the 48-byte external-port payload used in crozone's
demo. Its payload timestamp is preserved from the controller driver. A 64-bit
clock prevents low-timer wraparound reviving stale cached input. Host tests
cover those boundaries; physical-device validation is still required.

## Acceptance criteria for a working plugin

- A known DualSense registers through the stock UI and its link persists.
- Input arrives through built-in Bluetooth without an external bridge.
- Both PSP-native controls and external input behave predictably in XMB and
  games, including latch/negative/extended input reads and HOME behavior.
- Controller removal, stale packets, and suspend immediately release input.
- Reconnect, boot, XMB/game/POPS transitions, and other plugins are tested.
- Firmware changes are identified and checked before patching memory.
