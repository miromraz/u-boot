# U-Boot for the Google Pixel 4a (sunfish)

Developer handoff, updated 2026-10-29. This branch documents the SM7150 Tauchgang
bootloader baseline used for the Pixel 4a port. It is a source handoff,
not a verified installer for a stock phone.

For an unlocked stock-layout Pixel 4a whose Android userdata may be erased,
see the [installation preparation and release-status guide](https://github.com/miromraz/linux/blob/sunfish-share/Documentation/arm64/sunfish-installation.md).
Use the generic `userdata` lookup. The separate opt-in handset profile uses
a custom `linux` partition. No tested installer is currently published.

## Source and scope

- Upstream fork: [`sm7150-mainline/u-boot`, `tauchgang`](https://github.com/sm7150-mainline/u-boot/tree/tauchgang).
- Baseline: `f8c04469e8284aa2f1691a30aa3b7dc59fa0b8cd`.
- Version: **U-Boot 2026.01-rc3**.
- Device tree: `qcom/sm7150-google-sunfish`.
- The baseline includes `HACK: Hardcode text offset for sunfish to boot`.

This handoff pins the packaging tool and adds a separate handset profile and
guarded slot-flip helper. The generic phone environment remains unchanged.
The upstream code and original contributors' history are retained.

The maintainer's `miromraz/u-boot` repository also contains a separate Gemini
PDA port. Its default branch is not the Pixel 4a branch; always select the
explicit Pixel branch when sharing a link or cloning.

## Build

On a Linux build machine with a GCC 14 AArch64 cross compiler, install the
normal U-Boot dependencies (including `make`, `bison`, `flex`, `bc`, `xxd`,
OpenSSL and GnuTLS development headers, and Python development, setuptools,
pyelftools and YAML packages). A fresh build environment also needs a host C
compiler; the release builds used `gcc-14` for host tools too. From this repository:

```sh
make O=.output CROSS_COMPILE=aarch64-linux-gnu- CC=aarch64-linux-gnu-gcc-14 HOSTCC=gcc-14 \
  qcom_defconfig qcom-phone.config tauchgang.config
make O=.output CROSS_COMPILE=aarch64-linux-gnu- CC=aarch64-linux-gnu-gcc-14 HOSTCC=gcc-14 -j"$(nproc)" \
  CONFIG_DEFAULT_DEVICE_TREE=qcom/sm7150-google-sunfish
```

The phone config fragment is in `board/qualcomm/qcom-phone.config` and the
Tauchgang fragment is in `configs/tauchgang.config`. The output includes
`.output/u-boot-nodtb.bin` and
`.output/dts/upstream/src/arm64/qcom/sm7150-google-sunfish.dtb`.

The existing [GitHub workflow](.github/workflows/) builds these targets and
packages Android boot images. Its packaging concatenates the gzipped
`u-boot-nodtb.bin` with the control DTB and uses a kernel offset of `0x8000`
and a page size of `4096`. It uses the C `mkbootimg` implementation from
[`osm0sis/mkbootimg`](https://github.com/osm0sis/mkbootimg).
The workflow pins that tool to `17cea80bd5af64e45cdf9e263cad7555030e0e86`,
the revision used for the 2026-10-26 handoff build. Packaging uses `gzip -n`
to omit the input filename and timestamp from the compressed stream.

A fresh GCC 14 build and Android boot-image packaging passed on `llumik` on
2026-10-26 for both generic and handset profiles, including extraction and
verification of their compiled environments. Each build emitted three
host-tool const-qualification warnings. On 2026-10-29 the handset image was
RAM-launched through ABL and booted the preserved Linux system. On 2026-10-30
it was written to the maintainer handset's `boot_a`, boots Linux, and its Android
entry completed a round trip to slot B and back. The generic stock-userdata
image remains untested
on a recipient handset. See the
[build record](doc/board/qualcomm/sunfish-build-20261026.md). In the maintainer's
environment, builds run on `llumik`, never on the `dev` host.

## Boot flow and storage assumptions

ABL launches U-Boot as an Android boot image. U-Boot provides a display menu,
EFI boot, a USB console, USB mass storage and fastboot-related tools. See the
existing [Qualcomm phone documentation](doc/board/qualcomm/phones.rst).
The phone fastboot implementation does not provide a UFS flashing backend.

The **committed baseline** in `board/qualcomm/qcom-phone.env` finds the outer
partition named `userdata`, exposes it through `blkmap`, and uses EFI boot
discovery to load the Linux system inside it. This assumes a prepared Linux
storage layout; it is not an instruction to overwrite Android userdata.

The maintainer's handset has a custom outer partition named `linux` and
preserves Android's `userdata`. Apply `configs/sunfish-handset.config` last
to opt into that layout, its five-second menu, BengalOS label and Android
slot-B entry. The generic profile has no Android entry. Those assumptions
are specific to the maintainer's handset.

The new handset entry loads **`/EFI/sunfish/slotflip.efi`** from the nested
Linux ESP and stops with a message if loading fails. The separate
[off-phone helper builder](tools/sunfish-slotflip/README.md) creates this payload,
a no-write probe, and root-path compatibility copies for the old flashed
bootloader. Both paths were installed and hash-checked on 2026-10-29. A counted
no-write probe returned automatically to the preserved Linux system, with unchanged
boot-control metadata and boot partitions. Its console policy result was not
captured, and no Android slot switch was performed. On 2026-10-30, with this
U-Boot installed in `boot_a`, a one-shot `BootNext` to the helper (no keypress)
booted LineageOS on slot B; a maintainer-installed Android module selected A again
and `adb reboot` returned to Linux. The physical menu keypress is still untested.
See the [round-trip record](https://github.com/miromraz/linux/blob/sunfish-share/Documentation/arm64/sunfish-verification-20261029.md#u-boot-installation-and-android-round-trip-2026-10-30) and the
[handset verification record](https://github.com/miromraz/linux/blob/sunfish-share/Documentation/arm64/sunfish-verification-20261029.md).

## Known limits and next work

1. Test the physical Android menu keypress. The helper itself was validated
   through `BootNext` on 2026-10-30.
2. Provide an Android-side return policy for recipients. The helper does not
   install one; without it Android keeps booting slot B until fastboot
   `--set-active=a`.
3. Record the full source and toolchain identities with each image; the current
   packaging tool is pinned and the handoff build has a checksum.
4. Validate boot/menu/USB recovery on hardware before publishing images as
   tested. A successful build and valid image header do not prove bootability.

An unlocked device, a known recovery path and a device-specific partition
plan are prerequisites for installation work. No flash or repartition
commands are provided here because the maintainer's layout differs from a
stock handset.

The companion kernel source is
[`miromraz/linux`, `release/sunfish-0.26.1026.1`](https://github.com/miromraz/linux/tree/release/sunfish-0.26.1026.1).
General upstream U-Boot information remains in [README](README) and [doc](doc/).
