# Version 0.2 hardware capture

The second capture is complete: all ten files listed in the log are present
and have their expected sizes, including the Bluetooth manager, its USB
transport, and the native controller driver. The console again reports Sony
6.60 / model 4. The user confirms the DualSense is visible in scan results but
registration fails with a compatibility error; successful registration has
not been established.

## What the live trace establishes

`device-info trace setup result=0` confirms the query hook installed. There
are 17 calls:

- Calls 1–8 return `0x802F0203`. The manager checks its initialization flag
  before this API can run; these early calls do not establish a pairing error.
- Calls 9–16 succeed with a zero field at record offset `0x48`.
- Call 17 succeeds for device identifier 1 with `field48=0402`.

Consequently, the XMB's previously identified **zero-field** rejection would
not fire for the record returned by call 17. The trace does not prove that
authentication succeeded, or that no other unsupported-device path ran.

The manager copies the peer's UTF-16 name into record offset `0x06`.
Version 0.2's `field06=0044` is therefore the first name character, not a
capability flag. Version 0.3 removes that field from logging. The meaning of
every bit in the `0x48` mask is not yet established.

## Registration and connection boundaries

Bluetooth manager base: `0x88238900`. Its error translation table contains
the following entry at absolute address `0x88246488` / text offset `0xDB88`:

```text
BSM status 0x45 -> PSP error 0x802F0135
```

The following entry points were identified from the captured export table
and their code, independently of the older NID database's names:

| User-library NID | Capture address | Observed interface / command path |
| --- | --- | --- |
| `CBD232D1` | `0x8823CD80` | Device identifier + 80-byte output record |
| `8AC2F7B8` | `0x8823D374` | 84-byte scan/registration record + second pointer; calls registration helper at `0x8823F058` |
| `2D649923` | `0x8823D600` | One address pointer; six address bytes are packed into request `0x7E` |
| `F188A405` | `0x88243030` | Device identifier + mode flag; requests connection using `0x202` or `0x402` |
| `1238F36E` | `0x8823E300` | Device identifier + two 16-bit identity outputs + optional fourth pointer |

The scan-registration helper sends command `0x62`, then `0x65`, and may wait
for a completion message. The shared helper at `0x8823C020` sends the request
through `sceUsbBSMCDC_driver` and receives the reply status. Registration
helpers use the manager's translation table to turn BSM statuses into PSP
errors. The captured code establishes that `0x45` can become `0x802F0135`;
the uploaded runtime trace did not record a registration call's return value,
so it does **not** establish that the Bluetooth module actually returned
`0x45` during this attempt.

The connection API's two modes are a concrete research lead. Version 0.3
records the mode argument and preserves it. No mode is forced, and no error
is converted into success.

## Native input interface

Controller driver base: `0x880E6200`. Export `E467BEC8` at text offset
`0x11F0` accepts ports 1 and 2 and retains the supplied handler/source
pointers. The sampler reads the copy callback from handler offset `+4`,
passes the source and destination pointers, and copies 48 bytes into the
controller buffers. Export `6C86AF22` at `0x1AA8` stores the passthrough mask.
This independently supports the native adapter's port, callback, frame size,
and mask choices. The observed registration/sampling path does not require
the stock handler's additional callback. Actual injected input has still
not been tested in the uploaded logs.

## Version 0.3 and the next run

The new probe traces all five APIs above with separate per-API limits. The
new registration, connection, and identity wrappers record entry and return
so a blocked call can be distinguished from a returned rejection. All
arguments and return values are preserved. Every hook requires matching XMB
caller instructions, a resolved import, and matching provider instructions.
The provider checks have been run against the uploaded binary and all five
match. No peer address, PIN, key, full name, or complete runtime record is
logged. The non-secret identity query logs vendor/product numbers if its
original call succeeds.

Replace the PRX, rename `probe.log` to `probe-v02.log` on the console, restart,
and attempt registration once with optional markers absent. For this run,
only the **new `probe.log`** needs to be copied back; the driver binaries
needed for this analysis have already been supplied. All five `bt-api trace
setup` results should be zero. If a provider check fails, its capture can be
requested separately.

Pairing and live Bluetooth input remain unimplemented. A returned failure
from one of these APIs will locate the next patch boundary without assuming
that changing the XMB's displayed error establishes a controller connection.
