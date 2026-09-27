# Ciel-MountD

## Put the file 'interface/org.ciel.Mount.conf' in '/usr/share/dbus-1/system.d/' so that `dbus` registers it

<!-- A minimal, dependency-light daemon that watches udev for removable USB/block
devices and auto-mounts them — a starting point for something that can
eventually replace udisks2's + gvfs's auto-mount behavior.

## Status

- **Block devices (USB sticks, SD cards, external drives): working.**
  Detects add/remove, mounts to `/media/<label-or-uuid-or-devname>`,
  cleans up on removal and on daemon shutdown.
- **MTP (phones/cameras): detection only, not yet mounted.** See
  "MTP: why it's a stub" below — this needs a different subsystem
  entirely (libmtp + FUSE), not `mount(2)`.

## Layout

```c
udev_monitor.h     detection layer (libudev)
include/          headers (the public API of each module)
  device.h          shared device_info_t struct
  mount_manager.h    mounting layer (mount(2)/umount2(2))
  log.h              syslog wrapper
  util.h             safe_copy() helper
  config.h           tunables (mount base dir, uid/gid, ...)
src/               implementations, same split as above, plus main.c
systemd/ciel-mountd.service
Makefile
```

`main.c` is the only file that knows about both halves — `udev_monitor.c`
doesn't know mounting exists, and `mount_manager.c` doesn't know udev
exists. If you want to swap in a different detection mechanism, or a
different mount backend (e.g. libmount, or shelling out to udisksctl),
you only need to touch one file plus its header.

## Build & run

```sh
sudo apt install build-essential pkg-config libudev-dev   # Debian/Ubuntu
make
sudo ./bin/ciel-mountd -f      # foreground, logs to stderr too — good for testing
```

Plug in a USB stick and watch it get mounted under `/media/`. `Ctrl-C`
unmounts everything it mounted before exiting.

## Installing as a system service

```sh
sudo make install
sudo systemctl daemon-reload
sudo systemctl enable --now ciel-mountd
journalctl -u ciel-mountd -f
```

### Actually replacing udisks2/gvfs

Both udisks2 and gvfs (via `gvfs-udisks2-volume-monitor`) will race this
daemon to auto-mount the same devices if left running. To fully hand
mounting duty over to ciel-mountd:

```sh
systemctl mask udisks2.service
# gvfs's auto-mounter runs per-session as a user service/D-Bus activated
# process, not a system daemon, so it's stopped per-desktop-session
# rather than via systemctl — e.g. in GNOME/GTK-based desktops:
gsettings set org.gnome.desktop.media-handling automount false
gsettings set org.gnome.desktop.media-handling automount-open false
```

There's also a commented-out `Conflicts=udisks2.service` in
`systemd/ciel-mountd.service` you can enable once you're ready.

## Known limitations / where to go next

These are the things a "start it" skeleton deliberately leaves for you
to extend, roughly in the order I'd tackle them:

1. **Per-user mount ownership.** `MOUNT_UID`/`MOUNT_GID` in `config.h`
   are hardcoded (default 1000). A proper replacement for udisks2 would
   ask `logind` (via `libsystemd`'s `sd-login.h`, e.g.
   `sd_seat_get_active()` / `sd_uid_get_display()`) which user is active
   at the seat and mount with _their_ uid/gid — important on
   multi-user or multi-seat machines.

2. **MTP: why it's a stub.** MTP/PTP devices (Android phones, many
   cameras) don't show up as block devices at all — there's no
   filesystem for the kernel to mount with `mount(2)`. They speak a
   USB protocol (Media/Picture Transfer Protocol) that has to be
   implemented in userspace and exposed as a FUSE filesystem — that's
   what tools like `jmtpfs`/`simple-mtpfs`/gvfs's own MTP backend
   actually do, using `libmtp`. `udev_monitor.c` already detects MTP
   devices (via the `ID_MTP_DEVICE` udev property set by `mtp-probe`)
   and logs them; `mount_manager_handle_add()` has the obvious place to
   call out to `libmtp` and spawn a FUSE mount once you're ready to
   implement that half.

3. **No PolicyKit-style authorization.** udisks2 lets _unprivileged_
   users request mounts of their own removable media via D-Bus +
   PolicyKit. This daemon does everything itself as root in response to
   kernel events — simpler, but means there's no user-facing API yet
   (no "mount this specific device on demand" from a file manager, for
   instance). If you want that, the natural next step is exposing a
   small D-Bus service from this daemon.

4. **Filesystem type allow-list.** `mount_manager.c`'s `build_options()`
   currently special-cases vfat/exfat/ntfs and passes no options for
   everything else. You may want an explicit allow-list of filesystem
   types you're willing to auto-mount untrusted removable media as,
   rather than trusting whatever `ID_FS_TYPE` udev reports (defense in
   depth against a malicious/corrupted filesystem image).

5. **No config file.** Everything tunable lives in `config.h` and is
   baked in at compile time. Swapping that for a runtime config file
   (or command-line flags) is a small, self-contained change.

## Security notes

- Every mount is done with `MS_NOSUID | MS_NODEV`, so removable media
  can't be used to plant a setuid-root binary or a device node. Adjust
  in `mount_manager.c` if you also want `MS_NOEXEC`.
- The daemon needs `CAP_SYS_ADMIN` for `mount(2)`/`umount2(2)`; the
  provided systemd unit runs it as root but with
  `NoNewPrivileges=true`, `ProtectSystem=strict`, and a minimal
  `CapabilityBoundingSet` so it can't do much else as root.
-->
