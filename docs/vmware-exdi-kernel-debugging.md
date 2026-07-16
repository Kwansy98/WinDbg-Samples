# VMware EXDI kernel debugging fixes

本文档是本地 fork 的 PR 说明草案和维护记录。当前分支并未向上游提交 PR，但按开源项目的提交方式组织：功能分支、聚焦的代码提交、独立文档说明和可复现的验证步骤。

## PR 草案

标题：

```text
Fix VMware EXDI kernel debugging support
```

摘要：

- Implement `Kd=NTBaseAddr` support for VMware-backed EXDI sessions.
- Fix VMware monitor command synchronization.
- Add reliable VMware physical memory reads and x64 virtual-to-physical translation.
- Fix buffer growth for long monitor responses.
- Adjust AMD64 data breakpoint command generation.
- Document local deployment, validation, and known remaining gaps.

测试：

```powershell
& 'C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\MSBuild\Current\Bin\amd64\MSBuild.exe' `
  'D:\All\project\mycode\WinDbg-Samples\Exdi\exdigdbsrv\ExdiGdbSrv.sln' `
  /m /nr:false `
  /p:Configuration=Release `
  /p:Platform=x64 `
  /p:PlatformToolset=v142 `
  /v:minimal
```

实机验证：

```text
windbgx.exe -v -kx exdi:CLSID={29f9906e-9dbe-4d4b-b0fb-6acf7fb6d014},Kd=NTBaseAddr,DataBreaks=Exdi

u nt!SwapContext
bp nt!SwapContext
g
p
r
k
```

期望现象：

- WinDbg 能连接 VMware GDB stub。
- EXDI server 能找到 NT base。
- WinDbg 能加载 `ntkrnlmp.exe` 符号。
- `u nt!SwapContext` 显示真实指令，不再是 `??`。
- `bp nt!SwapContext` 能命中。
- `p` 能单步。

## 仓库结构

当前推荐维护目录：

```text
D:\All\project\mycode\WinDbg-Samples
```

远程仓库：

```text
origin   git@github.com:Kwansy98/WinDbg-Samples.git
upstream https://github.com/microsoft/WinDbg-Samples.git
```

当前开发分支：

```text
fix/vmware-exdi-kernel-debugging
```

该分支基于微软官方 `master`：

```text
1b0b2f3 Add a new section to call out projects that use TTD's APIs (#136)
```

## 与第三方项目的关系

本 fork 的代码修改直接落在微软官方 `WinDbg-Samples` 源码树中。

以下项目只用于理解问题和对照行为，没有被编译、部署或作为依赖引入：

```text
D:\All\project\mycode\study\ExdiGdbSrv-master
D:\All\project\mycode\study\ExdiHelper-master
```

关系说明：

- `ExdiGdbSrv-master` 提供了 VMware EXDI 问题方向参考，例如 monitor 命令、物理内存读、NT base、断点等。
- `ExdiHelper-master` 用于理解 Win8+ `KdDebuggerDataBlock` 编码问题和 kdexts `PteBase` 问题。
- 当前稳定分支不编译、不加载、不信任第三方 DLL。

## 已修复的问题

### 1. NT base 获取

微软原版对 VMware 场景没有实现 `DBGENG_EXDI_IOCTL_V3_GET_NT_BASE_ADDRESS_VALUE` 的有效返回。

原始现象：

```text
Unable to get the NT base address from the Exdi Server
Module List address is NULL
KdDebuggerDataBlock not available
```

当前实现：

```text
GetContextEx
  -> VMware monitor 读取 CR3 / IDTR
  -> 读取 IDT[0] handler
  -> 从 handler 所在 64K 对齐地址向低地址扫描
  -> 手动页表翻译并读取内核 PE header
  -> 验证 DOS/NT header、AMD64、native subsystem、section 布局
  -> 返回 ntoskrnl/ntkrnlmp image base
```

相关文件：

```text
Exdi/exdigdbsrv/ExdiGdbSrv/LiveExdiGdbSrvServer.cpp
Exdi/exdigdbsrv/ExdiGdbSrv/LiveExdiGdbSrvServer.h
```

### 2. VMware monitor 回包同步

VMware `qRcmd` monitor 命令通常先返回 `O...` 输出包，再返回最终 `OK`。

如果只消费第一包，下一条命令可能读到上一条命令残留的 `OK`，导致寄存器读取、内存模式切换、物理读等状态错位。

当前实现会：

- 发送 `qRcmd,<hex command>`。
- 解码 `O...` 输出。
- 等待最终 `OK`。
- 丢弃与当前命令不匹配的旧响应。

### 3. VMware 物理内存读

原版 VMware 路径没有可靠的物理内存读。当前实现：

```text
monitor phys
m<physical_address>,<size>
monitor virt
```

并且每次物理读按 `0x100` 字节分块，避免 VMware GDB stub 或 RSP packet 长度限制导致大块读取被截断。

这个细节很重要：曾经一次读取 `1280` 字节 section headers 时，VMware 只返回了部分数据，导致 NT base 扫描误判失败。分块读取后不再依赖单次大包。

内部页表遍历和 EXDI 对外的 `ReadPhysicalMemoryOrPeriphIO` 必须使用同一条 VMware 物理读取路径。只修内部页表遍历时，普通虚拟地址读取虽然可用，但 WinDbg 自己发起的物理页表读取仍会失败，表现为：

```text
!vtop ...
PML4E read error 0x8007001E
```

对外物理读接入 `ReadVMwarePhysicalMemory` 后，`!vtop` 和 `.process /p /r` 均可使用。

### 4. x64 虚拟内存读

当前 VMware 路径会根据 CR3 手动翻译 x64 虚拟地址：

```text
VA
  -> PML4
  -> PDPT
  -> PD
  -> PT
  -> PA
  -> VMware physical read
```

支持：

- 4KB 页
- 2MB large page
- 1GB large page

这使得以下命令能正常读取内核代码：

```text
u nt!SwapContext
db nt!SwapContext
```

### 5. BufferWrapper 自动扩容

原版 `BufferWrapper::SetLength` 假设调用方已经扩容：

```text
assert(newLength <= m_capacity)
```

VMware monitor 输出较长时会触发断言。当前 `SetLength` 在长度超出容量时会自动扩容，避免调试会话被断言弹窗中断。

相关文件：

```text
Exdi/exdigdbsrv/GdbSrvControllerLib/BufferWrapper.h
```

### 6. AMD64 数据断点参数

VMware/GDB stub 对 AMD64 watchpoint 命令的访问类型和宽度语义与原实现不匹配。

当前处理：

- AMD64 下 `daRead` 调整为 `daBoth`。
- access width 从 bit 语义换算为 byte 语义。

相关文件：

```text
Exdi/exdigdbsrv/GdbSrvControllerLib/AsynchronousGdbSrvController.cpp
```

### 7. 本地 VMware 默认配置

本地分支将 EXDI 配置默认指向 VMware：

```xml
<ExdiTargets CurrentTarget = "VMWare">
```

VMware target 默认连接：

```text
localhost:8864
```

并关闭通信刷屏：

```xml
displayCommPackets = "no"
gdbMonitorCmdDoNotWaitOnOK = "no"
```

启动脚本默认使用：

```text
Kd=NTBaseAddr,DataBreaks=Exdi
```

注意：这些配置适合本地维护分支。如果未来真的向微软提交 PR，应避免改变官方默认 `CurrentTarget` 和本地端口，改成文档说明或可选配置。

### 8. KdVersionBlock 和 KdDebuggerDataBlock

Windows 10 19041 的 `KdVersionBlock` 本身可直接读取，但 `KdpDataBlockEncoded=1`，所以目标内存中的 `KdDebuggerDataBlock` 原始字节不是明文 `KDBG`。直接把这段内存交给 DbgEng 会导致：

```text
Unable to read debugger data block header
KdDebuggerDataBlock not available
Module List address is NULL
```

当前 server 会：

1. 在已确认的 NT `.data` section 中定位 `DBGKD_GET_VERSION64`。
2. 验证 `KernBase`、`PsLoadedModuleList` 和 `DebuggerDataList`。
3. 从 `DebuggerDataList` 找到编码的 KDBG。
4. 使用 list head、`KDBG` owner tag、block size 和 `KernBase` 约束，唯一推导编码旋转量和 key。
5. 缓存解码后的 block，并只在 EXDI 返回与 KDBG 重叠的内存读取时覆盖返回缓冲区。
6. 通过 `ReadKdVersionBlock` 返回真实的 `DBGKD_GET_VERSION64`。

这个实现不写目标内存，也不 patch DbgEng 或 kdexts。Windows 10 19041 实机验证值为：

```text
KdVersionBlock       = fffff8005e80f3a0
DebuggerDataList     = fffff8005e8406b0
KdDebuggerDataBlock = fffff8005e800b20
OwnerTag / Size      = KDBG / 0x380
KernBase             = fffff8005dc00000
```

### 9. KTHREAD 与按 CPU 的 CR3

原来的 `Unable to read KTHREAD address 0000000000001f80` 不是 NT base 扫描错误。实时证据表明：

```text
PRCB+0 = MxCsr 0x1f80，加上相邻的 processor metadata
PRCB+8 = CurrentThread
```

KDBG 不可用时，DbgEng 没有取得 `OffsetPrcbCurrentThread=8`，实际按偏移 0 读取，于是把 `MxCsr=0x1f80` 当成 KTHREAD 地址。KDBG 修复后：

```text
$thread == poi(@$prcb+8)
```

并且 `!thread`、`!prcb` 和 `k` 在 `nt!KiPageFault` 断点上均可正常工作。

VMware 虚拟地址读取也不再使用一个全局 `m_lastCr3`。当前实现按 processor 保存最近的 CR3，并在 ring 0 context 中更新独立的 kernel CR3。`~Ns` 切换 processor 后，寄存器、PCR、PRCB、KTHREAD 和 CR3 会随 processor context 同步。

### 10. R3 地址空间与 `SwapContext`

Windows 10 19041 的现场反汇编确认：

```text
PsGetCurrentProcess:
    mov rax, gs:[188h]
    mov rax, [rax+0B8h]

PsGetCurrentThreadProcess:
    mov rax, gs:[188h]
    mov rax, [rax+220h]
```

因此 `KTHREAD+0xb8` 是 `ApcState.Process`，表示当前附加进程；`KTHREAD+0x220` 是 owner process。server 不需要硬编码当前附加进程相关的结构偏移，解码后的 KDBG 已提供：

```text
KiProcessorBlock
OffsetPrcbCurrentThread              = 0x8
OffsetKThreadApcProcess              = 0xb8
OffsetEprocessDirectoryTableBase     = 0x28
```

可以沿以下路径得到 WinDbg 当前逻辑线程的附加进程 CR3：

```text
KiProcessorBlock[cpu]
  -> PRCB.CurrentThread
  -> KTHREAD.ApcState.Process
  -> EPROCESS.DirectoryTableBase
```

但这不能直接替换硬件 CR3。`SwapContext` 入口存在正常的调度器过渡窗口：`PRCB.CurrentThread` 和 `rsi` 已指向 incoming KTHREAD，而硬件 CR3 及 `rdi` 仍属于 outgoing KTHREAD。函数稍后从 incoming `ApcState.Process` 读取 `DirectoryTableBase`，到 `SwapContext+0x3de` 才执行 `mov cr3, rcx`。

此时同一个 EXDI `ReadVirtualMemory(Address, Size)` 调用无法区分 WinDbg 是要读取 incoming PEB，还是要展开 outgoing 用户栈。自动尝试多个 CR3 会在相同用户 VA 同时存在于两个进程时静默返回错误进程的数据，因此当前实现不采用 CR3 猜测或 fallback。

正确做法是保留硬件 CR3 作为处理器执行地址空间，并通过 WinDbg 显式选择需要检查的进程：

```text
.process /p /r <EPROCESS>
!peb
.reload /user
```

部署后现场验证：

```text
!vtop 13b130000 ed64d66000
Virtual address ed64d66000 translates to physical address 13804e000.

.process /p /r ffffa28c2ee31080
Implicit process is now ffffa28c`2ee31080
.cache forcedecodeuser done
Loading User Symbols
```

随后 `db <PEB>`、`!peb` 和 `lm u` 均能完整读取 WmiPrvSE 的 PEB、loader list 与用户模块。在 `SwapContext` 的 incoming `csrss.exe` 上重复 `.process /p /r` 也能读取其 PEB 和模块链。由此确认原来的 `.process` 失败来自 EXDI 对外物理读取路径缺失，不是 Windows 没有提供当前进程信息。

## 部署方式

构建后手动复制：

```powershell
$repo = 'D:\All\project\mycode\WinDbg-Samples'
$install = Join-Path $repo 'Exdi\exdigdbsrv\local-install\x64'
$release = Join-Path $repo 'Exdi\exdigdbsrv\Release\x64'

Copy-Item -Force -LiteralPath (Join-Path $release 'ExdiGdbSrv.dll') -Destination (Join-Path $install 'ExdiGdbSrv.dll')
Copy-Item -Force -LiteralPath (Join-Path $release 'ExdiGdbSrv.pdb') -Destination (Join-Path $install 'ExdiGdbSrv.pdb')
Copy-Item -Force -LiteralPath (Join-Path $repo 'Exdi\exdigdbsrv\GdbSrvControllerLib\exdiConfigData.xml') -Destination $install
Copy-Item -Force -LiteralPath (Join-Path $repo 'Exdi\exdigdbsrv\GdbSrvControllerLib\systemregisters.xml') -Destination $install
```

注册 COM DLL：

```powershell
$clsid = '{29f9906e-9dbe-4d4b-b0fb-6acf7fb6d014}'
$key = "HKCU:\Software\Classes\CLSID\$clsid\InprocServer32"
Set-Item -Path $key -Value (Join-Path $install 'ExdiGdbSrv.dll')
New-ItemProperty -Path $key -Name 'ThreadingModel' -Value 'Apartment' -PropertyType String -Force | Out-Null
```

替换 DLL 前建议结束 WinDbg：

```powershell
Get-Process -Name windbgx,WinDbgX,DbgX.Shell,windbg -ErrorAction SilentlyContinue |
  Stop-Process -Force -ErrorAction SilentlyContinue

$loaded = tasklist /m ExdiGdbSrv.dll 2>$null | Select-String -Pattern '^\S+\s+(\d+)\s'
foreach ($line in $loaded) {
    if ($line.Line -match '^\S+\s+(\d+)\s') {
        Stop-Process -Id ([int]$Matches[1]) -Force -ErrorAction SilentlyContinue
    }
}
```

## VMware 配置

虚拟机 `.vmx` 至少需要：

```text
debugStub.listen.guest64 = "TRUE"
monitor.debugOnStartGuest64 = "FALSE"
```

当前本地 XML 使用：

```text
localhost:8864
```

如果 VMware 端口改变，需要同步修改：

```text
Exdi/exdigdbsrv/GdbSrvControllerLib/exdiConfigData.xml
Exdi/exdigdbsrv/local-install/x64/exdiConfigData.xml
```

## 日志

VMware 专用日志：

```text
%TEMP%\ExdiGdbSrv-vmware.log
```

常见日志：

```text
ntbase scan source=idt
ntbase found
KdVersionBlock found
KdDebuggerDataBlock decoded
VMware physical read bad reply
ReadVirtualMemory short read
```

含义：

- `ntbase scan source=idt`：正在从 IDT handler 向下扫描 NT base。
- `ntbase found`：已找到内核基址。
- `KdVersionBlock found`：已定位并验证 `DBGKD_GET_VERSION64`。
- `KdDebuggerDataBlock decoded`：已唯一推导编码参数并缓存明文 KDBG。
- `VMware physical read bad reply`：物理读返回长度或格式不符合预期。
- `ReadVirtualMemory short read`：页表翻译或物理读失败，WinDbg 可能显示 `??`。

## 已知未完成项

### GS base 和 MSR

VMware monitor 的 `help r` 只列出 selector、CR0/CR2/CR3/CR4、GDTR、IDTR 和 LDTR 等寄存器，没有 `fs_base`、`gs_base`、`k_gs_base` 或通用 MSR 接口。当前表现为：

```text
dg @gs       -> base 0
rdmsr ...    -> 无真实值
```

这不是当前 KTHREAD 问题的原因；KTHREAD 已通过 KDBG 中的 PRCB offset 修复。除非能确认 VMware 存在可靠接口，否则不应伪造这些值。

### kdexts PteBase

KDBG 中的真实 `PteBase` 已解码为：

```text
ffffc78000000000
```

但当前 WinDbg Preview 的 kdexts `!pte` 仍使用旧的 `fffff68000000000` 基址族，并尝试读取不可用的 `fffff6...` 地址。这是 kdexts 自己维护的 PteBase 状态，不是 EXDI server 返回的 KDBG 内容错误。

第三方 `ExdiHelper` 通过扫描并修改 kdexts 进程内变量解决该问题。本 fork 不部署未经审计的第三方 DLL，也不在 server 中 patch WinDbg 进程。

### 部分扩展命令

`!process 0 0 System`、`!thread`、`!prcb`、`k` 和模块枚举已经可用。`!vm 1` 可以返回主要统计，但仍可能报告个别 symbol/global 无法读取；这类问题需要按具体命令继续区分 kdexts 内部假设、缺失 system context 和真实页表读取限制。

## WinDbg Preview 更新风险

存在后续 WinDbg Preview 更新后失效的风险。

相对稳定的部分：

- COM 加载 EXDI server。
- `IeXdiServer3` / `IeXdiX86_64Context3` 基本接口。
- GDB RSP 寄存器、内存、断点命令。
- VMware debug stub 的 `qRcmd` monitor 命令。

风险较高的部分：

- `Kd=NTBaseAddr` 触发的 DbgEng EXDI IOCTL 行为。
- `DBGENG_EXDI_IOCTL_V3_*` 调用时机和结构。
- DbgEng 对 `KdVersionBlock` / `KdDebuggerDataBlock` 的读取路径。
- kdexts 内部 `PteBase` 初始化逻辑。

维护建议：

- 记录验证过的 WinDbg 版本。
- 更新 WinDbg 后先验证 `u nt!SwapContext`、`bp/g/p`。
- 验证 `dq nt!KdDebuggerDataBlock L4` 返回 `KDBG/0x380`。
- 验证 `$thread == poi(@$prcb+8)`、`!thread`、`!prcb` 和 `!process 0 0 System`。
- 将 `!pte` 单独视为 kdexts PteBase 问题，不要据此否定 server 的 KDBG 修复。

## 同步上游

本地维护推荐保留两个 remote：

```text
origin   -> user fork
upstream -> microsoft/WinDbg-Samples
```

同步微软官方更新：

```powershell
git fetch upstream
git switch master
git merge upstream/master
git switch fix/vmware-exdi-kernel-debugging
git rebase master
```

如遇冲突，优先保护 VMware EXDI 修复逻辑，再重新构建验证。
