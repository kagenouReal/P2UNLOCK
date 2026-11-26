# P2UNLOCK

[![Build](https://github.com/kagenouReal/P2UNLOCK/actions/workflows/build.yml/badge.svg)](https://github.com/kagenouReal/P2UNLOCK/actions/workflows/build.yml)

A Windows tool that unlocks the PCIe link speed (Gen2) and width (x8 / x16) of the NVIDIA P104-100 and P104-101 mining cards. One executable serves as both the command-line tool and an optional auto-start Windows service.

> **Status:** Early beta testing. Everything below marked "verified" was verified on one system (see [Tested configuration](#tested-configuration)). Please read [What is and is not verified](#what-is-and-is-not-verified) before you run it, and please [report your results](#help-wanted-report-your-results). Success reports are just as useful as failures.

> **Development roadmap:** P2UNLOCK is currently CLI-only. The main priority is collecting tester reports from the community to improve compatibility, reliability, and overall functionality across different hardware configurations. After gathering sufficient feedback, the plan is to expand the project with additional features. The next major focus will likely be a GUI to make P2UNLOCK easier to use for everyone, including users who are not comfortable with command-line tools.

## Quick download

**Download only [`P2UNLOCK-windows-x64.zip`](https://github.com/kagenouReal/P2UNLOCK/releases/latest/download/P2UNLOCK-windows-x64.zip)** from the [latest release](https://github.com/kagenouReal/P2UNLOCK/releases/latest). It is the only asset you need — it already contains `P2UNLOCK.exe` and `drivers\SvIoCtrlx64.sys`.

1. Extract the ZIP anywhere (for example, to the Desktop).
2. Right-click `P2UNLOCK.exe` → **Run as administrator**.
3. On first run the bundled driver is copied automatically to `%ProgramData%\P104-Gen2x8\tool\drivers\`. You do **not** need to copy any `.sys` file manually.
4. Check the card with `P2UNLOCK.exe scan`, then follow [Quick start](#quick-start).

If you only have the bare `.exe`, put `SvIoCtrlx64.sys` next to it in a `drivers\` folder — see [Getting the driver](#getting-the-driver).

---

## Table of contents

- [Quick download](#quick-download)
- [What it does (and does not do)](#what-it-does-and-does-not-do)
- [Tested configuration](#tested-configuration)
- [What is and is not verified](#what-is-and-is-not-verified)
- [Requirements](#requirements)
- [Hardware prerequisite for more than x4](#hardware-prerequisite-for-more-than-x4)
- [Getting the driver](#getting-the-driver)
- [Build](#build)
- [Quick start](#quick-start)
- [Commands](#commands)
- [How it works](#how-it-works)
- [Behaviour you should know about](#behaviour-you-should-know-about)
- [Troubleshooting](#troubleshooting)
- [Safety and security](#safety-and-security)
- [Help wanted: report your results](#help-wanted-report-your-results)
- [AI assistance and prior work](#ai-assistance-and-prior-work)
- [Disclaimer](#disclaimer)
- [License](#license)
- [Test report template](#test-report-template)

---

## What it does (and does not do)

The stock P104-100 is configured for a Gen1 x4 link. P2UNLOCK changes the card's link configuration so that it is allowed to train at a higher speed and a wider width, then retrains the link and checks the outcome.

| Command | Effect |
| --- | --- |
| `gen 2` | Lets the card and its upstream port train at Gen2 (5.0 GT/s) instead of Gen1. |
| `width 8` / `width 16` | Lifts the card's width limit so it may train at x8 / x16 instead of x4. |

**What it cannot do:** it cannot create PCIe lanes. A link only trains at a given width if the lanes actually exist electrically and carry a valid signal. On a stock card only four lanes are usable, so the link stays at x4 until the extra lanes are physically enabled (see [Hardware prerequisite for more than x4](#hardware-prerequisite-for-more-than-x4)). A lane that is dead, open or shorted cannot be rescued in software, and the link width must be a power of two counted from lane 0, so one bad lane inside the range caps the width.

The tool also writes a cosmetic x16 declaration (the card's reported maximum link width). Monitoring tools such as GPU-Z may therefore show "x16" as the card's maximum while the negotiated link is what the hardware actually achieved, for example `x16 2.0 @ x8 2.0`. Always trust the value after `@` (or the `negotiated` field in `status`), not the maximum.

---

## Tested configuration

| Item | Value |
| --- | --- |
| GPU | NVIDIA P104-100 8 GB (`10DE:1B87`), GP104, GDDR5X |
| Hardware mod | AC-coupling capacitors added on the extra lanes needed for x8 |
| CPU | Intel Core i5-3470 (Ivy Bridge) |
| Board | Old H61 board, root port limited to x8 |
| NVIDIA driver | 582.99 |
| Result | Gen2 x8 negotiated and stable (GPU-Z: `PCIe x8 2.0 @ x8 2.0`) |

Only this one setup has been tested. Details such as the exact Windows build, board model and BIOS version are listed in the maintainer's test report (see the issue tracker).

---

## What is and is not verified

### Verified (on the configuration above)

- Gen2 speed unlock works.
- x8 width unlock works once the extra lanes are physically enabled, with the link negotiating Gen2 x8.
- `verify` (CUDA full-buffer and inverse check) and `bw` (CUDA bandwidth) run successfully at that link.
- The card remained usable in 3D workloads at Gen2 x8 in short tests.

### Not verified

- **x16.** The author's board only exposes an x8 root port, so `width 16` has never been tested on a real x16 link. On an x8-limited board the width is capped to the platform limit, so nothing about real x16 behaviour has been observed.
- **Other platforms.** Only an old Intel desktop platform has been tested. AMD systems, newer Intel platforms, PCH-attached slots, laptops and external enclosures are untested. The source avoids hard-coded capability offsets (they are found by walking the PCIe capability list) and the reset sequence is not Intel-specific, but the reset and retrain behaviour on other root ports is unproven.
- **P104-101** (`10DE:1BC7`). The code recognises it but it has not been tested.
- Cold-boot auto-start timing, long-term stability and other Windows versions.
- **Gen3.** Not supported. This tool targets Gen1 and Gen2 only.

If you have any of these, your test report is valuable.

---

## Requirements

- Windows 10 or 11, 64-bit, run from an elevated (Administrator) prompt.
- Exactly one supported P104 GPU in the system (P104-100 or P104-101). Multiple P104 cards are refused.
- The ACPI MCFG table must be present (used to reach PCI configuration space).
- NVIDIA driver with CUDA support (`verify` and `bw` use the CUDA driver API).
- The bundled Seavo driver (auto-installed on first elevated run; see [Getting the driver](#getting-the-driver)).
- A display that is not connected to the P104 (it has no display outputs). Use the iGPU or another card for output.

---

## Hardware prerequisite for more than x4

To train above x4, the extra lanes of the card must be physically enabled. On the author's card this was done by adding AC-coupling capacitors on the TX pairs of the extra lanes (two capacitors per lane, **0402, 220 nF, X7R, 16 V**). Four extra lanes are needed to go from x4 to x8, and twelve for x16.

> **Check your card before modifying it.** Some board-partner variants (for example certain ASUS and Inno3D P104 cards) reportedly ship with these extra lane capacitors already populated from the factory. If yours already has them, no soldering is needed — just verify continuity and that there are no shorts. A card without them stays at x4 no matter what the software writes, so this is the most common reason a width unlock does not take effect.

This project does not cover the soldering. It is delicate work, you can easily destroy a pad or a trace, and nothing here is an endorsement of any particular procedure. If you modify your card, do so at your own risk, and check for shorts and continuity with a multimeter before you power the card on.

---

## Getting the driver

P2UNLOCK talks to the hardware through a signed third-party kernel driver, `SvIoCtrlx64.sys`, which exposes physical-memory read/write. A hash-verified copy is **bundled** at [`drivers/SvIoCtrlx64.sys`](drivers/SvIoCtrlx64.sys), copied next to `P2UNLOCK.exe` by CMake, and included in the release ZIP.

On the first elevated run, P2UNLOCK automatically copies the bundled driver to:

```text
C:\ProgramData\P104-Gen2x8\tool\drivers\SvIoCtrlx64.sys
```

The tool still refuses to load any file whose SHA-256 does not match:

```text
9ade632e54c3f5e8c75eaf636f2910bad3a8a07d6f078e48c9641d3af1fe811c
```

You can verify both copies yourself:

```powershell
Get-FileHash .\drivers\SvIoCtrlx64.sys -Algorithm SHA256
Get-FileHash C:\ProgramData\P104-Gen2x8\tool\drivers\SvIoCtrlx64.sys -Algorithm SHA256
```

If you only grabbed the bare `P2UNLOCK.exe` without its `drivers\` folder, place `SvIoCtrlx64.sys` in either of those locations manually. Antivirus products commonly flag drivers of this type; see [Safety and security](#safety-and-security).

---

## Build

Requirements: CMake 3.20 or newer and a C++20 compiler.

```powershell
# Visual Studio 2022 (x64)
cmake --preset windows-msvc-x64
cmake --build build/windows-msvc-x64 --config Release
```

The built executable is at `build/windows-msvc-x64/apps/cli/Release/P2UNLOCK.exe`, with the bundled driver copied to `build/windows-msvc-x64/apps/cli/Release/drivers/SvIoCtrlx64.sys`.

The `linux-core` preset builds only the portable core library (link and plan logic); the tool itself is Windows-only:

```bash
cmake --preset linux-core
cmake --build build/linux-core
```

You are encouraged to build from source rather than trust a prebuilt binary. GitHub Actions builds both configurations on every push and pull request; each successful run attaches a `P2UNLOCK-windows-x64` artifact containing `P2UNLOCK.exe`, `drivers\SvIoCtrlx64.sys`, and a ready-to-extract `P2UNLOCK-windows-x64.zip`.

---

## Quick start

Run in an elevated prompt:

```text
P2UNLOCK.exe scan
P2UNLOCK.exe install
P2UNLOCK.exe width 8
P2UNLOCK.exe gen 2
P2UNLOCK.exe status
P2UNLOCK.exe verify
P2UNLOCK.exe bw
```

Typical order: `install` → `width 8` → `gen 2`. `install` only registers the service; the profile is created by `width` and/or `gen`, and each command saves the profile and applies it immediately.

Then check GPU-Z (use the render test, because the link can drop to a lower state at idle) and compare `negotiated` in `status` to what you expected.

---

## Commands

| Command | Description |
| --- | --- |
| `install` | Register and start the auto-start service. Does not change the link by itself. |
| `uninstall` | Remove the service and saved profile. Restart the PC to return fully to stock. |
| `scan` | Read-only inspection of the GPU and the upstream bridge (maximum and negotiated width and speed, training state). |
| `status` | Show the saved profile, the declared width and the negotiated width. |
| `width 8\|16` | Unlock the lane count (x8 or x16). |
| `gen 1\|2` | Set the link speed to Gen1 or Gen2. |
| `revert` | Clear the profile and restore the declaration to the negotiated width. |
| `verify` | Run a CUDA full-buffer and inverse check (4 MiB) after confirming both link ends are stable and matching. |
| `bw` | Measure CUDA bandwidth: device-to-host, host-to-device and device-to-device. |

Notes:

- `verify` refuses to run unless the GPU and its upstream port are at a stable, matching Gen1 or Gen2 link.
- `bw` uses a pinned host allocation (`cuMemHostAlloc`), selects the CUDA device by the P104's PCI bus/device/function (not simply the first device), uses 64 MiB buffers, one warm-up and three measured iterations, and reports the fastest result. Results are printed in MiB/s.
- Theoretical one-direction PCIe 2.0 bandwidth is roughly 2 GB/s for x4, 4 GB/s for x8 and 8 GB/s for x16. Real transfers are somewhat lower. A result close to the x4 figure on an x8 setup suggests the link did not actually widen.

---

## How it works

1. **Find the target.** The tool enumerates PCI devices, requires exactly one supported P104, reads the ACPI MCFG table, and finds the upstream bridge (root port) above the GPU. Both must be on the same PCI segment.
2. **Plan.** It reads the current state and builds a list of register writes (GPU configuration space, GPU BAR0 registers, and the upstream port's Link Control and Link Control 2). Link Control offsets come from a PCIe capability walk, not hard-coded constants. The plan is rejected if any register is in an unexpected state.
3. **Journal.** Before touching anything, the plan is saved to a transaction journal at `C:\ProgramData\P2UNLOCK\p104.state`, with before/after values for every write, so an interrupted run can be rolled back.
4. **Apply and retrain.** For a width change, the tool writes the width and policy registers, toggles the upstream bridge's Secondary Bus Reset, performs a PnP disable/enable of the GPU, and retrains the link. For a speed change, it writes the speed registers and sets the target speed on both ends before retraining.
5. **Verify.** The tool waits (up to about 10 seconds) for both ends to settle at the target generation and at the requested width or the accepted fallback, with a stable training state. If it does not settle, the operation fails with an error and the tool attempts to restore the previous state from the journal.
6. **Cosmetic declaration.** Finally it writes the declared maximum width (x16) so reporting tools show it.

The requested width is clamped to `min(GPU maximum, upstream maximum)`, so you cannot exceed what your slot supports.

If the link is already at the target, the tool does nothing and says so. If it is stuck at a lower width, it reports that the requested width "was not reached" without writing anything.

---

## Behaviour you should know about

- Changes are volatile. Nothing is written to the VBIOS or fuses. After a reboot the card returns to stock unless the service re-applies the saved profile. The service applies the saved profile when it starts and retries if an attempt fails, so the link may take a little while to come up after boot. Behaviour right after a cold boot has had limited testing.
- **Reverting.** `revert` clears the profile and restores the declaration. `uninstall` removes the service and profile. A restart returns the card fully to stock.
- **Overclock profiles may reset.** A width change makes Windows disable and re-enable the GPU, which can reset profiles from MSI Afterburner, RTSS and similar tools. If your OC profile is applied automatically at startup, it can race with the service. Re-apply it after the unlock finishes, or delay the OC tool's start. A `gen`-only change normally does not do a PnP cycle and usually does not reset OC profiles.
- **Do not run other P104 unlock tools at the same time.** The tool refuses to take over the driver device if another tool has it open.
- **Failure can drop the card off the bus.** If link training fails badly, the GPU may disappear until the PC is fully powered off and on again. Because the changes are volatile, a full power cycle recovers it.
- **Idle link state.** GPU-Z and similar tools may show a lower link while idle. Check under load.

---

## Troubleshooting

| Message or symptom | Likely cause and what to do |
| --- | --- |
| `Exactly one supported P104 GPU ... is required` | No P104 found, or more than one. Remove other P104 cards. |
| `Seavo driver SHA-256 does not match ...` | The driver file is wrong or modified. Restore the bundled `drivers\SvIoCtrlx64.sys` copy (hash listed above) to the install path. |
| `Seavo driver is missing ... no bundled copy was found next to P2UNLOCK.exe` | You downloaded the bare `.exe`. Download the release ZIP instead, or copy `SvIoCtrlx64.sys` next to `P2UNLOCK.exe` into a `drivers\` folder. |
| `Seavo device is already present; refusing to take over an existing driver` | Another tool is holding the driver. Close it, or stop its service, and retry. |
| `P104 is already at Gen2 x4; requested x8 was not reached; no writes were performed` | The link is stuck at x4. Most often a hardware issue: the extra lane capacitors are missing (some ASUS/Inno3D variants already have them from the factory — check yours first), one lane is dead or shorted, or the slot/riser only provides four lanes. |
| `Link did not settle at Gen2 x8 ...` | Training failed after the write. Check lane continuity and shorts and try another slot, then report the details (see [Report your results](#help-wanted-report-your-results)). The tool attempts to restore the previous state. |
| Link shows `@ x1` or very low after a hardware change | A coupling capacitor is shorted or a lane is dead. Power off and re-check with a multimeter before retrying. |
| `Both ends must be at a stable matching Gen1 or Gen2 link` (from `verify`) | The GPU and the upstream port disagree or are still training. Wait a moment and run `status`. |
| `Upstream link control is not in the expected enabled state` | The upstream port is in a state the tool does not expect. Please report your platform. |
| MCFG-related errors | The firmware table is missing or unusual. Please report your board and BIOS. |
| GPU-Z says `x16 ... @ x8 ...` | Expected. The first value is the declared maximum, the value after `@` is the real link. |
| OC settings lost after `width` | See [Behaviour you should know about](#behaviour-you-should-know-about). |

---

## Safety and security

Please understand what this tool does before you run it.

- It uses a kernel driver with physical-memory access. While loaded, that capability is available to any administrator-level process. This is the same class of risk as other "bring your own vulnerable driver" tools, and antivirus products will often flag it. Bundling the driver does not remove this risk.
- Mitigations in the code: the bundled driver's SHA-256 is verified before it is loaded; it is loaded through a temporary kernel service that is removed afterwards; and the tool refuses to take over a driver device that already exists.
- The bundled driver is provided for convenience only. You remain responsible for deciding whether to allow such a driver on your system; see the [Disclaimer](#disclaimer).
- **No network access.** The source contains no network code and does not download or launch other programs.
- **Service control channel.** The service listens on the named pipe `\\.\pipe\P2UNLOCK`, restricted to SYSTEM and Administrators.
- **Register writes are guarded.** Writes are planned and journaled with before/after values, unexpected register states are rejected, and failed operations are rolled back.
- Recommendations: build from source, verify the driver hash, run on a machine you can afford to troubleshoot, keep a way to display output (the iGPU) so you can recover, and run `uninstall` if you no longer need the unlock.

Nothing here guarantees safety on hardware that has not been tested. See the [Disclaimer](#disclaimer).

---

## Help wanted: report your results

The most useful thing you can do is to try it on a different setup and tell us what happened, whether it worked or not. A success on an untested platform is as valuable as a failure.

Especially wanted:

- A real x16 link (an x16 CPU-attached slot, with the lanes physically enabled).
- AMD platforms and newer Intel platforms.
- PCH-attached slots.
- P104-101 cards.
- Other Windows versions and cold-boot behaviour with the service.

Please open an issue and copy the [test report template](#test-report-template) at the end of this README. Include:

1. **System:** CPU, motherboard and BIOS version, chipset, slot used (CPU- or chipset-attached), Windows version and build, NVIDIA driver version.
2. **Card:** exact model (P104-100 or P104-101), whether the lanes were physically modified, and how many lanes you enabled.
3. **Commands run, in order, and which build you used.**
4. **Output (paste as text, not screenshots) of:**

   ```text
   P2UNLOCK.exe scan
   P2UNLOCK.exe status
   P2UNLOCK.exe verify
   P2UNLOCK.exe bw
   ```

5. A GPU-Z screenshot taken during the render test, showing the Bus Interface line.
6. The result: link achieved (for example `Gen2 x8`), whether it stayed stable under load, and any errors.
7. For failures: the exact error message, whether the card disappeared from Device Manager, and whether a full power cycle brought it back.

Hardware measurements with a multimeter (for example, continuity from the gold finger to each capacitor, and no shorts between pads) are very helpful when a lane does not come up.

---

## AI assistance and prior work

- This tool was written with heavy AI assistance. The maintainer used AI coding assistants for essentially all of the design and implementation, and for much of this documentation. It has not had an independent security or code review. Treat it accordingly: read the source, build it yourself, and do not assume the code is free of mistakes just because it works on one machine.
- It draws on existing P104 unlock projects. The approach, and parts of the register-level behaviour, were modelled on earlier community work rather than invented from scratch. Credit for the original research and discoveries belongs to those projects and their authors. If you believe a specific technique or piece of documentation here originates from your work, please open an issue so it can be credited with a link.
- The bundled `SvIoCtrlx64.sys` is an unmodified third-party driver sample from the public [LOLDrivers catalog](https://www.loldrivers.io/drivers/8d2f15a5-e71a-47ba-a391-55d1a1962368/); its SHA-256 was matched to the expected value before inclusion. It is not authored by this project and is bundled only so the tool works out of the box. Use at your own risk.
- Where this project differs, the differences (for example the transaction journal and rollback, the driver hash check, and the CUDA verification) were added by the maintainer with AI help and are covered only by the single-machine testing described above.

If you notice a behaviour that looks copied from, or in conflict with, another project's documentation, please open an issue so it can be credited or corrected.

---

## Disclaimer

This software modifies hardware configuration registers and is provided as is, without warranty. Use it only on hardware you own, and at your own risk. Hardware modifications can permanently damage your card, your motherboard or your data, and may void any warranty. The authors are not responsible for any damage or loss.

---

## License

Released under the [MIT License](LICENSE).

---

## Test report template

Copy this into a new issue. Paste command output as text, not screenshots. Success reports are as useful as failures.

<details>
<summary>Show template</summary>

### Result

- [ ] Worked: link reached the target and stayed stable
- [ ] Partly worked (describe below)
- [ ] Failed (error message below)
- [ ] Card disappeared from the system after the command

**Link achieved:** (for example `Gen2 x8`)  
**Stable under load?** (yes / no / not tested)

### System

- CPU:
- Motherboard and BIOS version:
- Chipset:
- Slot used (CPU-attached or chipset-attached, physical size):
- Windows version and build:
- NVIDIA driver version:
- Display connected to (iGPU / other card):

### Card

- Model: P104-100 / P104-101
- Were extra lanes physically enabled (AC-coupling capacitors)? How many lanes?
- Anything unusual (riser, adapter, external power, etc.)?

### Test details

- P2UNLOCK version / commit:
- Build used:
- Commands run (in order):
- `scan` output:
- `status` output:
- `verify` output:
- `bw` output:
- GPU-Z Bus Interface under render load:

### Result details

- Link achieved:
- Stable under load:
- Errors or warnings:
- Did the GPU disappear from Device Manager?
- Did a full power cycle recover it?

### Additional notes

Anything else that might help reproduce or understand the result.

</details>
