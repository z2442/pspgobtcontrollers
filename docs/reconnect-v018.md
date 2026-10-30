# Version 0.18: generic HID reconnect and local input recovery

The user reports that version 0.17 still provides controls only after fresh
pairing. Controller reconnect, reboot or application startup requires deleting
and pairing again. No new trace accompanies this report; the local log is
still v0.16. A DS3-specific Bluetooth reconnect is a hypothesis, not a confirmed
cause. The captured XMB Connect method already passes generic mode 0.

Code inspection found three remaining problems relevant to that report:

1. A checked native input-setup failure calls the Bluetooth disconnect helper,
   including for the selected DualSense. That can tear down a usable link.
2. An existing context with the device slot filled is treated as reusable even
   when its receive request has stopped. A fresh PRX can also inherit Sony's
   unmodified context instead of constructing the selected DualSense read path.
3. The unprepared packet-selection fallback can still issue a remote DID query.
   Prepared state is volatile across power events and new PRX instances.

The new behavior keeps normal Bluetooth pairing and the native DS3 input sink
as separate stages:

- A classified DualSense uses generic mode 0 in the XMB Connect wrapper. The
  original connection result is returned. Unclassified devices keep their mode.
- A guarded hook at BSMan offset `0xA13C` makes its local input constructor use
  generic HID flags for a prepared, connected DualSense. Actual manager link
  flags and stored device capabilities remain unchanged. The DS3-only numeric
  identity/setup branch remains bypassed for that selected context.
- The worker selects input from cached metadata plus the real connected flag,
  without issuing a remote DID query, including when its prepared mask is empty.
- A selected DualSense's local setup failure preserves its Bluetooth link and
  waits at least one second before retrying setup. Other devices retain Sony's
  checked-failure disconnect behavior.
- A previous instance's context, or an owned context without a matching usable
  receive request, is cleaned up and rebuilt. The guarded native helper at
  offset `0xA2DC` unregisters the local service, cancels the CDC read and releases
  queues. It does not send a Bluetooth disconnect command. A foreign callback
  or context owning that CDC channel prevents the reset.
- Reset invalidates old packet generations and releases active input. Sony's
  cancellation callback retains normal buffer cleanup, but its attempted read
  repost is refused during reset. Failed cancellation preserves the outstanding
  staging ownership. Fresh setup gets new aligned 128-byte reads and a fresh
  DualSense initialization report. The bridge does not dereference an old
  context entry to recover it.

Firmware instruction guards match both the normal and relocated game captures.
Host tests reject changed/truncated reset signatures. Compiled MIPS tests cover
generic mode selection with alternate link flags, the actual captured local
cleanup routine, startup with a stale context entry, stopped-read recovery,
foreign-channel refusal, cleanup failure/backoff, preserved Bluetooth links,
and the existing transport, sink, power and release-profile behavior. XMB tests
check that native errors and other devices' mode arguments remain unchanged.

These are implementation and emulation checks. They do not prove that the
physical reconnect failure has been resolved. Keep the existing pairing when
testing controller reconnect, PSP reboot, PSP game startup, and PS1 game startup.
Use the existing `always` plugin entry so the PRX loads in each application.

The supplied build is a release (`PSPGOBT_DEBUG=0`); it creates no logs or dumps.
