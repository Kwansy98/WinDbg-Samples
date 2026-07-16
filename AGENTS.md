# WinDbg-Samples VMware EXDI maintenance instructions

## Communication

- Always communicate with the maintainer in Chinese.
- Treat this repository as a personal open-source project: prefer clear, coherent, long-term design over compatibility layers, temporary fallbacks, or patchwork fixes.
- When a design decision would change an established boundary, explain the conflict and discuss it with the maintainer before editing code.

## Project identity

This repository is a fork of `microsoft/WinDbg-Samples`. The maintained branch is:

```text
fix/vmware-exdi-kernel-debugging
```

The fork makes WinDbg kernel debugging work through an EXDI COM server connected to the VMware GDB stub. `ExdiGdbSrv.dll` is the EXDI COM server loaded by DbgEng; it is not a WinDbg extension. Third-party `ExdiGdbSrv-master` and `ExdiHelper-master` repositories are research references only and are not dependencies.

## Read before working

Before analyzing, editing, building, or operating WinDbg, read these files completely:

```text
docs/ai-maintenance-guide.md
docs/vmware-exdi-kernel-debugging.md
```

They are the persistent source of truth for architecture, verified behavior, deployment, live evidence, and known limitations. Do not reconstruct the project history from assumptions when these documents already contain it.

If code, documentation, and live behavior disagree, inspect the code and collect live evidence first, then update the persistent documentation as part of the same change.

## Verified capabilities

Do not redesign or remove these capabilities unless live evidence proves a regression:

- Locate the Windows NT image base and load kernel symbols.
- Read kernel virtual memory, disassemble code, set and hit breakpoints, continue, and single-step.
- Locate and return the real `DBGKD_GET_VERSION64` / `KdVersionBlock`.
- Decode the encoded Windows 10 19041 `KdDebuggerDataBlock` in the EXDI server without modifying guest memory or patching DbgEng.
- Obtain PRCB, `CurrentThread`, and KTHREAD correctly; the former `KTHREAD=0x1f80` failure is fixed.
- Track CR3 per processor and keep a confirmed kernel CR3 for kernel virtual reads.
- Read VMware physical memory through the synchronized `monitor phys` path.
- Support `!vtop`, `!thread`, `!prcb`, `!process`, kernel stacks, and explicit user-process inspection through `.process /p /r`.

## Known boundaries

- KDBG contains the correct `PteBase`, but the current kdexts `!pte` command retains an obsolete private PTE base. Do not treat this as a KDBG or page-table-translation regression.
- VMware monitor has no confirmed reliable interface for `FS_BASE`, `GS_BASE`, `KERNEL_GS_BASE`, or general MSR reads. Do not fabricate those values.
- At the entry of `SwapContext`, the incoming KTHREAD and the hardware CR3 can legitimately describe different processes until the context switch executes `mov cr3`. Do not make virtual reads guess across current, owner, incoming, or hardware CR3 values.
- Some kdexts commands have private initialization assumptions and must be investigated command by command.
- Do not patch DbgEng or kdexts, load unreviewed third-party extensions, or introduce a `!rox`-style solution without first discussing that architectural choice with the maintainer.

## Working rules

- For WinDbg investigations, collect live evidence before changing code. If `windbgskill` is available, use it for read-only inspection and controlled debugger commands.
- Preserve the distinction between the EXDI COM server and optional WinDbg extensions.
- Keep `exdi_manager.py` limited to status display, COM enable/disable, and WinDbg start/stop. Do not put build, deployment, cleanup, or source-maintenance logic into it.
- Build and deployment remain explicit development operations described in the maintenance documents.
- Update both the relevant implementation and its persistent documentation when behavior or a known limitation changes.
- Do not push, rewrite history, merge upstream, or open a pull request unless the maintainer explicitly requests it.
