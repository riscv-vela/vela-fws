# vela-fws

Firmware components of the [Vela](https://github.com/riscv-vela/vela) project,
running on the [Q-Vela](https://github.com/riscv-vela/q-vela) emulation
platform.

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

## RAS — EDK2

`edk2/` provides the UEFI firmware with RERI support, used by the UEFI-based
RAS flow.

> TODO: build and usage instructions.
