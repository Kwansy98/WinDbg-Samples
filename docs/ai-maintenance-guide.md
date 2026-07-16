# AI maintenance guide

本文档用于在新的 Codex 会话中快速接手本 fork。它不是用户教程，而是维护入口说明。

仓库根目录的 `AGENTS.md` 是 AI 的自动入口，包含项目身份、必读文档、已验证能力和不可破坏的设计边界。支持 `AGENTS.md` 的工具通常不需要维护者在每个新会话里重新粘贴背景；AI 应先按其中的要求完整阅读本文档和 `docs/vmware-exdi-kernel-debugging.md`。

## 手动开场提示

以下提示只用于不自动读取 `AGENTS.md` 的工具，或者需要提醒 AI 重新加载上下文时。

新开 Codex 并打开本目录后，可以直接发：

```text
先阅读 docs/ai-maintenance-guide.md 和 docs/vmware-exdi-kernel-debugging.md。
这是我维护的 WinDbg-Samples fork，目标是修复 VMware + EXDI 调试 Windows 内核的问题。
请先理解当前分支的设计、已修复问题、部署方式和验证方式，再开始改代码。
不要 push，除非我明确要求。不要把构建、部署、清理逻辑重新塞回 exdi_manager.py。
```

如果要继续分析剩余的 EXDI 上下文限制，可以发：

```text
继续分析 VMware EXDI 下 GS base、MSR 和 kdexts PteBase 的剩余问题。
先基于现有 NT base、KdDebuggerDataBlock、KTHREAD、虚拟地址读取、断点和单步能力收集实时证据。
先给出代码级原因和验证方案，再改代码。
```

如果要维护 GUI，可以发：

```text
维护根目录 exdi_manager.py。
这个 GUI 只允许做状态显示、启用 COM、停用 COM、启动 WinDbg、结束 WinDbg。
构建、复制部署、清理卸载都不属于这个 GUI。
```

## 当前分支

```text
fix/vmware-exdi-kernel-debugging
```

远程：

```text
origin   git@github.com:Kwansy98/WinDbg-Samples.git
upstream https://github.com/microsoft/WinDbg-Samples.git
```

关键提交：

```text
d98f21c Fix VMware EXDI kernel debugging
96d9928 Document VMware EXDI debugging workflow
b0a60e7 Add VMware EXDI manager GUI
a2d99db Hide EXDI manager launcher console
232bdcf Simplify VMware EXDI manager controls
```

## 先读文件

```text
docs/vmware-exdi-kernel-debugging.md
Exdi/exdigdbsrv/ExdiGdbSrv/LiveExdiGdbSrvServer.cpp
Exdi/exdigdbsrv/ExdiGdbSrv/LiveExdiGdbSrvServer.h
Exdi/exdigdbsrv/GdbSrvControllerLib/AsynchronousGdbSrvController.cpp
Exdi/exdigdbsrv/GdbSrvControllerLib/BufferWrapper.h
Exdi/exdigdbsrv/GdbSrvControllerLib/exdiConfigData.xml
exdi_manager.py
exdi_manager.bat
```

## 已完成能力

当前 fork 已经补齐 VMware + EXDI 的核心调试能力：

- WinDbg 可以通过 EXDI 连接 VMware GDB stub。
- EXDI server 可以获取 Windows NT base。
- WinDbg 可以加载 `ntkrnlmp.exe` 符号。
- 可以读取内核虚拟地址内存。
- 可以 `u nt!SwapContext`。
- 可以设置并命中 `bp nt!SwapContext`。
- 可以单步。
- 可以定位并返回真实的 `DBGKD_GET_VERSION64`。
- 可以在 server 内解码 Windows 10 19041 的 `KdDebuggerDataBlock`，不修改目标内存和 DbgEng。
- `!thread`、`!prcb`、`k` 和 `!process 0 0 System` 可以使用。
- `$thread` 会读取 `PRCB+8` 的真实 `CurrentThread`，不再把 `PRCB+0` 的 `MxCsr=0x1f80` 当成 KTHREAD。
- VMware 虚拟内存读取会按 processor 保存 CR3，并单独保存已确认的 kernel CR3。
- VMware 的 EXDI 对外物理读已接入同一条 `monitor phys` 分块读取路径，`!vtop` 和 `.process /p /r` 可以使用。
- `.process /p /r <EPROCESS>` 后可以读取目标进程 PEB、loader list、用户模块并执行 `.reload /user`。

## 仍需继续的问题

### VMware system context

VMware monitor 当前只提供 selector、CR0/CR2/CR3/CR4/CR8、GDTR 和 IDTR。它不提供 `fs_base`、`gs_base`、`k_gs_base` 或 MSR 读取，因此 `dg @gs` 仍显示 base 0，`rdmsr` 也得不到真实值。

不要伪造这些寄存器。继续修复前应先确认 VMware 是否存在未记录但可靠的 monitor 接口。

### kdexts PteBase

目标 KDBG 已正确返回 `PteBase=ffffc78000000000`，但当前 WinDbg Preview 的 kdexts `!pte` 仍使用旧的 `fffff68000000000` 基址族，因而会访问不可读的 `fffff6...` 地址。这是 kdexts 内部初始化问题，与 server 的 KDBG 解码和 CR3 页表翻译是两条路径。第三方 `ExdiHelper` 通过进程内 patch 修正它；本项目不采用这种方式。

### `SwapContext` 的 R3 地址空间

KDBG 已提供 `OffsetKThreadApcProcess` 和 `OffsetEprocessDirectoryTableBase`，所以 server 可以取得 `KTHREAD.ApcState.Process` 对应的当前附加进程 CR3。19041 上 `PsGetCurrentProcess` 读取 `KTHREAD+0xb8`，`PsGetCurrentThreadProcess` 读取 owner process `KTHREAD+0x220`。

但是在 `SwapContext` 入口，`PRCB.CurrentThread` 已是 incoming thread，硬件 CR3 和 `rdi` 仍属于 outgoing thread；到函数内部执行 `mov cr3` 后才重新一致。不要让 `ReadVirtualMemory` 自动尝试 current、owner 和硬件多个 CR3，这会在相同用户 VA 存在于多个进程时返回语义错误但表面成功的数据。

需要检查指定进程 R3 时使用：

```text
.process /p /r <EPROCESS>
!peb
.reload /user
```

这条路径依赖 `ReadPhysicalMemoryOrPeriphIO` 正确读取 VMware 物理内存；不要再次把它改回微软原版的通用 PA mode 路径。

后续分析时不要重新推翻 NT base 扫描、KDBG 解码、VMware monitor 同步、物理读、按 CPU 的 CR3 和虚拟地址翻译这些已经验证过的基础能力。

## GUI 边界

根目录 GUI 是日常操作入口，不是构建系统。

允许功能：

- 显示 DLL、PDB、配置、COM、WinDbg、VMware 日志状态。
- 启用 COM：写入 HKCU CLSID，使 WinDbg 加载 `local-install\x64\ExdiGdbSrv.dll`。
- 停用 COM：删除该 HKCU CLSID 注册。
- 启动 WinDbg：使用 `Kd=NTBaseAddr,DataBreaks=Exdi`。
- 结束 WinDbg：结束 WinDbg 和加载 `ExdiGdbSrv.dll` 的进程。

不允许塞回 GUI：

- 构建项目。
- 复制部署 DLL/PDB/XML。
- 删除部署目录。
- 清理源码或构建输出。
- 打开日志、打开文档这类非核心按钮。

构建和部署应该由 Codex、Visual Studio 或明确的开发命令完成。

## 常用验证

脚本状态：

```powershell
python .\exdi_manager.py --status
python -m py_compile .\exdi_manager.py
```

构建：

```powershell
& 'C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\MSBuild\Current\Bin\amd64\MSBuild.exe' `
  '.\Exdi\exdigdbsrv\ExdiGdbSrv.sln' `
  /m /nr:false `
  /p:Configuration=Release `
  /p:Platform=x64 `
  /p:PlatformToolset=v142 `
  /v:minimal
```

WinDbg 启动命令：

```powershell
windbgx.exe -v -kx exdi:CLSID={29f9906e-9dbe-4d4b-b0fb-6acf7fb6d014},Kd=NTBaseAddr,DataBreaks=Exdi
```

WinDbg 内验证：

```text
u nt!SwapContext
bp nt!SwapContext
g
p
r
k
!vtop <DirectoryTableBase> <virtual-address>
.process /p /r <EPROCESS>
!peb
.reload /user
```

## Git 规则

- 常规维护提交到 `fix/vmware-exdi-kernel-debugging`。
- 可以 fetch `upstream` 对比微软官方更新。
- 不要自动 merge upstream。先看官方是否改了 EXDI 相关文件，再决定如何重放本地修改。
- 不要 push，除非用户明确说 push。
- 不要向微软发 PR，除非用户明确要求。
