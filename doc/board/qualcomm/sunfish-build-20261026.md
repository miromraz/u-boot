# Pixel 4a U-Boot build record, 2026-10-26

Status: **build and packaging verified; hardware boot not tested**.

The build ran in a disposable Debian sid container on `llumik`, with an
AArch64 GCC 14 cross compiler and host GCC 16. No build or test ran on `dev`.

| Input | Identity |
| --- | --- |
| U-Boot source | `6d9f0bf02d1b18fb3a8368c9c1447066198c47ac` |
| Runtime baseline | `f8c04469e8284aa2f1691a30aa3b7dc59fa0b8cd` (the source commit above adds only documentation) |
| Target compiler | `aarch64-linux-gnu-gcc-14 (Debian 14.4.0-2) 14.4.0` |
| Host compiler | `gcc (Debian 16.2.0-4) 16.2.0` |
| mkbootimg | `osm0sis/mkbootimg` at `17cea80bd5af64e45cdf9e263cad7555030e0e86` |
| Config | `qcom_defconfig qcom-phone.config tauchgang.config` |
| DTB selection | `CONFIG_DEFAULT_DEVICE_TREE=qcom/sm7150-google-sunfish` |

The shared branch's handset-specific local environment modification was
**not** included. This build uses the committed generic `userdata` lookup.

Commands, after installing the README's dependencies:

```sh
make O=.output CROSS_COMPILE=aarch64-linux-gnu- CC=aarch64-linux-gnu-gcc-14 \
  qcom_defconfig qcom-phone.config tauchgang.config
make O=.output CROSS_COMPILE=aarch64-linux-gnu- CC=aarch64-linux-gnu-gcc-14 \
  -j2 CONFIG_DEFAULT_DEVICE_TREE=qcom/sm7150-google-sunfish

# From a separate mkbootimg checkout at the revision above:
make CFLAGS=-Wstringop-overflow=0 mkbootimg

# From the U-Boot checkout, with that mkbootimg on PATH:
gzip -n -c .output/u-boot-nodtb.bin > uboot-dtb
cat .output/dts/upstream/src/arm64/qcom/sm7150-google-sunfish.dtb >> uboot-dtb
mkbootimg --base 0x0 --kernel_offset 0x00008000 --pagesize 4096 \
  --kernel uboot-dtb -o u-boot-sunfish.img
```

The build passed after adding the initially missing `xxd` dependency. Three
const-qualification warnings appeared in host utilities: one in
`tools/atmelimage.c`, two in the host libfdt overlay code. They are recorded
here; no fixes to those unrelated utilities are claimed.

Output image: **622592 bytes**, SHA256:

```text
bb742a2d6acf8415ad36e71cbf918a2c38e21523569a5e88e2e22139db5e2840
```

Structural checks passed: Android boot magic, page size 4096, kernel load
address `0x8000`, gzip payload and appended DTB equal to the built sunfish
DTB. The image has no ramdisk, matching the existing CI packaging method.
This has not been compared by booting it against the maintainer's previous
image, which retained an Android ramdisk.

Still required: temporary boot on the target, EFI discovery from prepared
userdata, menu and USB recovery tests, and an explicit tested boot-slot
installation/recovery procedure. This record does not certify an installer.
