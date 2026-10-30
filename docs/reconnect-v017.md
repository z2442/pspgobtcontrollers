# Version 0.17: application startup and stored-controller reconnect

The new v0.16 log contains three sessions. One forwards 3,967 valid input
reports with no rejected frames. Another loads the same Bluetooth firmware
at different addresses during application startup, where address-specific
guards reject installation. The final session logs connection flags but
does not return from its device-info query. These observations show multiple
failure paths; they do not prove that a passthrough reset caused every failure.

The user also reports that popsman can clear `sceCtrl_driver_6C86AF22` after
plugin initialization. Version 0.17 addresses delayed resets in every mode:

- While accepted controller input remains fresh, the transport worker resolves
  the controller APIs, re-registers the DS3 port-1 transfer handler, and calls
  `sceCtrl_driver_6C86AF22(1)` every 100 ms. Initial session registration still
  happens immediately. No maintenance runs inside the sampling callback;
  stale, disconnected or suspended input cannot initiate maintenance.
- Firmware guards derive embedded addresses from each module's actual load
  address, retaining the original instruction, delay-slot and target checks.
  The normal and relocated application captures both pass these checks.
- Internal metadata queries issue the native serialized `0x63` cached-record
  command directly. The public device-info API can issue a `0x64` remote
  refresh and wait first. The bridge needs only cached name/capabilities.
  Public APIs, pairing records and discovery for other devices are unchanged.
  Command failures, nonzero status and mismatched nonempty slots are rejected.
- If Bluetooth firmware is replaced while the plugin remains resident, its
  worker clears old sessions and rebinds the new image after verifying it.
  It reuses its thread and power handler. Application launches that reload
  the PRX initialize a fresh bridge from the current modules.

Use the `always` entry from `PLUGINS.TXT.example`, replacing the earlier VSH-only
entry for this PRX. The plugin must actually load in GAME/POPS to maintain their
native input routes. Keep the controller paired when testing reboot,
disconnect/reconnect, suspend/resume, PSP game startup, and PS1 game startup.

Validation covers host protocol/guard tests, compiled MIPS transport and sink
tests, delayed driver resets, cached-record command framing/error handling,
provider-replacement orchestration, and release-profile checks. It does not
substitute for testing these transitions on a PSP Go. Version 0.17 has not yet
been confirmed on hardware.

The supplied release uses `PSPGOBT_DEBUG=0`: logging, module inventories and
firmware dumps are compiled out. Captures are not included in the package.
