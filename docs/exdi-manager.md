# VMware EXDI Manager

这是一个可放在任意目录运行的单目标 VMware EXDI 管理器。它监视用户选定的一个精确 `VMX + 快照 UID`，可按持久配置自动启动 WinDbgX，也可由用户手动启动；两条路径都会建立同一种 EXDI 内核调试会话并加载 `windbgskill`。

设计有意保持单一：

- 只保存一个虚拟机快照，不支持并行会话。
- VMware GDB Stub 固定为 `127.0.0.1:8864`。
- windbgskill 固定为 `127.0.0.1:26700`。
- 只启动 `%LOCALAPPDATA%\Microsoft\WindowsApps\WinDbgX.exe`。
- EXDI 只使用 Out-of-Process COM server，不存在 InProc 或 fallback。

管理器源码和打包脚本属于 WinDbg-Samples fork，因为它们依赖同一仓库中维护的 EXDI DLL、XML、调试语义和安全清理约定。源码保留在仓库根目录，实际运行的发布包放在仓库已忽略的独立目录：

```text
WinDbg-Samples\artifacts\VMwareEXDI
```

运行时代码只根据 `exdi_manager.py` 所在目录定位 DLL、PDB、XML、配置和日志，因此整个发布目录复制到其他机器或其他路径后仍然有效，不依赖源码树的相对位置。

## 发布目录

运行时文件全部来自脚本所在目录：

```text
ExdiGdbSrv.dll
ExdiGdbSrv.pdb
windbgskill.dll
windbgskill.pdb
exdiConfigData.xml
systemregisters.xml
exdi_manager.py
exdi_manager.bat
README.md
LICENSE-WinDbg-Samples.txt
Third-Party-Notices.txt
LICENSE-windbgskill.txt
```

DLL、PDB 和 XML 缺少任意一个时，管理器都会直接报错，不搜索源码树、构建目录或系统安装目录。

`exdi_manager.json` 是首次运行后生成的用户配置，因此发布包不携带它。

`exdi_manager.log` 也是运行时文件。管理器每次运行时在同目录创建或追加它，发布包不预置该文件。

## 使用前提

- Windows 已安装 Python 3、Python Launcher 和 tkinter。
- 已安装 VMware Workstation，且虚拟机出现在 VMware inventory 中。
- 已安装新版 WinDbg；`WinDbgX.exe` 必须存在于 WindowsApps 别名目录。
- 目标 VMX 已启用 VMware GDB Stub。未显式指定端口时 VMware 使用 `8864`。

VMX 至少需要：

```text
debugStub.listen.guest64 = "TRUE"
monitor.debugOnStartGuest64 = "FALSE"
debugStub.hideBreakpoints = "TRUE"
```

## 首次运行

双击 `exdi_manager.bat`。管理器会：

1. 验证同目录运行文件、WinDbgX 和 `vmrun.exe`。
2. 枚举 VMware inventory 及各虚拟机的 `.vmsd` 快照。
3. 要求用户选择一个精确快照。
4. 将 VMX 路径、快照名称、快照 UID、内存快照标记和自动启动开关写入同目录 `exdi_manager.json`。
5. 每 2 秒只读检查该 VMX 是否运行、当前快照 UID 是否匹配，以及 GDB 8864 监听进程的 VMX PID 是否换代。

同一台机器只允许一个管理器 GUI。重复双击 BAT 时，第二个进程会提示管理器已经运行并立即退出；不同目录中的发布包也使用同一个系统级 mutex，不会绕过单实例限制。GUI 运行时命令行仍可执行只读 `status`；修改会话的操作统一由 GUI watcher 串行完成。

配置格式固定为：

```json
{
  "version": 2,
  "target": {
    "vm_name": "19045",
    "vmx": "D:\\All\\vm\\22h2\\19045.vmx",
    "snapshot_name": "s3",
    "snapshot_uid": "3",
    "includes_memory": true
  },
  "auto_start": true
}
```

不支持目标数组、目标级端口或旧格式兼容逻辑。要换目标，直接在 GUI 中重新选择。

GUI 有一个开关和四个操作按钮：

- “自动启动 WinDbg”默认开启并持久化。关闭后只停止后续自动启动，不结束已经存在的会话；从关闭改为开启时，如果目标快照当前匹配且没有会话，会立即触发一次自动启动。
- “选择监控快照”更换唯一监控目标；活动会话存在时不可更换。
- “启动 WinDbg”在目标快照匹配且没有活动会话时手动启动；正在启动或已经连接时按钮禁用，后端仍会做幂等检查，避免自动与手动入口竞态。
- “结束 WinDbg”安全结束当前会话。本次目标快照持续匹配期间不会再次自动启动；手动启动仍可用。
- “重启 WinDbg”只在活动会话存在时启用，按安全顺序结束旧会话后重新启动，且不修改自动启动开关。

## 自动启动原理

检测到所选快照正在运行后，管理器执行一条固定路径：

1. 在当前用户下把 EXDI CLSID 注册到同目录 `ExdiGdbSrv.dll`，并关联专用 AppID。
2. 带同目录 XML 环境变量启动该 AppID 的 `dllhost.exe`。
3. 启动 WinDbgX，连接字符串为：

   ```text
   exdi:CLSID={29f9906e-9dbe-4d4b-b0fb-6acf7fb6d014},Kd=NTBaseAddr,DataBreaks=Default
   ```

4. 通过 WinDbgX 的 `-c` 执行：

   ```text
   .load windbgskill.dll; !windbgskill start 127.0.0.1 26700
   ```

管理器通过子进程环境变量把发布目录加入 `_NT_DEBUGGER_EXTENSION_PATH`，因此 `-c` 中不需要嵌套带路径的引号，发布目录包含空格时也不会破坏 WinDbgX 的命令行解析。

WinDbgX 通过 COM 调用运行在 `dllhost.exe` 中的 EXDI server。`ExdiGdbSrv.dll` 不会加载到 WinDbgX 的 `EngHost.exe` 中；这就是本项目所说的 Out-of-Process。

开启自动启动时，同一次快照匹配只触发一次。虚拟机关机或当前快照 UID 变成其他快照时，管理器会检测到目标离开，安全结束 EXDI 会话并关闭自己启动的 WinDbgX；目标快照再次运行时才会形成下一次自动启动事件。

VMware 恢复内存快照时会重建 `vmware-vmx.exe`。管理器直接从 Windows TCP 表读取 GDB 8864 的监听进程 PID，并把 PID 换代视为一次新的 VM 生命周期，因此恢复同一个快照时即使 `.vmsd` 中的 UID 没变也能识别。原 EXDI socket 所属的旧 VMX 已经退出，旧会话不再具备执行 `bc/q` 的目标；管理器会清理自己启动的失效 WinDbgX 和 COM surrogate。若“自动启动 WinDbg”已开启，目标 UID 仍匹配时随即建立新会话；开关关闭时只清理旧会话，等待用户手动启动。

如果已经活动的 WinDbgX 被外部关闭或异常退出，而目标快照仍然匹配，管理器会检测到 windbgskill 会话消失并完成残留清理，但不会自动重启。GUI 会提示会话已经结束，用户可点击“启动 WinDbg”。这样不会把用户主动关闭窗口误判为需要保活，也不会形成重启循环。

自动启动和手动启动汇合到同一个串行、幂等的启动入口。启动阶段从未成功建立 windbgskill 时不会自动重试，90 秒后明确显示失败；可点击“启动 WinDbg”清理失败现场并重试。

## 安全结束会话

不要强制结束 EXDI WinDbg。VMware GDB Stub 不会在客户端异常退出后可靠清除已经安装的 watchpoint。

管理器关闭、目标快照离开或用户点击“结束 WinDbg”时，会固定执行：

1. 查询 windbgskill 状态；目标运行时先请求中断。
2. 执行 `bc *`，清除 DbgEng 断点。
3. 执行 `q`，让 DbgEng 正常释放 EXDI COM 对象。
4. 确认 windbgskill 进入 `no_target` 或控制端点正常消失。
5. 根据管理器专用的 WinDbgX 启动命令行签名找到对应窗口，发送正常的 `WM_CLOSE` 并确认进程退出，使 DbgEng 释放最后的 COM 引用。
6. 确认 surrogate 已不再保持到 VMware GDB 8864 的 TCP 连接，再结束该 AppID 的空闲 `dllhost.exe`。

`ExdiGdbSrv.dll` 的 `FinalRelease` 会再次删除当前会话持有的数据断点并关闭 GDB socket。本 fork 不再创建跨 apartment 回调自身的通知线程；异步 GDB 完成和 keepalive 由 COM STA 自己的 100ms timer 处理，从结构上消除了通知线程与 `FinalRelease` 相互等待的析构死锁。WinDbgX 异常消失后，预启动的 `dllhost.exe` 可能作为空闲 COM 容器继续存在；进程存在本身不代表 EXDI 会话仍然活动。管理器以该 surrogate 是否仍保持到 VMware GDB 8864 的 `Established` 连接为清理判据：连接已经释放时可以结束空闲容器，连接仍存在时拒绝强制结束。管理器不会按进程名结束其他 WinDbgX，也不会用强制终止代替 DbgEng 的正常退出。

唯一的强制清理场景是已确认 GDB 8864 的 VMX 监听 PID 换代。此时原调试连接的宿主 VMX 已经退出，无法也没有必要再向旧目标发送断点清理命令；管理器只结束带本工具专用命令行签名的 WinDbgX 和专用 AppID surrogate，不影响其他调试器进程。

GUI 的 watcher 和按钮任务都运行在后台线程，但后台线程只向线程安全队列投递消息。Tk 主线程统一更新控件、弹窗和“操作记录”，并使用非阻塞状态快照刷新按钮，因此自动启动、结束或残留清理期间窗口仍可正常重绘和关闭。

## 命令行诊断

GUI 是日常入口；以下命令用于诊断和测试：

```powershell
py -3 .\exdi_manager.py status
py -3 .\exdi_manager.py start
py -3 .\exdi_manager.py stop
py -3 .\exdi_manager.py restart
py -3 .\exdi_manager.py unregister
```

- `status`：显示 GUI 是否运行、自动启动配置、VMware/快照是否匹配、GDB 监听 VMX PID、会话状态、windbgskill 状态、COM 注册、专用 surrogate PID 和管理器 WinDbgX PID。
- `start`：目标快照匹配时幂等地启动 WinDbgX；已经运行时直接报告现状。
- `stop`：按安全顺序结束当前会话。
- `restart`：目标快照匹配时安全结束旧会话并重新启动 WinDbgX。
- `unregister`：仅当 HKCU CLSID 仍指向当前目录时删除本管理器的 CLSID/AppID 注册。执行前应先 `stop`。

GUI 运行时只有 `status` CLI 可用，其他命令会明确拒绝，避免命令行与 2 秒轮询同时修改会话。停止和重启请直接使用 GUI 按钮。

`session_state` 的含义：

- `active`：windbgskill 已连接到活动目标。
- `surrogate_with_gdb_connection_without_active_skill`：windbgskill 不可达，但 surrogate 仍连接 VMware GDB；可能处于启动/析构过程，超时后仍不消失才属于需要诊断的状态。
- `idle_surrogate`：预启动的 COM 容器仍存在，但已经没有到 VMware GDB 的活动连接，可以安全清理。
- `no_target`：windbgskill 服务仍在，但 DbgEng 没有目标。
- `none`：没有 EXDI 会话。

## 清理与卸载

日常关闭 GUI 会自动安全结束当前 EXDI 会话，不需要额外清理。

命令行完整清理顺序为：

```powershell
py -3 .\exdi_manager.py stop
py -3 .\exdi_manager.py unregister
```

`stop` 负责调试会话、断点、GDB socket、windbgskill 和 surrogate；`unregister` 只删除当前用户下指向本发布目录的 EXDI CLSID/AppID。二者都不会删除 `exdi_manager.json`、`exdi_manager.log` 或发布文件。

需要彻底删除时，先执行上述两条命令并关闭 GUI，然后删除整个发布目录即可。不得在活动 EXDI 会话中直接删除 DLL 或强制结束进程。

## 日志

管理器日志位于发布目录：

```text
exdi_manager.log
```

它使用 UTF-8 追加写入。GUI“操作记录”中出现的每一条记录都会以相同内容和时间戳立即写入该文件，包含管理器启动、监控目标、自动启动设置、启动 WinDbgX、安全结束和异常信息。

EXDI DLL 的底层诊断日志仍位于：

```text
%TEMP%\ExdiGdbSrv-vmware.log
```

底层日志用于查看 GDB/内存访问细节，内容可能很多；它与简洁的管理器操作日志相互独立。

## 生成发布包

构建 EXDI 和 windbgskill 后，在 WinDbg-Samples 仓库根目录执行：

```powershell
.\package_exdi_manager.ps1
```

默认部署到：

```text
artifacts\VMwareEXDI
```

该目录已被仓库 `.gitignore` 排除，不会与微软原项目文件或源码提交冲突。脚本可以重复执行：它只覆盖 12 个固定发布文件，保留同目录已经生成的 `exdi_manager.json` 和 `exdi_manager.log`。

重新部署前先关闭管理器 GUI，让它完成安全会话清理；活动中的 `dllhost.exe` 会占用 `ExdiGdbSrv.dll`，不应在调试过程中覆盖发布文件。

需要生成一份全新的可分发目录时，指定一个新路径：

```powershell
.\package_exdi_manager.ps1 -Destination D:\path\to\VMwareEXDI
```

日常开发和本机部署都使用 `artifacts\VMwareEXDI`，不再把发布包复制到 `autowindbg` 项目。
