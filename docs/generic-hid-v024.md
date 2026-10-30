# Version 0.24: generic HID device type and connection methods

**Superseded by [v0.25](hid-mode-v025.md).** Hardware feedback showed an audio
type and unsupported-device error. The v0.24 selector excluded scan type 8,
and the saved-device loader bypassed this registration-only constructor hook.
The tests below did not cover that complete saved-device path.

The DualSense now uses the PSP XMB's generic HID device type, with its HID
Connect/Disconnect methods attached explicitly. The bridge still decodes
DualSense reports and feeds the PSP's external controller input port.

This implements the requested experiment of avoiding PS3 device classification.
It does not establish that a driver conflict caused the reconnect failures;
hardware verification is still needed.

## What the captured XMB actually does

The device-type label dispatcher at `bluetooth_plugin_module + 0x146C4`
selects `msg_ps3_controller` for type 1 and `msg_bt_hid` for type 2. Type 8 is
an audio-device classification; it is not generic HID. Earlier notes describing
type 2 as unknown and type 8 as HID were incorrect.

For the observed DualSense capabilities `0x0402`, the profile constructor at
`+0x14E0C` only attaches HID methods when the device type is 1. That is why
earlier versions changed type 2 to type 1. Keeping type 2 without also supplying
the methods would leave Connect/Disconnect without a HID handler.

The separate native method factory at `+0x14F64` can attach methods independently
of the device-type field. Its method-kind 2 selects the HID vtable at `+0x20C28`.
The vtable's connection methods call `F188A405(slot, 0)` for generic HID Connect
and `68327A27(slot)` for Disconnect. These are also the transport methods Sony
uses for its controller type; the profile enum and label are separate.

The generated user-mode wrapper now:

1. Checks the existing DualSense name, `0x0402` capabilities and type 1 or 2.
2. Keeps/sets type 2 and invokes the native factory with method-kind 2.
3. Returns type 2 to the caller, which stores the constructor result in the
   profile object. It does not re-enter the type-1-dependent constructor.

Other devices continue through the original constructor with its arguments
and return value preserved. The metadata and Bluetooth registration are not
rewritten. Both existing type-1 objects and new type-2 objects are handled when
the XMB constructs them, so an existing pairing can be retained. Returning a
selected result for a type-2 object is necessary: each fresh object still needs
its methods attached even when its type field does not change.

Instruction guards now also verify the method factory's jump-table entry,
allocator call, vtable references and HID method bodies in both captured XMB
relocations before installing this wrapper.

## Input and session handling

Generic Bluetooth HID is the connection route. The plugin continues to send
the DualSense initialization report, read and decode its Bluetooth reports, and
submit controller frames through the PSP's external controller input interface.
The Sony HID descriptor parser is still bypassed for a selected DualSense: it
cannot replace the plugin's DualSense decoder merely by changing a device type.

The v0.23 session correction is included: native state 2 can signal an outgoing
connection failure and no longer queues an extra synchronous disconnect on the
input worker. Matching state-3 indications still update native manager state
through Sony's finalizer, with local read teardown serialized before reuse.
See [session details](session-v023.md).

## Install and test

Install `pspgobt-release-v0.24.zip`, replacing the PRX at
`ef0:/seplugins/pspgobt/pspgobt_probe.prx`. Keep its `always` entry, the existing
pairing and `controllers.cfg`. Fully power off the PSP and controller once to
start a fresh plugin instance and retire any previously blocked worker.

Check that Manage Bluetooth Devices now displays the generic HID type rather
than PS3 controller. The exact translated text depends on the XMB language.
Test Settings Disconnect followed by Connect, verify input after reconnect,
then launch and exit a game while connected. Also test controller power-off/on
and a PSP reboot using the saved pairing.

The release has both diagnostic flags disabled. If a failure remains, the
optional `pspgobt-session-trace-v0.24.zip` records a bounded `session.log` without
firmware dumps; follow the [trace procedure](session-v023.md#optional-session-trace-separate-from-the-release)
using the v0.24 archive. Its BOOT version field is 24 decimal (`0x18`). Restore
the release afterward.

## Validation

Host protocol/guard tests and MIPS wrapper tests pass. Tests execute the actual
captured native method factory, label dispatcher and HID Connect/Disconnect
methods for both available XMB relocations. They verify generic HID type 2,
the `msg_bt_hid` label key, generic Connect mode 0, preserved error results,
slot/metadata/stack preservation, and refusal to install with changed signatures.
Existing bridge, controller sink, reconnect, native manager and shutdown checks
exercise the compiled release separately. Radio behavior and game transitions
still need the physical PSP test above.
