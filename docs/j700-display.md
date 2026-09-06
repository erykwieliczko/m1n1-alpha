<!-- SPDX-License-Identifier: MIT -->
# J700 inherited-display handoff

J700/T8140 can use the normal inline payload loader for a minimal U-Boot
framebuffer console. This is not yet a full Linux/platform handoff.

The runtime FDT must identify both `apple,j700` and `apple,t8140`, with a boolean
`asahi,display-only` in `/chosen`. The option is recognized only when the actual
chip is T8140 and board ID is 0x64. It has no effect on existing Mac targets.

For this mode, payload dispatch does not start secondary CPUs or change their
performance states. FDT preparation retains the existing m1n1, ADT, FDT,
chainload and CTRR reservations, live usable-memory bounds, framebuffer fixup,
firmware metadata and machine identity. It does not run ISP initialization or
peripheral/CPU topology fixups. Kernel handoff skips the later MCC/clock/tunable,
USB, PCIe and DAPF initialization, then uses the normal next-stage entry and
exception/cache/MMU teardown. When the FDT supplies an `mtp` alias, preparation
also reserves the live MTP `segment-ranges`, propagates the keyboard layout,
and initializes only `/arm-io/dart-mtp`'s ADT-described DAPF. m1n1 does not start
MTP or consume its HID protocol; U-Boot owns that session. This preserves inherited scanout
without pretending unimplemented devices are ready for an OS.

The ordinary early stage2 initialization still runs. T8140 is excluded from
the older-Mac Sequoia display power-cycle branch: the unknown mBoot version
otherwise selects that workaround and stalls in DCP initialization. Dummy,
external and non-retina display handling is unchanged; this mode requires a
valid initialized internal framebuffer. Do not generalize it to unsupported
display states.

Build stage2 with `make RELEASE=1 CHAINLOADING=0 USE_CLANG=1 BUILDSTD=1` and
package it with U-Boot's `apple_j700_defconfig` DTB and ARM64 Image using
`tools/apple_m1n1_payload.py` in the U-Boot tree. The FDT has a disabled
`/chosen/framebuffer` placeholder and an address-less `/memory`; normal m1n1
fixups populate them from each boot. Gzip compression lets the regular payload
loader place U-Boot at its required alignment.

The initial console uses the inherited physical UART at 115200 baud and the
inherited 32-bit framebuffer. It requires no active host to draw on screen.
RAM upload for testing does not alter installed stage1 or imply persistent
disk boot has been installed. U-Boot can add the built-in MTP keyboard without
changing this boot ABI. SMP, storage, USB, reset and an OS boot
remain separate bring-up milestones.

## Optional polled ANS storage handoff

When the minimal FDT supplies `ans`, `ans-mbox` and `sart-ans` aliases, stage2
transfers the secure NVMe BAR, separate NVMMU, ASC control, mailbox and SART
resources from the live ADT. T8140's linear submission registers extend beyond
the advertised 64-KiB secure BAR, so the NVMe aperture is expanded to 256 KiB,
as on T8132. The mailbox is split from ASC control and only SART's first
resource is transferred; its third window overlaps the separate NVMMU.

The ANS firmware segments come from `/arm-io/ans/iop-ans-nub` and are described
as reserved memory. This minimal path does not initialize m1n1's NVMe driver,
reset ANS, or apply the T8132 PMGR readback workaround. The uninitialized
`nvme_shutdown()` path leaves inherited firmware untouched. On the tested RAM
boot, ASC is running while standard NVMe queues are disabled; U-Boot wakes
RTKit and owns the new queues and shared buffers.

No mailbox/NVMe interrupt properties are synthesized for this polled U-Boot
milestone: Neo's interrupt semantics and native Linux storage takeover remain
unvalidated. The existing normal T8132 handoff still transfers its IRQs with
the same ordering as before. See U-Boot's `doc/board/apple/j700.rst` for the
storage test procedure and limits.
