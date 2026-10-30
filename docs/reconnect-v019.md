# Version 0.19: attachment to a controller connected before plugin startup

The user confirms that input can work in the XMB and games, but does not
reliably survive the transition. They use an `always` plugin entry. They also
report that a controller already connected before the PRX starts has no input.
The available local trace is still v0.16; it does not establish which boundary
fails in the latest hardware run.

## Known-controller startup

Previous startup always queried Bluetooth metadata before adopting/rebuilding
input, including for the same previously working controller. Those queries use
the manager's serialized command transport. A new PRX also loses the prepared
device mask. Version 0.19 retains the addresses of up to two peers selected by
the existing DualSense name/capability checks.

`controllers.cfg` is a 32-byte configuration file with a format/version header,
two six-byte addresses and a checksum. It contains no pairing secrets, names,
reports or firmware data. It is read before the worker starts and updated only
when a new peer is selected. Encoding is bounded and corrupt/partial files are
ignored. File writes run on the worker, outside interrupts and sampling; failed
or short writes remain pending with a one-second retry interval.

For an already known address, startup compares the current native paired-slot
ID, `0x0402` capability word and address against this cache. Instruction guards
verify the paired-table layout against the normal and relocated captures.
There is no new metadata command on this path, and no fabricated public device
record. A changed, removed or incompatible paired slot cannot use the cached
route. Input still requires the real connected flag and valid DualSense frames.
Unknown peers use the existing metadata query and must pass its classification.

Connect once after this version starts to populate the cache, retaining the
existing pairing. Thereafter an application can load a fresh PRX and attach to
the already-open link. Version 0.18's guarded local context reset, generic HID
setup, descriptor bypass and fresh 128-byte receive staging remain in use.

## Startup and silent reads

The loader callback is installed before the initial inventory, with serialized
UID deduplication. A resident retry worker starts even when providers are absent
or the first installation attempt fails. Later module availability can therefore
install the bridge without rebooting or creating another worker/power handler.

A pending CDC callback alone no longer counts as evidence of a working stream.
After two seconds with no valid report, a selected connected peer can receive
up to three local input resets. These reuse the guarded cancel/setup path and
preserve the Bluetooth connection. Fresh valid input resets the recovery budget;
disconnect, suspend and provider replacement reset session timers. Short or zero
initialization writes no longer count as a completed 79-byte output report.

## Validation and hardware procedure

Host tests cover the cache format, replacement, invalid addresses, truncated
files and corruption. Compiled MIPS tests cover known-peer attachment without
metadata calls, a context from a previous application, changed/removed pairings,
bounded silent-stream recovery, healthy input and power resets, late providers,
the full installer for both captured module layouts, and exact configuration
write size/error handling. Captured controller-code tests run the compiled sink
through Sony's real sampler and positive/negative user-mode reads, checking
buttons, analog input and stale release in both the XMB and game layouts.

These checks pass locally but do not prove hardware transition reliability.
The tests use local Sony captures when present; no captures are distributed.
The release keeps logs and firmware dumps compiled out. Its only new persistent
output is the small controller configuration file.

1. Install the release, keep the `always` entry, and start with the controller off.
2. Connect the existing paired controller after the XMB loads. Confirm input,
   then wait two seconds for the configuration write. No re-pairing is required.
3. Launch a game while still connected, return to the XMB, and try both directions.
4. Test a reboot/reconnect after the cache has been learned. Report whether the
   controller stays connected and whether input resumes in each mode.
