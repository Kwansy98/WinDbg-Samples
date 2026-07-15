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
VMware physical read bad reply
ReadVirtualMemory short read
```

含义：

- `ntbase scan source=idt`：正在从 IDT handler 向下扫描 NT base。
- `ntbase found`：已找到内核基址。
- `VMware physical read bad reply`：物理读返回长度或格式不符合预期。
- `ReadVirtualMemory short read`：页表翻译或物理读失败，WinDbg 可能显示 `??`。

## 已知未完成项

### KdDebuggerDataBlock

当前核心调试能力已经可用，但 `KdDebuggerDataBlock` 仍未解码。

可能仍看到：

```text
Unable to read debugger data block header
Unable to read KTHREAD address ...
!pte -> Unknown platform 0
```

影响范围：

- `!pte`
- `!process 0 0`
- 部分 kdexts
- 更完整的内核数据结构枚举

下一步建议优先做 server 侧修复：

- 找到 `KdVersionBlock` / `DebuggerDataList`。
- 在 EXDI server 内解码 `KdDebuggerDataBlock`。
- 通过 `ReadKdVersionBlock` 或 DbgEng 实际读取路径提供正确数据。

不要直接部署未经审计的第三方 WinDbg 扩展 DLL。

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
- KDBG 修复完成后额外验证 `!pte`、`!process 0 0`。

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

