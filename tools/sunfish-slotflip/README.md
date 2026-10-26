# Maintainer handset: Android slot-flip helper

This is an opt-in helper for the repartitioned Pixel 4a with **U-Boot in A,
Android in B, and Linux in a separate `linux` partition**. It is not part of
the stock-userdata installation path and is not a portable dual-boot installer.

The bootloader profile is `configs/sunfish-handset.config`, applied last:

```sh
make O=.output-handset CROSS_COMPILE=aarch64-linux-gnu- \
  CC=aarch64-linux-gnu-gcc-14 HOSTCC=gcc-14 \
  qcom_defconfig qcom-phone.config tauchgang.config sunfish-handset.config
make O=.output-handset CROSS_COMPILE=aarch64-linux-gnu- \
  CC=aarch64-linux-gnu-gcc-14 HOSTCC=gcc-14 -j2
```

Without this final fragment, the generic phone environment still uses
`userdata` and has no Android slot-B entry. The handset menu calls the Linux
entry BengalOS and loads Android's helper from **`/EFI/sunfish/slotflip.efi`**
on the nested ESP. A missing helper produces a message and never executes a
stale load buffer.

## Build off-phone

Build on `llumik`, with host `python3`, `readelf` and `ukify`. Inputs are a
matching ARM64 Linux Image/DTB and release string, an ARM64 systemd EFI stub,
and an ARM64 runtime root containing busybox, qbootctl and their libraries.
The runtime root can be assembled by extracting trusted Debian ARM64 packages
(`busybox-static`, `qbootctl`, `libc6`, `zlib1g`, plus any additional runtime
dependencies). Extract `systemd-boot-efi:arm64` for `linuxaa64.efi.stub`.
Record the package versions and checksums with the build.

```sh
python3 tools/sunfish-slotflip/build.py \
  --kernel /build/kernel/arch/arm64/boot/Image \
  --dtb /build/kernel/arch/arm64/boot/dts/qcom/sm7150-google-sunfish.dtb \
  --release 7.1.0-sm7150 \
  --stub /build/arm64/usr/lib/systemd/boot/efi/linuxaa64.efi.stub \
  --runtime-root /build/arm64 --output /build/slotflip-output
```

The builder validates ARM64 inputs and shared-library dependencies, writes a
small initramfs without requiring root or `mknod`, and produces:

- `EFI/sunfish/slotflip.efi`: validates the A -> Android B policy, marks A
  successful, selects B only if that succeeds, then reboots.
- `EFI/sunfish/slotflip-test.efi`: boots the same kernel/initramfs, reports the
  current slot and reboots **without any boot-control writes**.
- Identical copies at the output root for the old already-flashed bootloader
  and existing BootNext entries that still use `/slotflip*.efi`.
- `slotflip-manifest.json`, with kernel, DTB, stub, runtime and payload hashes.

Stage into a normal build directory. The builder does not install U-Boot or
modify a phone's slot metadata. After checking and backing up the phone's
ESP, install the staged `EFI/sunfish` directory and compatibility copies to
that ESP. No install or slot switch is performed as part of the build.

The initramfs no longer mounts the ESP at an assumed byte offset just to log.
It reports through the kernel and display consoles. The probe payload is the
first hardware test; a successful compile does not prove the EFI chain works.

## Validation and remaining hardware work

Run the mock-device policy and initramfs tests on the build host:

```sh
python3 tools/sunfish-slotflip/test_slotflip.py
```

The tests do not call real boot control. They cover successful operation,
failure to mark A, failure to select B, invalid inputs, the no-write probe,
missing EFI loads, and standard cpio compatibility.

The phone must still validate probe boot/return, the Android transition,
the return from Android, menu and USB recovery. Android's own boot policy
determines whether it returns to A on reboot; this helper does not install
an Android-side return policy. Preserve a known-good A boot image and an
independent ABL recovery path until those tests pass.

qbootctl is the separate [linux-msm/qbootctl](https://github.com/linux-msm/qbootctl)
project. No qbootctl or firmware binaries are embedded in this source tree.
