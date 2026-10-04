.. SPDX-License-Identifier: GPL-2.0+

Planet Computers Gemini PDA
===========================

The Planet Computers Gemini PDA is a clamshell handset built around the
MediaTek MT6797 (Helio X20/X25/X27) SoC with 4 GiB of LPDDR3 and eMMC on
MSDC0.

U-Boot runs as a payload **chainloaded from the vendor LK bootloader**; it is
not a first-stage bootloader and does not replace the preloader, LK or TEE.

Boot chain
----------

::

   BROM -> preloader -> ATF + TEE -> LK -> Android boot image (boot2/boot3)

LK reads an Android boot image v0 from the selected boot partition, gunzips the
"kernel" to ``0x40080000``, scans for a DTB appended to the gzip stream (it
ignores the header ``dt_size``), patches that DTB (``/memory``, every CPU
node's ``clock-frequency`` and its ``mblock-*`` reserved-memory regions) and
jumps to the payload in 64-bit mode with ``x0`` pointing at the patched DTB.

U-Boot therefore uses the LK-provided DTB as its control device tree
(``board_fdt_blob_setup()`` picks it up from ``x0`` via
``CONFIG_SAVE_PREV_BL_FDT_ADDR``). The Linux kernel loads its own full device
tree from the rootfs; U-Boot only copies LK's ``mblock-*`` reservations into
the kernel DTB in ``ft_board_setup()``.

The 64-bit boot path is selected by ``bootopt=64S3,32N2,64N2`` in the boot
image command line. LK leaves the MTK RGU watchdog running (10 s timeout), so
``CONFIG_WDT_MTK`` is enabled and U-Boot services it.

Building
--------

.. code-block:: bash

   export CROSS_COMPILE=aarch64-linux-gnu-
   make O=build-gemini gemini-pda_defconfig
   make O=build-gemini -j$(nproc)

Then wrap the result in a boot image LK accepts (requires ``mkbootimg``):

.. code-block:: bash

   ./board/planet/gemini-pda/mkbootimg.sh build-gemini
   # -> build-gemini/u-boot-gemini-boot.img

The script gzips ``u-boot.bin``, appends U-Boot's own Gemini DTB
(``mt6797-gemini-pda.dtb``) and packs an Android boot image v0 with the
offsets and ``bootopt`` command line LK expects.

Flashing
--------

.. warning::

   Flash **only** to a secondary boot partition (``boot2`` or ``boot3``).
   **Never** write to ``boot``, ``lk``, ``lk2``, ``preloader`` or ``nvram`` --
   overwriting the preloader/LK can hard-brick the device and overwriting
   ``nvram`` destroys the IMEI. Back up these partitions before you start.

With mtkclient (MT6797 = hwcode 0x279)::

   mtk w boot2 build-gemini/u-boot-gemini-boot.img

or, from a booted system that exposes fastboot::

   fastboot flash boot2 build-gemini/u-boot-gemini-boot.img

Booting
-------

Power on while holding the **silver side button** to make LK boot the
``boot2`` partition. The U-Boot banner appears on the debug UART (UART0,
921600 8N1). U-Boot then runs ``bootflow scan`` and boots an
``extlinux.conf`` from the eMMC rootfs.

Debug UART
----------

The early debug UART is off by default. To enable it, add to the defconfig::

   CONFIG_DEBUG_UART=y
   CONFIG_DEBUG_UART_BASE=0x11002000
   CONFIG_DEBUG_UART_CLOCK=26000000
   CONFIG_DEBUG_UART_ANNOUNCE=y

UART0 is at ``0x11002000``; the console runs at 921600 baud.
