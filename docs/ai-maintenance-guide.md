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

如果要维护便携式管理器，可以发：

```text
维护根目录 exdi_manager.py 和 docs/exdi-manager.md。
管理器只负责同目录运行依赖、HKCU COM 注册、单个用户目标、VMware 快照轮询和 Out-of-Process WinDbg 会话生命周期。
构建、打包、源码部署和清理都不属于运行时管理器。
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
docs/exdi-manager.md
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
- VMware legacy RSP 单步使用 `Hc<thread-id>` 选择处理器后发送独立的 `s` 包；普通 `bp` 命中后即使单步期间由另一处理器再次命中，也能完成断点绕过并重新插入。
- 可以定位并返回真实的 `DBGKD_GET_VERSION64`。
- 可以在 server 内解码 Windows 10 19041 的 `KdDebuggerDataBlock`，不修改目标内存和 DbgEng。
- `!thread`、`!prcb`、`k` 和 `!process 0 0 System` 可以使用。
- `$thread` 会读取 `PRCB+8` 的真实 `CurrentThread`，不再把 `PRCB+0` 的 `MxCsr=0x1f80` 当成 KTHREAD。
- VMware 虚拟内存读取会按 processor 保存 CR3，并单独保存已确认的 kernel CR3。
- VMware 的 EXDI 对外物理读已接入同一条 `monitor phys` 分块读取路径，`!vtop` 和 `.process /p /r` 可以使用。
- `.process /p /r <EPROCESS>` 后可以读取目标进程 PEB、loader list、用户模块并执行 `.reload /user`。
- AMD64 虚拟 DR6 按 stop reply 的命中处理器返回，不会把一个 watchpoint 命中广播给所有处理器。

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

## 便携式管理器边界

根目录 `exdi_manager.py` 是打包源，也是日常 GUI 入口；发布后只识别脚本同目录的运行文件，不识别源码树、`Release` 或 `local-install`。

允许功能：

- 严格验证同目录 EXDI/windbgskill DLL、PDB 和两个 XML。
- 自动把当前用户的固定 CLSID 指向同目录 `ExdiGdbSrv.dll`。
- 第一次运行枚举 VMware inventory 和 `.vmsd`，把用户选择的一个精确 VMX 和快照 UID 保存到同目录 `exdi_manager.json`。
- 每 2 秒只读轮询运行虚拟机和当前快照；配置中的自动启动开关默认开启，也可关闭后只用手动启动。
- VMware GDB 固定使用 `8864`，windbgskill 固定使用 `26700`；不支持多目标或并行会话。
- GUI 操作记录必须同步追加到发布目录的 UTF-8 `exdi_manager.log`；发布包本身不携带运行时日志。
- 安全停止会话：查询状态、必要时中断、`bc *`、`q`，确认 COM 对象的 `FinalRelease` 已执行且 windbgskill 进入 `no_target`。
- Out-of-Process 异步通知：不创建“跨 apartment 回调自己”的通知线程。COM STA 自己的 100ms timer 轮询异步 GDB 命令完成和 keepalive，因此 `FinalRelease` 可以直接停 timer、清理虚拟数据断点并释放 GDB controller，不存在通知线程与 STA 相互等待的析构环。
- GUI 线程模型：Tk 只允许主线程访问。watcher 通知和按钮任务结果只能写入线程安全队列，由主线程定时消费；界面刷新使用 watcher 的非阻塞快照，安全停止等慢操作不能冻结窗口。

所有管理器会话必须使用 Out-of-Process COM。管理器为固定 CLSID 关联专用 AppID，带发布目录 XML 环境变量启动对应 `dllhost.exe`，随后启动不含 `InProc` 参数的 WinDbgX。不要添加 InProc 模式、模式切换或 fallback。

`windbgskill` 提供自动化控制入口，并用于停止 WinDbg 时显式关闭 EXDI 会话；它不参与虚拟 DR、GDB watchpoint 或条件判定，这些调试能力必须在不加载扩展时也成立。

不要在存在活动 `ba` 时直接强制结束 WinDbg。VMware GDB stub 不会在客户端异常消失后自动删除 `Z2`/`Z4`，下一次连接会得到 `bl` 为空但持续 `SIGTRAP` 的幽灵 watchpoint。新版 WinDbg 的 `DbgX.Shell.exe` 可能承载多个标签，管理器不得用进程终止代替 DbgEng 的正常 `q`。

管理器检测到虚拟机关机或当前快照 UID 离开目标后，必须依次完成 windbgskill 中断、`bc *`、`q`、按管理器专用命令行签名向自己启动的 WinDbgX 发送 `WM_CLOSE`、等待 `FinalRelease` 断开 GDB 连接，最后清理空闲 surrogate。WinDbgX 已经异常消失时，预启动的 surrogate 可能作为空闲 COM 容器继续存在；必须以该 PID 是否仍保持到 VMware GDB 8864 的 `Established` 连接区分活动 EXDI 对象与空容器。无 GDB 连接的空容器可以结束，仍有 GDB 连接时不得强杀。禁止按进程名批量关闭或强制终止活动调试进程。相同快照恢复前后 UID 不变，不能宣称仅靠 `.vmsd` 轮询可以识别。

自动启动和手动启动必须汇合到同一个串行、幂等入口。自动启动对同一次目标快照匹配只尝试一次；手动停止后不再自动拉起，但手动启动仍可用。曾经活动的 windbgskill 会话若意外消失，管理器只清理并提示，不自动重启，因为无法区分用户主动关闭与进程崩溃。GUI 运行时，修改会话的 CLI 必须拒绝执行，避免和 watcher 并发操作。

不允许塞回 GUI：

- 构建项目。
- 制作或复制发布包。
- 删除发布目录或运行时配置。
- 清理源码或构建输出。
- 打开日志、打开文档这类非核心按钮。

构建由 Visual Studio/MSBuild 完成，发布包由根目录 `package_exdi_manager.ps1` 显式生成。

默认本机部署目录固定为 git-ignored 的 `artifacts\VMwareEXDI`。打包脚本可重复覆盖固定发布文件，但必须保留该目录中的 `exdi_manager.json` 和 `exdi_manager.log`。不要再把日常部署放到相邻的 `autowindbg` 项目。

## 常用验证

脚本状态：

```powershell
python .\exdi_manager.py status
python .\exdi_manager.py start
python .\exdi_manager.py stop
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
windbgx.exe -v `
  -kx "exdi:CLSID={29f9906e-9dbe-4d4b-b0fb-6acf7fb6d014},Kd=NTBaseAddr,DataBreaks=Default" `
  -c ".load windbgskill.dll; !windbgskill start 127.0.0.1 26700"
```

WinDbg 内验证：

```text
u nt!SwapContext
bp nt!SwapContext
g
p
r
k
ba w 8 /w "1 == 0" <frequently-written-address>
g
<break manually and replace the condition with "1 == 1">
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
