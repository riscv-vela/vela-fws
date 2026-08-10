# vela-fws

Firmware components of the [riscv-vela](https://github.com/riscv-vela/) project,
running on the [Q-Vela](https://github.com/riscv-vela/q-vela) emulation
platform, booting [Vela-OS](https://github.com/riscv-vela/vela).

| Subsystem | Contents |
|-----------|----------|
| **TEE** (Keystone) | OpenSBI hosting the Rust Security Monitor. Enclave isolation is enforced with **RISC-V WorldGuard** instead of Keystone's original PMP-based scheme. |
| **RAS** (RERI) | OpenSBI and EDK2 support for the RISC-V Error Record Interface |

---

## Repository Structure

```
vela-fws/
├── sm/            # Security Monitor (Rust) — built as libvela.a, linked into OpenSBI
├── opensbi/       # OpenSBI (M-mode firmware) with the Vela TEE/RERI patches applied
├── edk2/          # EDK2 UEFI firmware with RERI support
└── buildroot/     # Submodule — lightweight root filesystem builder
```

---

## TEE — Building OpenSBI + Security Monitor

### Prerequisites

A RISC-V cross toolchain and [rustup](https://rustup.rs) are required. The
Security Monitor pins its toolchain via `sm/rust-toolchain.toml`, so `cargo`
on `PATH` must be **rustup's shim** — a distro-packaged `cargo` will not pick
up the pinned nightly.

```bash
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
export PATH=$HOME/.cargo/bin:$PATH
```

### 1. Build the Security Monitor

OpenSBI links the Security Monitor, so `sm` must be built **first**.

```bash
cd sm
cargo build          # → sm/target/riscv64gc-unknown-linux-gnu/debug/libvela.a
cd ..
```

### 2. Build OpenSBI

```bash
cd opensbi
make PLATFORM=generic CROSS_COMPILE=riscv64-unknown-linux-gnu- \
     PLATFORM_RISCV_ABI=lp64d \
     -j$(nproc)
cd ..
```

`PLATFORM_RISCV_ABI=lp64d` is **required**. OpenSBI defaults to the soft-float
`lp64` ABI, while `libvela.a` is built for `riscv64gc-unknown-linux-gnu`
(`lp64d`); without it the link fails with
`can't link double-float modules with soft-float modules`.

### Build Output

| File | Description |
|------|-------------|
| `opensbi/build/platform/generic/firmware/fw_jump.bin` | M-mode firmware with the Security Monitor linked in, passed to QEMU via `-bios` |

Launching this firmware requires `-machine virt,wg=on` (WorldGuard) and
`-semihosting-config enable=on` — the Security Monitor emits its console output
through RISC-V semihosting. See the
[q-vela](https://github.com/riscv-vela/q-vela) README for the full launch
command.

---

## RAS — EDK2 and Root Filesystem

`edk2/` provides the UEFI firmware with RERI support, used by the UEFI-based
RAS flow.

OpenSBI is shared with TEE — build `sm` and `opensbi` first (see above). RAS
boots `OpenSBI → EDK2 (UEFI) → Linux`, so it uses **`fw_dynamic.bin`** rather
than `fw_jump.bin`, and needs two artifacts TEE does not: the EDK2 pflash
images and a root filesystem.

### Prerequisites

The same RISC-V cross toolchain used for OpenSBI. `BaseTools` is built with the
host compiler and `python3`.

### 1. Build EDK2

Run these in **one shell** — `edksetup.sh` exports the environment and puts
`build` on `PATH`.

```bash
export WORKSPACE=$(pwd)
export GCC5_RISCV64_PREFIX=riscv64-linux-gnu-   # match your toolchain prefix
export PACKAGES_PATH=$WORKSPACE/edk2
export EDK_TOOLS_PATH=$WORKSPACE/edk2/BaseTools

source edk2/edksetup.sh --reconfig
make -C edk2/BaseTools -j$(nproc)
source edk2/edksetup.sh BaseTools

build -a RISCV64 --buildtarget RELEASE -p OvmfPkg/RiscVVirt/RiscVVirtQemu.dsc -t GCC5
```

`edksetup.sh` is sourced twice on purpose: the first call populates
`edk2/Conf/` from the templates, the second one picks up `BaseTools` after it
has been built. `Build/` is created under the repository root (`vela-fws/`),
not under `edk2/`.

The RERI support is already applied in this tree — `AcpiErrorInjectionTableDxe`
(EINJ) and `AcpiHardwareErrorTableDxe` (HEST) are registered in
`OvmfPkg/RiscVVirt/RiscVVirtQemu.dsc`. No patch step is needed here.

### 2. Pad the flash images

```bash
truncate -s 32M $WORKSPACE/Build/RiscVVirtQemu/RELEASE_GCC5/FV/RISCV_VIRT_CODE.fd
truncate -s 32M $WORKSPACE/Build/RiscVVirtQemu/RELEASE_GCC5/FV/RISCV_VIRT_VARS.fd
```

QEMU pflash requires the backing file to match the flash device size exactly
(32 MiB). The build emits smaller images, so this step is **required** —
without it QEMU refuses to start.

### 3. Build the root filesystem

```bash
cd buildroot
make qemu_riscv64_virt_defconfig
make menuconfig
make -j$(nproc)
cd ..
```

In `menuconfig`:

| Option | Why |
|--------|-----|
| `Filesystem images` → **cpio the root filesystem** | **Required** — the RAS quick start boots with `-initrd rootfs.cpio` |
| `Kernel` → **Linux kernel** — *disable* | The kernel is built separately in [vela-linux](https://github.com/riscv-vela/vela-linux) |
| `Target packages` → `Networking applications` → `iproute2`, `openssh` | Optional — for SSH into the guest |

buildroot builds its own toolchain (`riscv64-buildroot-linux-gnu-`), so
`CROSS_COMPILE` does not apply here. busybox provides `devmem`, which the RERI
smoke test uses to program the error records.

### Build Output

| File | Description |
|------|-------------|
| `opensbi/build/platform/generic/firmware/fw_dynamic.bin` | M-mode firmware, passed to QEMU via `-bios` |
| `Build/RiscVVirtQemu/RELEASE_GCC5/FV/RISCV_VIRT_CODE.fd` | UEFI code, attached as pflash0 (read-only) |
| `Build/RiscVVirtQemu/RELEASE_GCC5/FV/RISCV_VIRT_VARS.fd` | UEFI variables, attached as pflash1 — QEMU writes to it, so use a disposable copy |
| `buildroot/output/images/rootfs.cpio` | initramfs, passed via `-initrd` |

### Running

Launching requires `-M virt,reri=true,aia=aplic-imsic` together with the two
pflash devices. RERI errors are reported through ACPI (HEST/GHES), so the ACPI
boot path via EDK2 is required — a device-tree boot produces no reports.

See [q-vela `docs/ras.md`](https://github.com/riscv-vela/q-vela/blob/main/docs/ras.md)
for the full launch command and the RERI smoke test.

> `DXEFV` is 98% full (about 60 KiB free) in this configuration. Adding drivers
> may overflow the firmware volume.
