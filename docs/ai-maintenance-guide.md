# AI maintenance guide

本文档用于在新的 Codex 会话中快速接手本 fork。它不是用户教程，而是维护入口说明。

## 推荐开场提示

新开 Codex 并打开本目录后，可以直接发：

```text
先阅读 docs/ai-maintenance-guide.md 和 docs/vmware-exdi-kernel-debugging.md。
这是我维护的 WinDbg-Samples fork，目标是修复 VMware + EXDI 调试 Windows 内核的问题。
请先理解当前分支的设计、已修复问题、部署方式和验证方式，再开始改代码。
不要 push，除非我明确要求。不要把构建、部署、清理逻辑重新塞回 exdi_manager.py。
```

如果要继续调试 `KdDebuggerDataBlock`，可以发：

```text
继续分析 VMware EXDI 下 KdDebuggerDataBlock 读取失败的问题。
先基于现有 NT base、虚拟地址读取、断点和单步能力，定位 WinDbg 为什么仍提示 Unable to read debugger data block header。
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

## 仍需继续的问题

主要剩余问题是 `KdDebuggerDataBlock`：

```text
Unable to read debugger data block header
KdDebuggerDataBlock not available
```

这会影响部分依赖完整 KD debugger data block 的 WinDbg 命令，例如模块链表、部分扩展命令、页表扩展等。

后续分析时不要重新推翻 NT base 扫描、VMware monitor 同步、物理读和虚拟地址翻译这些已经验证过的基础能力。应该先查：

- `DBGKD_GET_VERSION64` 返回给 WinDbg 的字段是否完整。
- `KdVersionBlock` 地址是否正确。
- WinDbg 读取 debugger data block 时访问的虚拟地址是否被正确翻译。
- 目标 Windows 版本是否使用编码后的 `KdDebuggerDataBlock`。
- EXDI server 是否需要补充 WinDbg 期望的 `KDDEBUGGER_DATA64` 相关路径。

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
```

## Git 规则

- 常规维护提交到 `fix/vmware-exdi-kernel-debugging`。
- 可以 fetch `upstream` 对比微软官方更新。
- 不要自动 merge upstream。先看官方是否改了 EXDI 相关文件，再决定如何重放本地修改。
- 不要 push，除非用户明确说 push。
- 不要向微软发 PR，除非用户明确要求。

