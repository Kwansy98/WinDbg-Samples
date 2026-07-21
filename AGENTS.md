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
docs/exdi-manager.md
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
- Keep `exdi_manager.py` focused on portable runtime operation: strict sibling-artifact validation, HKCU COM registration, persistent target selection, read-only VMware snapshot polling, and per-session WinDbg start/stop. Do not put build, packaging, source cleanup, or source-maintenance logic into it.
- Manager-launched EXDI sessions are always out-of-process. The manager registers a dedicated AppID, starts its `dllhost.exe` with package-local XML environment variables, and launches WinDbgX without `InProc`. Do not add an InProc mode or fallback.
- The manager owns one configured `VMX + snapshot UID` and one active session. VMware GDB is fixed at `8864`, windbgskill is fixed at `26700`, and parallel sessions are deliberately unsupported.
- A system-wide named mutex permits only one GUI manager instance across copies of the package. CLI diagnostics remain available while the GUI is running.
- VM power-off and a change away from the selected snapshot trigger safe EXDI shutdown followed by a normal `WM_CLOSE` only for WinDbgX shells carrying the manager's unique launch signature. Same-snapshot restore is not observable from an unchanged snapshot UID alone.
- Automatic start is a persisted, default-enabled option and consumes one attempt per target-snapshot match. Automatic and manual starts share one serialized, idempotent entry point. If an active WinDbgX session disappears while the target still matches, clean it and report the stopped state without automatically restarting; GUI Start and Restart are explicit user actions. Mutating CLI actions are rejected while the GUI owns the watcher.
- Tk is owned exclusively by the GUI thread. Watcher notifications and worker results must enter the GUI through the thread-safe event queue; background threads must never call widgets or `after`. GUI refreshes use non-blocking watcher snapshots so lifecycle cleanup cannot freeze the window.
- A prestarted `dllhost.exe` may remain as an idle COM container after DbgEng releases EXDI. Distinguish it from an active EXDI object by an `Established` connection from that surrogate PID to VMware GDB port `8864`: an idle container may be terminated, but a surrogate that still owns the GDB connection must not be force-killed while the VM is running.
- Every GUI operation record is appended immediately to package-local UTF-8 `exdi_manager.log`; the release package does not include this runtime log.
- The canonical local deployment is the git-ignored `artifacts/VMwareEXDI` directory. `package_exdi_manager.ps1` updates fixed package files there while preserving runtime config and logs.
- Build and deployment remain explicit development operations described in the maintenance documents.
- Update both the relevant implementation and its persistent documentation when behavior or a known limitation changes.
- Do not push, rewrite history, merge upstream, or open a pull request unless the maintainer explicitly requests it.
