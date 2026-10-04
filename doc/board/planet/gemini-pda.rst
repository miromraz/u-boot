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
921600 8N1). U-Boot then shows a boot menu for ~3 s and, if no key is
pressed, auto-selects the first entry (``bootflow scan``), which boots an
``extlinux.conf`` from the eMMC rootfs.

USB gadget: fastboot and UMS
----------------------------

The left USB-C port is wired to the MT6797 SSUSB/MTU3 controller, driven here
in peripheral mode (high-speed only; the USB3 PHY is not wired). From the
U-Boot shell::

   fastboot usb 0          # host can then: fastboot flash/boot
   ums 0 mmc 0             # expose the eMMC as USB mass storage

The gadget enumerates with the Google fastboot IDs (``18d1:4ee0``). Data path,
on ``fastboot usb 0``:

* ``mtu3`` (UDC) probes and requests its three clocks;
* the ``mediatek,mt6797-ssusb-cg`` provider ungates the infracfg SSUSB gates
  (ICG2: sys bit 24, ref bit 9, bus bit 18) and reparents the topckgen
  ``ssusb_top_sys`` mux to ``univpll3_d2``;
* the T-PHY powers on and, because VBUS/session is supplied by firmware rather
  than sensed, forces a valid B-device session
  (``mediatek,force-b-session-valid``);
* the device pulls up D+ and enumerates at high speed.

.. warning::

   ``fastboot flash``/``erase`` are restricted to an allowlist of partitions
   (``boot2``, ``boot3``, ``linux``, ``userdata``); every other partition
   (``preloader``, ``lk``, ``lk2``, ``nvram``, ``seccfg``, ``protect*``,
   ``md*``, ``tee*``, ``pgpt``/``sgpt`` …) is refused. ``oem format`` (GPT
   rewrite) and whole-eMMC erase are disabled by not enabling
   ``FASTBOOT_CMD_OEM_FORMAT`` / ``FASTBOOT_MMC_USER_SUPPORT``.

.. warning::

   ``ums 0 mmc 0`` exposes the **whole** eMMC user area to the host,
   including ``nvram``, ``lk`` and the GPT. The fastboot partition allowlist
   does not apply to it. Only write to ``boot2``, ``boot3`` or ``linux``.

Clocks and the forced session are marked ``TODO(hw-verify)`` in the sources:
they are adapted from the Linux MT6797 bring-up and the bsg100 reference and
have not yet been confirmed on this unit. ``mediatek,force-vbus`` on the
``ssusb`` node is an in-tree alternative if the PHY force alone is insufficient.

Boot menu and hardware keys
---------------------------

The power and home keys are read from the MT6351 PMIC (TOPSTATUS register)
through the PMIC wrapper and exposed as a keyboard, so the boot menu can be
navigated without a serial console:

* home key -> *Down* (move to the next entry)
* power key -> *Enter* (select the highlighted entry)

The built-in QWERTY keyboard (AW9523 expander on I2C5, polled 8x7 matrix) is
also an input device, so arrow keys and Enter work too. It relies on LK
leaving the AW9523 out of reset (``TODO(hw-verify)``); the PMIC keys work
regardless.

The menu does not wrap from the last entry back to the first, so the home key
only moves downward; starting at the top, the two keys still reach every entry.

.. note::

   ``Esc(On)`` is the PMIC power key and the silver side button the PMIC home
   key. Planet's LK reads the same register bits to choose ``boot2``/``boot3``,
   so both keys are usually still held when U-Boot starts; ``preboot`` waits
   up to ~5 s for them to be released before the autoboot countdown and menu.

Debug UART
----------

The early debug UART is off by default. To enable it, add to the defconfig::

   CONFIG_DEBUG_UART=y
   CONFIG_DEBUG_UART_BASE=0x11002000
   CONFIG_DEBUG_UART_CLOCK=26000000
   CONFIG_DEBUG_UART_ANNOUNCE=y

UART0 is at ``0x11002000``; the console runs at 921600 baud.
