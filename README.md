# OpenStick CH347 display-output package

`org.msys.openstick.ch347` is the installable owner of the OpenStick CH347 +
ST7796 X11 output and XPT2046 touch pipeline. It preserves the development
component identity `org.msys.openstick.ch347:x11-spi-touch-output`, so an
installed version atomically replaces the Core fallback without changing any
profile, role, Shell, or application manifest.

The immutable package contains the aarch64 capture/sink executables, CH347
userspace library, Xorg configuration, default calibration, start/stop scripts,
and display-session state publisher.
Runtime resolution starts at
`MSYS_PACKAGE_ROOT/files/x11display`; `/root/x11display` is only a source-tree
development fallback and is not used by an installed package.

Provider and detached display-daemon processes share an atomic generation
ownership token. A delayed cleanup from an older `msysd` generation can stop
only its own processes; it cannot remove the active generation's pid file or
rebind the active CH347 USB interface.

If a loose connector leaves CH347 below its required 480 Mbit/s speed for the
full transport wait window, the daemon publishes one atomic `degraded` edge.
The managed provider turns that edge into one localized MSYS notification and
rate-limits repeated failures to one per minute. A later 480 Mbit/s edge emits
one recovery notification. This path keeps Xorg `:24` and every X11 client
alive; it neither reopens applications nor depends on D-Bus. Standalone
`/root/x11display` exposes the same state in
`/tmp/ch347_dirty_usb_x11/ch347-link-state.env` for an optional supervisor.

Platform contracts remain explicit: the board supplies Linux USB access, Bash,
Xorg plus its X11/XDamage libraries, and the CH347 device. No target package
manager, systemd, D-Bus, logind, or Python installation is invoked. The normal
MSYS isolated runtime supplies Python to publish the typed display-session
document.
The `display-output` provide explicitly claims the stable
`org.msys.role.display-output.v1@1.0.0` descriptor.

Font rendering remains a session/runtime concern (Fontconfig, FreeType and
Xft/XRender), not a display transport concern. The rejected BDF experiment is
explicitly excluded from the MAF, so this driver only moves completed pixels
and touch events.

Physical rotation is owned here as a display-output capability. The provider
reads the small package-state `rotation.env`; `normal`, `right`, `inverted`,
and `left` select the logical X11 geometry, rotate RGB565 frames into the fixed
320x480 panel, and apply the exact inverse mapping to touch coordinates. HAL
commits that file and signals the active provider generation. The provider
changes the existing RandR root and republishes the same-generation display
session; Xorg, Shell and application clients remain connected throughout.

Display tuning is package-state, not immutable-manifest state. The provider
strictly parses `${MSYS_APP_STATE_DIR}/ch347/fps.env` as `DEBUG`, `FPS`,
`XCAP_MAX_FPS`, and `XCAP_IDLE_FPS`, rejects shell syntax, duplicates, and
out-of-range values, and makes it authoritative over manifest defaults on
every managed start. It publishes a generation-bound runtime receipt only
after X11 is ready, allowing HAL and Settings to distinguish a saved debug
overlay switch from one applied to the live sink. Direct development can still
override these values explicitly, including
`CH347_TOUCH=1 DEBUG=1 FPS=60 .../start_ch347_dirty_usb_x11.sh`.

The stable SPI policy uses one merged dirty bounding box
(`CH347_MAX_RECTS=1`). Explicit development environments may override it, but
the manifest, vendor defaults, and both launcher layers do not silently enable
the slow multi-rectangle path.

Version 0.1.13 keeps that stable policy unchanged and adds low-frequency
`dirty_stats` records from the sink.  They distinguish exact transmitted
pixels, zero-damage passes, large refreshes, and true full-panel refreshes even
when the visible debug overlay is disabled.  This lets Settings and one-pass
acceptance inspect SPI behaviour without creating extra display damage.

Version 0.1.16 restores the stablev1 bus-paced mailbox contract.  While a slow
SPI rectangle is in flight, capture retains at most one complete pending frame;
once that slot is occupied, `consumed_seq` paces further capture until the sink
finishes the current transfer.  Capture therefore does not continually
overwrite pending drag positions or deliberately skip intermediate drag
frames.  The stable single-bbox dirty calculation remains unchanged, and one
frame's rectangle is still fully reaped before another frame starts.

Version 0.1.17 makes the effective dirty policy explicit in diagnostics. With
the stable `max_rects=1, stale_ms=0` profile, the direct single-bbox path is in
use and the configured 40% fallback is reported as inactive.

Version 0.1.18 separates detailed sink logging from the optional on-panel
diagnostic overlay. The overlay is off by default and has independent alpha,
compact font scale, selected metric rows, and a bounded sample interval. It
uses the existing single bounding-box renderer without changing its damage
selection policy.

Version 0.1.19 keeps an enabled overlay live while full-frame mailbox input is
idle. Its bounded timer sends only the overlay rectangle; the disabled default
adds no timer or damage. Rect-protocol overlay updates are folded into the
existing damage as one bounding box when `max_rects=1`; the stable dirty-bbox
implementation and defaults remain unchanged.

Version 0.1.20 disables the LCD-side touch cursor by default at every packaged
launch layer. Touch movement therefore injects input without manufacturing
framebuffer damage; `CH347_CURSOR=1` remains an explicit calibration/debug
override.

Version 0.1.21 provisions that opt-in as a strict mutable `cursor.env`
document. The provider exports its persisted value before launch and publishes
a generation-bound applied receipt before READY, so HAL and Settings never
confuse a saved value with the value running in the sink. Older package
versions safely ignore the extra state file and retain their cursor-off
default during rollback.

Version 0.1.26 applies FPS, sink logging, debug overlay, touch cursor and
physical rotation through the active provider/daemon `SIGUSR1` chain. Rotation
uses RandR on the existing `:24` root and reloads capture/touch mapping in
place; Xorg, Shell and application processes keep their PIDs. The stablev1
single-bbox dirty renderer is unchanged. Its signal-safe child wait also keeps
the daemon alive when Bash unsets the `wait -p` result during a control signal.

A CH347 interface enumerated at 12M is treated as a loose/degraded physical
link, never as a usable display transport.  Recovery now issues a device-only
`USBDEVFS_RESET` (with a bounded authorization fallback), waits for the same
interface to return at 480M, and leaves Xorg `:24` plus its applications alive
throughout the transport repair.

CH347 cable/USB loss is isolated from the X11 session. The detached daemon
keeps the original Xorg `:24` process and all connected applications alive,
then recreates only `xdamage_shm_capture` and `ch347_dirty_usb_sink` until the
transport returns (`CH347_RESTART_MAX=0`). A real Xorg exit or an unreachable
`:24` is a different failure domain: the daemon records `x-session-lost` and
exits with its dedicated fatal status; it never starts a replacement Xorg
behind Core's back.

Build and strictly validate a MAF on the Windows/WSL workstation:

```powershell
wsl env PYTHONPATH=/mnt/g/Code/MsYs/msys-tools:/mnt/g/Code/MsYs/msys-install:/mnt/g/Code/MsYs/msys-sdk `
  python3 -m msys_tools.dev package build /mnt/g/Code/MsYs/msys-openstick-ch347 `
  --format maf --output /mnt/g/Code/MsYs/dist --force
```

The three ELF files and `libch347spi.so` must be target-built AArch64 artifacts.
`msys-tools sync-x11display` remains the development builder; a release refresh
copies its verified target outputs into this repository before producing a new
driver version.

## One-command display recovery acceptance

`tools/display_recovery_acceptance.py` replaces the manual sequence of SSH,
`ps`, `kill`, log-tail, and PID comparisons. It runs locally on the board and
prints one `msys.display-recovery-acceptance.v1` JSON document. The document
records Xorg, capture, sink, daemon, system-UI, and running manual-application
identities as both PID and kernel start time, so PID reuse cannot produce a
false pass.

Inspection is the default and never sends a signal:

```sh
/opt/msys-dev/.runtime/python/bin/python3 \
  /opt/msys-dev/msys-openstick-ch347/tools/display_recovery_acceptance.py
```

The result includes `ready_for.sink` and `ready_for.xorg`. The Xorg acceptance
requires at least one running manual X11 application so it can prove the
application was dropped and was not deceptively reopened. Start Settings or
Calculator first if `ready_for.xorg` is false.

Fault injection is available only through an explicit `--inject` value and
must run as root. `all` first validates the cheaper stream fault and stops
without killing Xorg if that level fails:

```sh
/opt/msys-dev/.runtime/python/bin/python3 \
  /opt/msys-dev/msys-openstick-ch347/tools/display_recovery_acceptance.py \
  --inject all --timeout 45 \
  --output /tmp/display-recovery-acceptance.json
```

The sink case passes only when capture and sink receive new identities while
Xorg `:24`, the provider daemon, system UI, and every manual app retain their
exact identities. It also rejects any full-session recovery log in that time
window. The Xorg case passes only when Xorg/provider/system UI are recreated,
all previously running manual X11 apps remain absent, Core logs both a real
`display-session-lost` fault and `applications_reopened=false`, and the native
Shell notification window is observed while visible. `xwininfo` is therefore
a strict preflight dependency only for Xorg injection. A failed preflight is
read-only: no process is killed.

Custom runtime paths remain explicit for nonstandard development starts:

```sh
python3 tools/display_recovery_acceptance.py \
  --runtime-dir /tmp/msys-main \
  --run-dir /tmp/ch347_dirty_usb_x11 \
  --log-file /tmp/msysd.log --display :24
```
