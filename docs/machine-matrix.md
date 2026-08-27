# SmallBox and single-core machine matrix

This audit is based on the machine configuration and all literal machine
includes in `oe-alliance-core`. `FLASHSIZE` is the OpenEmbedded image budget;
on some receivers it is deliberately smaller than the physical NAND.

These rules describe the OpenATV image integration only. Other OE-Alliance
distributions continue to use their existing SmallFlash image and setup paths.

## Build rules

- In OpenATV, `smallflash` selects the internal bootstrap image and the native
  C wizard.
- `smallflash` + `chkrootmb` + `FLASHSIZE <= 256` builds one additional full
  Chkroot `rootfs.tar.bz2` archive in the same build.
- `smallbox-chkroot-required` suppresses FlashExpander. It is intentionally
  limited to `dm500hd` and `dm800se`.
- `dm8000` and `gb800solo` have no `chkrootmb`; their wizard offers only
  FlashExpander and no Chkroot archive is built.
- `singlecore` alone never selects the SmallBox wizard or its transport image.
  It keeps optional server packages installed but disables their fresh-image
  SysV autostart links. Enigma2 can enable them later.
- On a SmallBox FlashExpander, OPKG installs the excluded packages after the
  image post-process hooks have run. The native wizard therefore applies the
  same Single-Core service policy immediately after OPKG and before writing
  its completion marker.
- A native flash/backup format remains machine-specific. The SmallBox full
  image is a tar only because it is a Chkroot transport image; ordinary
  Chkroot and flash images may still be UBI or JFFS2.

## Totals

- 41 `smallflash` machines
- 43 `singlecore` machines
- 25 machines with both features
- 59 machines in the union
- SmallBox sizes: 7 x 64, 26 x 96, 5 x 128, 3 x 256 MiB
- Single-core sizes: 1 x 64, 21 x 96, 4 x 128, 16 x 256, 1 x 512 MiB

## Machines

| Machine | Class | `FLASHSIZE` MiB | Chkroot in native wizard | Native flash format |
|---|---|---:|---|---|
| `7000s` | singlecore | 256 | none | UBI |
| `7100s` | singlecore | 256 | none | UBI |
| `7210s` | singlecore | 256 | none | UBI |
| `7220s` | singlecore | 256 | none | UBI |
| `7300s` | singlecore | 256 | none | UBI |
| `7400s` | singlecore | 256 | none | UBI |
| `9900lx` | singlecore | 96 | none | UBI |
| `ch62lc` | smallflash + singlecore | 96 | optional | UBI |
| `dags7335` | smallflash + singlecore | 96 | optional | UBI |
| `dags7356` | smallflash | 96 | optional | UBI |
| `dags7362` | singlecore | 256 | none | UBI |
| `dm500hd` | smallflash | 64 | required | UBI/NFI |
| `dm500hdv2` | smallflash | 96 | optional | UBI/NFI |
| `dm7020hd` | smallflash | 64 | optional | UBI/NFI |
| `dm7020hdv2` | smallflash | 64 | optional | UBI/NFI |
| `dm8000` | smallflash | 96 | disabled | UBI/NFI |
| `dm800se` | smallflash | 64 | required | UBI/NFI |
| `dm800sev2` | smallflash | 96 | optional | UBI/NFI |
| `e3hd` | smallflash + singlecore | 96 | optional | UBI |
| `et4x00` | smallflash + singlecore | 96 | optional | UBI |
| `et5x00` | smallflash | 96 | optional | UBI |
| `et6x00` | smallflash | 96 | optional | UBI |
| `et7x00` | singlecore | 96 | none | UBI |
| `et9x00` | smallflash | 96 | optional | UBI |
| `ew7356` | smallflash | 96 | optional | UBI |
| `ew7358` | smallflash + singlecore | 96 | optional | UBI |
| `ew7362` | smallflash + singlecore | 96 | optional | UBI |
| `formuler3` | singlecore | 256 | none | UBI |
| `formuler3ip` | singlecore | 256 | none | UBI |
| `formuler4` | singlecore | 256 | none | UBI |
| `formuler4ip` | singlecore | 256 | none | UBI |
| `gb7325` | smallflash + singlecore | 64 | optional | UBI |
| `gb7358` | smallflash + singlecore | 96 | optional | UBI |
| `gb7362` | singlecore | 96 | none | UBI |
| `gb800solo` | smallflash + singlecore | 128 | disabled | JFFS2 |
| `h3` | singlecore | 256 | none | UBI |
| `hd1100` | smallflash + singlecore | 128 | optional | UBI |
| `hd1200` | singlecore | 256 | none | UBI |
| `hd500c` | smallflash | 128 | optional | UBI |
| `i55` | smallflash + singlecore | 256 | optional | UBI |
| `inihde` | smallflash + singlecore | 96 | optional | UBI |
| `inihde2` | smallflash + singlecore | 96 | optional | UBI |
| `jj7362` | smallflash + singlecore | 96 | optional | UBI |
| `lc` | smallflash | 256 | optional | UBI |
| `odinm7` | smallflash + singlecore | 96 | optional | UBI |
| `odinm9` | smallflash | 64 | optional | UBI |
| `osnino` | singlecore | 512 | none | UBI |
| `sh1` | smallflash + singlecore | 256 | optional | UBI |
| `tiviarmin` | smallflash + singlecore | 96 | optional | UBI |
| `ultramini` | smallflash + singlecore | 96 | optional | UBI |
| `vg2000` | smallflash + singlecore | 96 | optional | UBI |
| `vg5000` | smallflash + singlecore | 96 | optional | UBI |
| `vuduo` | smallflash + singlecore | 128 | optional | UBI (legacy `.jffs2` filename) |
| `vusolo` | smallflash + singlecore | 128 | optional | UBI (legacy `.jffs2` filename) |
| `vuuno` | smallflash | 64 | optional | UBI (legacy `.jffs2` filename) |
| `xc7362` | singlecore | 256 | none | UBI |
| `xp1000` | smallflash + singlecore | 96 | optional | UBI |
| `yh62tc` | smallflash + singlecore | 96 | optional | UBI |
| `yh7362` | smallflash + singlecore | 96 | optional | UBI |

## Single-core fresh-image service policy

The packages remain present. Fresh images remove only the SysV start links for
`autofs`, `avahi-daemon`, `llmnrd`, `nfsserver`, `samba`, `smartd`,
`telnetd.busybox`, `vsftpd`, `wsdd`, and `wsdd2` in runlevels 2 through 5.
`update-rc.d` from Enigma2's Network Services screen recreates the normal
links when the user enables a service. Dropbear, Chrony and Cron remain active.
The DHCP wait code performs its wait only when a softcam init script exists.

## Resolved exceptions

- `dm7020hd` and `dm7020hdv2` include 1024-MiB NAND geometry although their
  OpenEmbedded `FLASHSIZE` budget is 64. They must not be inferred as
  Chkroot-only from that number.
- `dm500hd` and `dm800se` use the real 64-MiB NAND geometry and are explicitly
  marked `smallbox-chkroot-required`.
- The former `chkrootmb && FLASHSIZE <= 256` dependency affected many unrelated
  receivers. Requiring `smallflash` prevents normal images from gaining a
  SmallBox full-image build side effect.
