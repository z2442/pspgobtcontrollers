# Version 0.5 registration failure and version 0.6 experiment

The supplied log identifies probe 0.5, Sony firmware `0x06060010`, and model
`4`. Both XMB route patches installed and the DualSense record matched:

```text
bt-pair-route target=DualSense record84=1 parameter=empty class=082500; using native full-record registration
bt-api begin op=register-scan event=1
bt-api end op=register-scan event=1 result=802F0134
bt-pair-route completed result=802F0134; native result preserved
```

No HID connection follows this failed registration. The early query errors
`0x802F0203` occurred before Bluetooth manager initialization, as in earlier
captures, and are a separate observation.

## Meaning of the error

The captured Bluetooth manager's translation table contains:

| BSM status | PSP error | Table entry text offset |
| --- | --- | --- |
| `0x46` | `0x802F0134` | `0xDB90` |

Thus the full-record procedure returned an error corresponding to BSM status
`0x46`. The log does not establish which command or completion produced it.
No verified symbolic meaning for `0x46` has been found in the supplied code
or references; it must not be labeled an authentication or PIN failure based
only on the numeric value.

## Pairing parameter correction

Version 0.5 allocated a zeroed parameter buffer, representing an empty string.
The captured normal XMB constructor initializes four UTF-16 `U+0030` code
units instead:

| XMB text offset | Instruction |
| --- | --- |
| `0x13A5C` | `li v1,48` |
| `0x13A60` | `li v0,3` |
| `0x13A64` | `addiu v0,v0,-1` |
| `0x13A68` | `sh v1,0(s0)` |
| `0x13A6C` | `bgez v0,0x13A64` |
| `0x13A70` | `addiu s0,s0,2` |

The existing constructor first clears its 32-byte parameter storage, then
this loop writes `0000`. Version 0.6 matches that default in its terminated
user-RAM buffer. The new instruction checks cover this initializer as well
as the registration methods. This corrects a difference from the native
path; it does not prove the parameter difference caused the observed error
or that DualSense supports a legacy PIN-based pairing procedure.

## Command-level tracing

The normal registration helper sends a 34-byte parameter command `0x62`, then
starts registration with command `0x65`. Either immediate response can be
translated through the status table, and the later completion can fail too.
Version 0.6 replaces just these two kernel `jal` targets:

| Bluetooth manager text offset | Purpose |
| --- | --- |
| `0x68A0` | Parameter command through common request helper |
| `0x69F4` | Registration command through common request helper |

The common helper at offset `0x3720` stays intact. Each wrapper logs a begin
line with command, length, expected response, and mode, invokes the original
helper with the same three arguments and K1, then logs its result. When the
helper succeeds, the returned 16-bit status at response-descriptor offset
`+4` is logged. Transport/helper failures report status as unavailable.
No descriptor payload pointer is followed by the trace. Peer addresses,
pairing material, and user PIN contents are not recorded.

The trace caps output at 64 calls per boot. Both call targets, delay slots,
argument setup, command setup, and common helper prologue must match before
either call is patched. All hooks remain resident until restart. A mismatch
refuses tracing rather than altering unknown code. Tracing adds filesystem
I/O around the command calls, so timing still needs hardware validation.

## Validation and next test

Host tests verify the exact UTF-16 default, its terminator and padding,
unchanged bytes past the buffer, truncated-buffer rejection, and mutation
rejection for every new instruction and both call sites. The existing eight
MIPS emulation tests pass. Both updated signatures also match the supplied
6.60 XMB/manager executable captures using their own original bases. The PRX
compiles with warnings treated as errors. Hardware pairing remains unverified.

Install version 0.6, archive the old log as `probe-v05.log`, and restart with
the three optional markers absent. Remove the previous controller entry if
it remains, then attempt registration from a fresh scan with DualSense in
pairing mode. Return the new log, including:

- `bt-pair-command trace setup result=`
- `bt-pair-command begin` / `end`
- `bt-pair-route` and `register-scan` results
- Any subsequent `hid-connect` result

If `0x62` fails, investigate parameter acceptance before attempting HID
connection. If `0x62` succeeds and `0x65` fails, investigate the registration
request and response. If both are accepted and the API later fails, inspect
the asynchronous completion/authentication path. The DS3 input adapter is
separate and remains unused until report transport is implemented.
