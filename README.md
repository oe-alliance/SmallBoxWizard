# OE-Alliance SmallBox Wizard

SmallBox Wizard is a native C first-boot environment for Enigma2 receivers
with limited internal flash and RAM. It runs directly on the Linux framebuffer
before Enigma2, Python, or the normal GUI stack is started.

The current OpenEmbedded integration enables this native first-boot path only
for OpenATV. Other OE-Alliance distributions retain their existing SmallFlash
image composition and first-boot workflow.

The wizard is controlled with a standard receiver remote through Linux evdev.
It uses the common navigation keys (`UP`, `DOWN`, `LEFT`, `RIGHT`, `OK`, and
`BACK`) and contains no receiver-specific remote-control mapping. All text shown
on the television is English.

Setup is mandatory. Enigma2 remains blocked until the wizard has completed and
`/etc/smallbox-wizard.done` exists. Before launching the wizard, the SysV init
script brings Chrony online and requests an immediate clock step. It runs the
wizard before Enigma2 and restarts it after an unexpected exit. There is no
option to skip first-boot setup.

## Installation modes

The available mode is selected from OpenEmbedded machine metadata.

### FlashExpander

FlashExpander keeps the internal root filesystem and moves `/usr` to USB:

1. Detect safe whole USB `sdX` devices through sysfs.
2. Warn that all data will be destroyed and require two confirmations.
3. Create an ext4 data partition and an exact 512 MiB swap partition.
4. Activate swap before copying `/usr` or running OPKG.
5. Copy `/usr` to `/.FlashExpander/.FlashExpander` with percentage, MiB,
   elapsed-time, and remaining-time progress.
6. Add a UUID-based, marked block to `/etc/fstab` and bind-mount the copied
   directory as `/usr` immediately.
7. Reuse an active wired network or configure a wired interface with DHCP.
8. Synchronize the clock before the first HTTPS request.
9. Install `packagegroup-openatv-small` and its dependency plan from the feed.
10. Apply the Single-Core boot profile when required, write the completion
    marker, and reboot.

OPKG cache files, package lists, temporary files, and the complete install log
are stored on USB under `/.FlashExpander/.smallbox-opkg`. They do not consume
the remaining internal flash.

### Chkroot Multiboot

Chkroot keeps only the bootstrap and shared kernel in internal flash. The full
root filesystem is installed on USB:

1. Create a GPT layout containing a FAT32 `STARTUP` partition, one
   legacy-compatible ext4 rootfs partition, and an exact 512 MiB swap
   partition.
2. Activate swap before downloading or unpacking the image.
3. Create and verify `STARTUP_FLASH` and the available `STARTUP_<slot>` files.
4. Mount the rootfs partition persistently as `/media/<device>`.
5. Download the server-pinned `_multiboot.zip` with MiB, percentage, and
   elapsed-time progress.
6. Install rootfs-only into slot 1 with `ofgwrite`. The internal kernel is not
   flashed.
7. Write UUID-based target `fstab` entries and the completion marker into the
   installed root filesystem, then reboot into Chkroot.

The wizard verifies that it is running from internal flash before installing a
Chkroot image. `ofgwrite` receives exclusive framebuffer ownership while it is
running; the wizard clears the screen first and redraws it afterwards. Combined
stdout and stderr are saved as `smallbox/ofgwrite.log` on the USB device and the
last relevant output is included in an error screen.

Chkroot policy:

- `smallflash` + `chkrootmb` + an OE `FLASHSIZE` budget from 1 through 256 MiB:
  Chkroot is available.
- `smallbox-chkroot-required`: Chkroot is mandatory and FlashExpander is not
  offered. This currently applies to `dm500hd` and `dm800se`.
- 512 MiB or larger: Chkroot is not offered by SmallBox Wizard.
- A SmallBox receiver without `chkrootmb` uses FlashExpander only.

`FLASHSIZE` is the OpenEmbedded image budget. It is not assumed to be the
physical NAND size.

## Multiboot image feed

The default endpoint is generated per machine:

```text
https://images.mynonpublic.com/openatv/wizard.php?open=<machinebuild>
```

`wizard.php` pins the release and image chosen by the server administrator. The
wizard does not scan release directories. A direct `image_url` can be supplied
instead of the JSON feed.

Feed entries use `link`, `date`, and `size` fields. The selected link must
contain the configured machine build and the configured `_multiboot.zip` match
string. The size is used for download progress.

```json
{
  "images": [
    {
      "link": "https://example.invalid/oe-alliance-vusolo_multiboot.zip",
      "date": 20260827,
      "size": 100259696
    }
  ]
}
```

The distributable archive contains only the rootfs transport image and version
metadata:

```text
smallbox/<machinebuild>/rootfs.tar.bz2
smallbox/<machinebuild>/imageversion
```

It is not a native USB flash image and contains no kernel. No SHA256 sidecar is
downloaded or required.

## OPKG recovery

The normal package installation is one fast bulk operation. Progress includes
the feed stage, total package count, installed count, remaining count, current
package, and elapsed time.

If OPKG fails, the wizard refreshes the installed-package state once and shows:

- the exact failed package when it can be identified;
- the last relevant OPKG error;
- how many packages are installed and still missing.

The user can retry the remaining plan, skip the failed package and its unresolved
dependency chain, or abort. Recovery still installs all remaining packages in
one bulk operation; it does not fall back to slow package-by-package installs.
Skipped packages are stored in `/etc/smallbox-wizard.skipped-packages` and are
honored after a reboot.

The active FlashExpander remains mounted after a package error. On the next boot
the wizard resumes at network and package setup. The recovery screen also offers
`START OVER WITH USB SETUP`, which safely disconnects the marked `/usr` and swap
mounts before returning to USB selection. No device is erased until it is
selected and confirmed again.

## Single-Core boot profile

Single-Core receivers keep optional network-server packages installed, but
fresh images do not start the following services automatically:

```text
autofs
avahi-daemon
llmnrd
nfsserver
samba
smartd
telnetd.busybox
vsftpd
wsdd
wsdd2
```

Only SysV start links in runlevels 2 through 5 are removed. Enigma2's Network
Services screen can enable the services later with `update-rc.d`. Dropbear,
Chrony, and Cron remain enabled.

The image post-process hook applies this policy to normal and full Chkroot
images. FlashExpander packages are installed after image creation, so the native
wizard applies the same policy immediately after OPKG and before writing its
completion marker.

## Safety properties

- Only whole `sdX` devices whose sysfs path resolves through USB are shown.
- Both `/sys/class/block` and `/sys/block` layouts are supported for new and
  old receiver kernels.
- Devices backing `/`, `/usr`, or `/boot` are excluded.
- Device identity and size are checked again immediately before partitioning.
- OpenATV `/dev/nomount.<device>` guards prevent the automounter from mounting
  a selected disk or its new partitions during setup.
- The destination filesystem remains the process working directory while data
  is copied, preventing an old automounter from redirecting writes into flash.
- `/etc/fstab` is updated only after formatting, copying, and UUID discovery
  succeed. Changes are committed with `fsync` and atomic `rename`.
- The selected block-device path cannot be entered manually.
- The USB device must remain connected after confirmation and while the box is
  running from FlashExpander or Chkroot.

## OpenATV build integration

OpenATV SmallBox and Single-Core selection use these machine features:

| Feature | Effect |
|---|---|
| `smallflash` | Build the internal bootstrap image and include the native wizard. |
| `chkrootmb` | Allow the shared-kernel Chkroot option when the size policy permits it. |
| `smallbox-chkroot-required` | Disable FlashExpander and require Chkroot. |
| `singlecore` | Record the capability in `enigma.info` and apply the optional-service boot profile. |

An OpenATV SmallBox build compiles packages once and produces two independent
deliverable archives when Chkroot is enabled:

- the normal machine-native bootstrap flash archive;
- one rootfs-only `_multiboot.zip` for Chkroot.

The bootstrap contains the C wizard but no Enigma2, Python, GUI, or multimedia
stack. The full Chkroot rootfs contains Enigma2, all SmallBox add-ons, and
`ofgwrite`, but not the C wizard. Intermediate rootfs tar files and misleading
machine-native full-image USB archives are not published.

The current OpenATV policy audit covers 41 `smallflash` machines, 43
`singlecore` machines, and 25 machines with both features. See
[`docs/machine-matrix.md`](docs/machine-matrix.md) for the complete list,
effective image budgets, Chkroot policy, native flash formats, and documented
exceptions.

## Building the program

The native executable requires only libc and Linux kernel headers:

```sh
make
```

The OpenEmbedded recipe supplies the cross compiler, flags, runtime tools, init
script, and generated `/etc/smallbox-wizard.conf`.

Useful diagnostic commands:

```sh
smallbox-wizard --version
smallbox-wizard --list-devices
smallbox-wizard --no-reboot
```

`--no-reboot` suppresses only the final reboot. It is not a dry run: confirmed
partitioning, formatting, copying, and installation operations still take
effect.

## Current scope

- Wired networking is configured with DHCP.
- Existing wired IPv4 configuration can be reused.
- Wi-Fi adapters may be detected but are not configured before package
  installation. Wi-Fi setup remains available in Enigma2 after first boot.
- The boot environment uses SysV init, Linux framebuffer, and evdev.

## License

SmallBox Wizard is licensed under the MIT License. See [`LICENSE`](LICENSE).
