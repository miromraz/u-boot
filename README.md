# U-Boot for the Google Pixel 4a (sunfish)

Developer handoff, 2026-10-26. This branch documents the SM7150 Tauchgang
bootloader baseline used for the Pixel 4a port. It is a source handoff,
not a verified installer for a stock phone.

## Source and scope

- Upstream fork: [`sm7150-mainline/u-boot`, `tauchgang`](https://github.com/sm7150-mainline/u-boot/tree/tauchgang).
- Baseline: `f8c04469e8284aa2f1691a30aa3b7dc59fa0b8cd`.
- Version: **U-Boot 2026.01-rc3**.
- Device tree: `qcom/sm7150-google-sunfish`.
- The baseline includes `HACK: Hardcode text offset for sunfish to boot`.

This handoff adds documentation without changing the baseline's runtime
behavior. The upstream code and original contributors' history are retained.
The maintainer also has an uncommitted, handset-specific boot environment;
that customization is not part of the committed baseline described here.

The maintainer's `miromraz/u-boot` repository also contains a separate Gemini
PDA port. Its default branch is not the Pixel 4a branch; always select the
explicit Pixel branch when sharing a link or cloning.

## Build

On a Linux build machine with an AArch64 cross compiler, install the normal
U-Boot dependencies (including `make`, `bison`, `flex`, OpenSSL and GnuTLS
development headers). From this repository:

```sh
make O=.output CROSS_COMPILE=aarch64-linux-gnu- \
  qcom_defconfig qcom-phone.config tauchgang.config
make O=.output CROSS_COMPILE=aarch64-linux-gnu- -j"$(nproc)" \
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
The workflow fetches that tool without pinning a commit, so artifact
reproducibility still needs improvement.

These commands are taken from the existing build configuration; no new
build or hardware test was run for this documentation-only handoff. In the
maintainer's environment, builds run on `llumik`, never on the `dev` host.

## Boot flow and storage assumptions

ABL launches U-Boot as an Android boot image. U-Boot provides a display menu,
EFI boot, a USB console, USB mass storage and fastboot-related tools. See the
existing [Qualcomm phone documentation](doc/board/qualcomm/phones.rst).
The phone fastboot implementation does not provide a UFS flashing backend.

The **committed baseline** in `board/qualcomm/qcom-phone.env` finds the outer
partition named `userdata`, exposes it through `blkmap`, and uses EFI boot
discovery to load the Linux system inside it. This assumes a prepared Linux
storage layout; it is not an instruction to overwrite Android userdata.

The maintainer's handset instead has a custom outer partition named `linux`
and preserves Android's `userdata`. Its local environment changes the
partition lookup, adds a five-second menu and an Android slot-B entry, and
still calls the Linux entry "Mobian" although the handset now uses BengalOS.
Those assumptions are specific to that handset.

The Android entry loads `/slotflip.efi` from the nested Linux ESP. That helper
is a separate Linux-based EFI payload, not a feature supplied by U-Boot.
The 2026-10-24 phone notes report that it is missing from the BengalOS ESP,
so that menu entry is currently broken. Do not treat the menu as a complete,
portable Android/Linux dual-boot implementation.

## Known limits and next work

1. Keep the generic boot environment separate from a handset's partition and
   Android slot policy before publishing the local customization as supported.
2. For a complete dual-boot handoff, supply a reproducible slot-flip helper,
   guard a failed helper load, and verify the return path from Android.
3. Pin external build tooling and record the source SHA with each boot image.
4. Build the exact shared source and validate boot/menu/USB recovery on
   hardware before publishing images as tested.

An unlocked device, a known recovery path and a device-specific partition
plan are prerequisites for installation work. No flash or repartition
commands are provided here because the maintainer's layout differs from a
stock handset.

The companion kernel source is
[`miromraz/linux`, `release/sunfish-0.26.1024.1`](https://github.com/miromraz/linux/tree/release/sunfish-0.26.1024.1).
General upstream U-Boot information remains in [README](README) and [doc](doc/).
