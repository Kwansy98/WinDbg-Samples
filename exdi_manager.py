# -*- coding: utf-8 -*-
from __future__ import annotations

import argparse
import ctypes
import datetime as dt
import json
import os
import queue
import re
import socket
import subprocess
import sys
import threading
import time
import tkinter as tk
import urllib.error
import urllib.request
import winreg
from ctypes import wintypes
from dataclasses import asdict, dataclass
from pathlib import Path
from tkinter import messagebox, ttk
from typing import Any, Callable


APP_NAME = "VMware EXDI Manager"
CONFIG_VERSION = 2
GDB_PORT = 8864
WINDBGSKILL_PORT = 26700
POLL_INTERVAL_SECONDS = 2.0
SESSION_START_TIMEOUT_SECONDS = 90.0

CLSID = "{29f9906e-9dbe-4d4b-b0fb-6acf7fb6d014}"
APPID = "{1FC9AD2A-EEC4-467E-AA40-951987327C81}"
GUI_MUTEX_NAME = rf"Global\VMwareEXDIManager-{APPID[1:-1]}"
CLSID_REG_PATH = rf"Software\Classes\CLSID\{CLSID}"
INPROC_REG_PATH = CLSID_REG_PATH + r"\InprocServer32"
APPID_REG_PATH = rf"Software\Classes\AppID\{APPID}"
WINDBG_CONNECTION = f"exdi:CLSID={CLSID},Kd=NTBaseAddr,DataBreaks=Default"

ROOT = Path(__file__).resolve().parent
CONFIG_PATH = ROOT / "exdi_manager.json"
LOG_PATH = ROOT / "exdi_manager.log"
EXDI_DLL = ROOT / "ExdiGdbSrv.dll"
EXDI_PDB = ROOT / "ExdiGdbSrv.pdb"
EXDI_CONFIG_XML = ROOT / "exdiConfigData.xml"
SYSTEM_REGISTERS_XML = ROOT / "systemregisters.xml"
WINDBGSKILL_DLL = ROOT / "windbgskill.dll"
WINDBGSKILL_PDB = ROOT / "windbgskill.pdb"

REQUIRED_ARTIFACTS = (
    EXDI_DLL,
    EXDI_PDB,
    EXDI_CONFIG_XML,
    SYSTEM_REGISTERS_XML,
    WINDBGSKILL_DLL,
    WINDBGSKILL_PDB,
)

VMRUN_REGISTRY_KEYS = (
    r"SOFTWARE\VMware, Inc.\VMware Workstation",
    r"SOFTWARE\WOW6432Node\VMware, Inc.\VMware Workstation",
)
VMRUN_STANDARD_PATHS = (
    Path(r"C:\Program Files\VMware\VMware Workstation\vmrun.exe"),
    Path(r"C:\Program Files (x86)\VMware\VMware Workstation\vmrun.exe"),
)
VMWARE_INVENTORY_PATH = Path(os.environ.get("APPDATA", "")) / "VMware" / "inventory.vmls"

ERROR_FILE_NOT_FOUND = 2
ERROR_ACCESS_DENIED = 5
ERROR_INSUFFICIENT_BUFFER = 122
ERROR_ALREADY_EXISTS = 183
AF_INET = 2
SYNCHRONIZE = 0x00100000
TCP_TABLE_OWNER_PID_LISTENER = 3
WM_CLOSE = 0x0010

KERNEL32 = ctypes.WinDLL("kernel32", use_last_error=True)
KERNEL32.CreateMutexW.argtypes = (ctypes.c_void_p, wintypes.BOOL, wintypes.LPCWSTR)
KERNEL32.CreateMutexW.restype = wintypes.HANDLE
KERNEL32.OpenMutexW.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR)
KERNEL32.OpenMutexW.restype = wintypes.HANDLE
KERNEL32.CloseHandle.argtypes = (wintypes.HANDLE,)
KERNEL32.CloseHandle.restype = wintypes.BOOL
IPHLPAPI = ctypes.WinDLL("iphlpapi", use_last_error=True)
IPHLPAPI.GetExtendedTcpTable.argtypes = (
    ctypes.c_void_p,
    ctypes.POINTER(wintypes.DWORD),
    wintypes.BOOL,
    wintypes.ULONG,
    wintypes.ULONG,
    wintypes.ULONG,
)
IPHLPAPI.GetExtendedTcpTable.restype = wintypes.DWORD
USER32 = ctypes.WinDLL("user32", use_last_error=True)
WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
USER32.EnumWindows.argtypes = (WNDENUMPROC, wintypes.LPARAM)
USER32.EnumWindows.restype = wintypes.BOOL
USER32.GetWindowThreadProcessId.argtypes = (wintypes.HWND, ctypes.POINTER(wintypes.DWORD))
USER32.GetWindowThreadProcessId.restype = wintypes.DWORD
USER32.IsWindowVisible.argtypes = (wintypes.HWND,)
USER32.IsWindowVisible.restype = wintypes.BOOL
USER32.PostMessageW.argtypes = (wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM)
USER32.PostMessageW.restype = wintypes.BOOL


class ManagerError(RuntimeError):
    pass


class MibTcpRowOwnerPid(ctypes.Structure):
    _fields_ = (
        ("state", wintypes.DWORD),
        ("local_address", wintypes.DWORD),
        ("local_port", wintypes.DWORD),
        ("remote_address", wintypes.DWORD),
        ("remote_port", wintypes.DWORD),
        ("owning_pid", wintypes.DWORD),
    )


def close_handle(handle: int) -> None:
    if not KERNEL32.CloseHandle(handle):
        raise ctypes.WinError(ctypes.get_last_error())


def acquire_gui_mutex() -> int | None:
    ctypes.set_last_error(0)
    handle = KERNEL32.CreateMutexW(None, False, GUI_MUTEX_NAME)
    if not handle:
        raise ctypes.WinError(ctypes.get_last_error())
    if ctypes.get_last_error() == ERROR_ALREADY_EXISTS:
        close_handle(handle)
        return None
    return handle


def manager_instance_running() -> bool:
    ctypes.set_last_error(0)
    handle = KERNEL32.OpenMutexW(SYNCHRONIZE, False, GUI_MUTEX_NAME)
    if handle:
        close_handle(handle)
        return True
    error = ctypes.get_last_error()
    if error == ERROR_FILE_NOT_FOUND:
        return False
    if error == ERROR_ACCESS_DENIED:
        return True
    raise ctypes.WinError(error)


@dataclass(frozen=True, slots=True)
class Target:
    vm_name: str
    vmx: str
    snapshot_name: str
    snapshot_uid: str
    includes_memory: bool

    @classmethod
    def from_dict(cls, value: dict[str, Any]) -> Target:
        expected = {"vm_name", "vmx", "snapshot_name", "snapshot_uid", "includes_memory"}
        if not isinstance(value, dict) or set(value) != expected:
            raise ManagerError("target 字段不符合配置格式。")
        if not isinstance(value["includes_memory"], bool):
            raise ManagerError("includes_memory 必须是布尔值。")
        return cls(
            vm_name=require_text(value["vm_name"], "vm_name"),
            vmx=require_text(value["vmx"], "vmx"),
            snapshot_name=require_text(value["snapshot_name"], "snapshot_name"),
            snapshot_uid=require_text(value["snapshot_uid"], "snapshot_uid"),
            includes_memory=value["includes_memory"],
        )


@dataclass(frozen=True, slots=True)
class ManagerConfig:
    target: Target
    auto_start: bool

    @classmethod
    def from_dict(cls, value: dict[str, Any]) -> ManagerConfig:
        if not isinstance(value, dict) or set(value) != {"version", "target", "auto_start"}:
            raise ManagerError("配置文件必须使用精确的版本 2 格式。")
        if value["version"] != CONFIG_VERSION:
            raise ManagerError(f"配置文件版本必须是 {CONFIG_VERSION}。")
        if not isinstance(value["auto_start"], bool):
            raise ManagerError("auto_start 必须是布尔值。")
        return cls(Target.from_dict(value["target"]), value["auto_start"])

    def to_dict(self) -> dict[str, Any]:
        return {
            "version": CONFIG_VERSION,
            "target": asdict(self.target),
            "auto_start": self.auto_start,
        }


@dataclass(frozen=True, slots=True)
class WatcherSnapshot:
    config: ManagerConfig
    vmware_state: str
    target_matches: bool
    session_phase: str
    session_state: str


def require_text(value: Any, name: str) -> str:
    if not isinstance(value, str) or not value.strip():
        raise ManagerError(f"{name} 必须是非空字符串。")
    return value


def normalized_path(value: str | Path) -> str:
    return os.path.normcase(os.path.abspath(os.fspath(value)))


def atomic_write_json(path: Path, payload: dict[str, Any]) -> None:
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    os.replace(temporary, path)


def append_operation_log(message: str) -> str:
    timestamp = dt.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    lines = message.rstrip().splitlines() or [""]
    record = "\n".join(f"[{timestamp}] {line}" for line in lines)
    with LOG_PATH.open("a", encoding="utf-8", newline="\n") as stream:
        stream.write(record + "\n")
    return record


def load_config() -> ManagerConfig | None:
    if not CONFIG_PATH.is_file():
        return None
    try:
        value = json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ManagerError(f"无法读取配置文件 {CONFIG_PATH}：{exc}") from exc
    return ManagerConfig.from_dict(value)


def save_config(config: ManagerConfig) -> None:
    atomic_write_json(CONFIG_PATH, config.to_dict())


def validate_runtime_artifacts() -> None:
    missing = [path.name for path in REQUIRED_ARTIFACTS if not path.is_file()]
    if missing:
        raise ManagerError("发布目录缺少必需文件：\n" + "\n".join(f"  {name}" for name in missing))


def register_com() -> None:
    validate_runtime_artifacts()
    with winreg.CreateKeyEx(winreg.HKEY_CURRENT_USER, CLSID_REG_PATH, 0, winreg.KEY_SET_VALUE) as key:
        winreg.SetValueEx(key, "AppID", 0, winreg.REG_SZ, APPID)
    with winreg.CreateKeyEx(winreg.HKEY_CURRENT_USER, INPROC_REG_PATH, 0, winreg.KEY_SET_VALUE) as key:
        winreg.SetValueEx(key, None, 0, winreg.REG_SZ, str(EXDI_DLL))
        winreg.SetValueEx(key, "ThreadingModel", 0, winreg.REG_SZ, "Apartment")
    with winreg.CreateKeyEx(winreg.HKEY_CURRENT_USER, APPID_REG_PATH, 0, winreg.KEY_SET_VALUE) as key:
        winreg.SetValueEx(key, "DllSurrogate", 0, winreg.REG_SZ, "")


def read_com_registration() -> tuple[str | None, str | None, str | None]:
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, INPROC_REG_PATH) as key:
            target, _ = winreg.QueryValueEx(key, None)
            threading_model, _ = winreg.QueryValueEx(key, "ThreadingModel")
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, CLSID_REG_PATH) as key:
            appid, _ = winreg.QueryValueEx(key, "AppID")
        return target, threading_model, appid
    except OSError:
        return None, None, None


def delete_registry_tree(root: int, path: str) -> None:
    try:
        with winreg.OpenKey(root, path, 0, winreg.KEY_READ | winreg.KEY_WRITE) as key:
            children: list[str] = []
            index = 0
            while True:
                try:
                    children.append(winreg.EnumKey(key, index))
                    index += 1
                except OSError:
                    break
        for child in children:
            delete_registry_tree(root, path + "\\" + child)
        winreg.DeleteKey(root, path)
    except FileNotFoundError:
        return


def unregister_com() -> None:
    target, _, appid = read_com_registration()
    if target and normalized_path(target) != normalized_path(EXDI_DLL):
        raise ManagerError(f"HKCU COM 当前指向其他文件，拒绝删除：{target}")
    if appid and appid.casefold() != APPID.casefold():
        raise ManagerError(f"HKCU COM 当前使用其他 AppID，拒绝删除：{appid}")
    delete_registry_tree(winreg.HKEY_CURRENT_USER, CLSID_REG_PATH)
    delete_registry_tree(winreg.HKEY_CURRENT_USER, APPID_REG_PATH)


def find_windbg() -> Path:
    path = Path(os.environ.get("LOCALAPPDATA", "")) / "Microsoft" / "WindowsApps" / "WinDbgX.exe"
    if not path.is_file():
        raise ManagerError(f"没有找到 WinDbgX.exe：{path}")
    return path


def find_vmrun() -> Path:
    for key_name in VMRUN_REGISTRY_KEYS:
        try:
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, key_name) as key:
                install_path, _ = winreg.QueryValueEx(key, "InstallPath")
        except OSError:
            continue
        candidate = Path(install_path) / "vmrun.exe"
        if candidate.is_file():
            return candidate
    for candidate in VMRUN_STANDARD_PATHS:
        if candidate.is_file():
            return candidate
    raise ManagerError("没有找到 VMware Workstation 的 vmrun.exe。")


def run_process(arguments: list[str | Path], timeout: float = 30.0, check: bool = True) -> tuple[int, str]:
    try:
        result = subprocess.run(
            [os.fspath(argument) for argument in arguments],
            cwd=str(ROOT),
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=timeout,
            creationflags=subprocess.CREATE_NO_WINDOW,
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        raise ManagerError(f"命令执行超时：{arguments[0]}") from exc
    output = ((result.stdout or "") + (result.stderr or "")).strip()
    if check and result.returncode:
        raise ManagerError(f"命令执行失败 ({result.returncode})：{output}")
    return result.returncode, output


def run_vmrun(*arguments: str) -> str:
    return run_process([find_vmrun(), "-T", "ws", *arguments])[1]


def decode_vmware_text(path: Path) -> str:
    raw = path.read_bytes()
    header = raw[:256].decode("ascii", errors="ignore")
    match = re.search(r'^\.encoding\s*=\s*"([^"]+)"', header, flags=re.MULTILINE)
    return raw.decode(match.group(1) if match else "utf-8")


def parse_vmware_mapping(path: Path) -> dict[str, str]:
    mapping: dict[str, str] = {}
    for line in decode_vmware_text(path).splitlines():
        match = re.match(r'^\s*([^=]+?)\s*=\s*"(.*)"\s*$', line)
        if match:
            value = match.group(2).replace('\\"', '"').replace('\\\\', '\\')
            mapping[match.group(1).strip()] = value
    return mapping


def running_vm_paths() -> set[str]:
    lines = run_vmrun("list").splitlines()
    return {normalized_path(line) for line in lines[1:] if line.strip()}


def tcp_listener_pids(port: int) -> set[int]:
    size = wintypes.DWORD()
    table: ctypes.Array[ctypes.c_char] | None = None
    while True:
        result = IPHLPAPI.GetExtendedTcpTable(
            table,
            ctypes.byref(size),
            False,
            AF_INET,
            TCP_TABLE_OWNER_PID_LISTENER,
            0,
        )
        if result == ERROR_INSUFFICIENT_BUFFER:
            table = ctypes.create_string_buffer(size.value)
            continue
        if result:
            raise ctypes.WinError(result)
        break

    if table is None:
        return set()

    row_count = wintypes.DWORD.from_buffer_copy(table).value
    row_size = ctypes.sizeof(MibTcpRowOwnerPid)
    rows_offset = ctypes.sizeof(wintypes.DWORD)
    listeners: set[int] = set()
    for index in range(row_count):
        row = MibTcpRowOwnerPid.from_buffer_copy(table, rows_offset + index * row_size)
        if socket.ntohs(row.local_port & 0xFFFF) == port:
            listeners.add(row.owning_pid)
    return listeners


def gdb_server_pid() -> int | None:
    listeners = tcp_listener_pids(GDB_PORT)
    if len(listeners) > 1:
        raise ManagerError(f"GDB 端口 {GDB_PORT} 同时由多个进程监听：{sorted(listeners)}")
    return next(iter(listeners), None)


def list_registered_vms() -> list[dict[str, Any]]:
    if not VMWARE_INVENTORY_PATH.is_file():
        raise ManagerError(f"VMware inventory 不存在：{VMWARE_INVENTORY_PATH}")
    mapping = parse_vmware_mapping(VMWARE_INVENTORY_PATH)
    running = running_vm_paths()
    virtual_machines: list[dict[str, Any]] = []
    for key, value in mapping.items():
        match = re.match(r"^vmlist(\d+)\.config$", key)
        if not match or not value:
            continue
        prefix = f"vmlist{match.group(1)}"
        vmx = Path(value)
        virtual_machines.append(
            {
                "name": mapping.get(f"{prefix}.DisplayName", vmx.stem),
                "vmx": str(vmx),
                "exists": vmx.is_file(),
                "running": normalized_path(vmx) in running,
            }
        )
    return sorted(virtual_machines, key=lambda item: item["name"].casefold())


def snapshot_inventory(vmx_value: str | Path) -> dict[str, Any]:
    vmx = Path(vmx_value)
    vmsd = vmx.with_suffix(".vmsd")
    if not vmsd.is_file():
        return {"vmx": str(vmx), "current_uid": None, "snapshots": []}
    mapping = parse_vmware_mapping(vmsd)
    current_uid = mapping.get("snapshot.current")
    indexes = sorted(
        int(match.group(1))
        for key in mapping
        if (match := re.match(r"^snapshot(\d+)\.uid$", key))
    )
    snapshots: list[dict[str, Any]] = []
    for index in indexes:
        prefix = f"snapshot{index}"
        uid = mapping[f"{prefix}.uid"]
        snapshots.append(
            {
                "uid": uid,
                "name": mapping.get(f"{prefix}.displayName", "") or f"UID {uid}",
                "includes_memory": mapping.get(f"{prefix}.type") == "1",
                "is_current": uid == current_uid,
            }
        )
    return {"vmx": str(vmx), "current_uid": current_uid, "snapshots": snapshots}


def enumerate_targets() -> list[Target]:
    targets: list[Target] = []
    for vm in list_registered_vms():
        if not vm["exists"]:
            continue
        inventory = snapshot_inventory(vm["vmx"])
        for snapshot in inventory["snapshots"]:
            targets.append(
                Target(
                    vm_name=vm["name"],
                    vmx=vm["vmx"],
                    snapshot_name=snapshot["name"],
                    snapshot_uid=snapshot["uid"],
                    includes_memory=snapshot["includes_memory"],
                )
            )
    return sorted(targets, key=lambda item: (item.vm_name.casefold(), item.snapshot_name.casefold()))


def target_vmware_state(target: Target) -> tuple[bool, bool, str]:
    running = normalized_path(target.vmx) in running_vm_paths()
    if not running:
        return False, False, "虚拟机未运行"
    current_uid = snapshot_inventory(target.vmx)["current_uid"]
    if current_uid == target.snapshot_uid:
        return True, True, "目标快照正在运行"
    return True, False, f"当前快照 UID：{current_uid or '未知'}"


def skill_request(path: str, data: str | None = None, timeout: float = 2.0) -> str:
    body = None if data is None else data.encode("utf-8")
    request = urllib.request.Request(
        f"http://127.0.0.1:{WINDBGSKILL_PORT}{path}",
        data=body,
        method="POST" if body is not None else "GET",
    )
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return response.read().decode("utf-8", errors="replace")


def query_skill_status(timeout: float = 1.0) -> dict[str, Any] | None:
    try:
        value = json.loads(skill_request("/api/status", timeout=timeout))
    except (urllib.error.URLError, TimeoutError, json.JSONDecodeError, OSError):
        return None
    return value if isinstance(value, dict) else None


def surrogate_pids() -> list[int]:
    command = (
        "Get-CimInstance Win32_Process -Filter \"Name='dllhost.exe'\" | "
        f"Where-Object {{ $_.CommandLine -like '*{APPID}*' }} | "
        "ForEach-Object { $_.ProcessId }"
    )
    code, output = run_process(
        ["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", command],
        timeout=15,
        check=False,
    )
    if code:
        raise ManagerError(f"无法查询 EXDI COM surrogate：{output}")
    return [int(line) for line in output.splitlines() if line.strip().isdigit()]


def terminate_surrogates() -> None:
    pids = surrogate_pids()
    if not pids:
        return
    time.sleep(0.5)
    for pid in surrogate_pids():
        run_process(["taskkill.exe", "/F", "/PID", str(pid), "/T"], timeout=10)


def discard_stale_session() -> str:
    windbg_pids = managed_windbg_pids()
    for pid in windbg_pids:
        run_process(["taskkill.exe", "/F", "/PID", str(pid), "/T"], timeout=10)
    stale_surrogates = surrogate_pids()
    terminate_surrogates()

    details: list[str] = []
    if windbg_pids:
        details.append(f"WinDbgX PID={','.join(str(pid) for pid in windbg_pids)}")
    if stale_surrogates:
        details.append(f"COM surrogate PID={','.join(str(pid) for pid in stale_surrogates)}")
    if not details:
        return "失效的 EXDI 会话已经退出。"
    return f"已清理失效的 {'、'.join(details)}。"


def surrogate_gdb_pids(pids: list[int] | None = None) -> list[int]:
    if pids is None:
        pids = surrogate_pids()
    if not pids:
        return []

    process_ids = ",".join(str(pid) for pid in pids)
    command = (
        f"$surrogatePids = @({process_ids}); "
        f"$connections = Get-NetTCPConnection -State Established -RemotePort {GDB_PORT} "
        "-ErrorAction SilentlyContinue; "
        "$connections | "
        "Where-Object { $surrogatePids -contains $_.OwningProcess } | "
        "Select-Object -ExpandProperty OwningProcess -Unique; "
        "exit 0"
    )
    code, output = run_process(
        ["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", command],
        timeout=15,
        check=False,
    )
    if code:
        raise ManagerError(f"无法查询 EXDI surrogate 的 GDB 连接：{output}")
    return [int(line) for line in output.splitlines() if line.strip().isdigit()]


def wait_for_surrogate_gdb_disconnect(timeout: float) -> list[int]:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        connected = surrogate_gdb_pids()
        if not connected:
            return []
        time.sleep(0.1)
    return surrogate_gdb_pids()


def terminate_surrogates_after_gdb_disconnect(vm_is_running: bool) -> None:
    if not surrogate_pids():
        return
    connected = wait_for_surrogate_gdb_disconnect(10)
    if connected and vm_is_running:
        raise ManagerError(
            f"EXDI surrogate PID={','.join(str(pid) for pid in connected)} "
            f"仍连接 VMware GDB {GDB_PORT}；为避免遗留底层断点，不会强制结束。"
        )
    terminate_surrogates()


def managed_windbg_pids() -> list[int]:
    command = (
        "Get-CimInstance Win32_Process -Filter \"Name='DbgX.Shell.exe'\" | "
        "Where-Object { "
        "$_.CommandLine -like '*VMware EXDI -*' -and "
        f"$_.CommandLine -like '*{CLSID}*' -and "
        f"$_.CommandLine -like '*windbgskill start 127.0.0.1 {WINDBGSKILL_PORT}*' "
        "} | ForEach-Object { $_.ProcessId }"
    )
    code, output = run_process(
        ["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", command],
        timeout=15,
        check=False,
    )
    if code:
        raise ManagerError(f"无法查询管理器启动的 WinDbgX：{output}")
    return [int(line) for line in output.splitlines() if line.strip().isdigit()]


def close_managed_windbg_windows() -> list[int]:
    pids = managed_windbg_pids()
    if not pids:
        return []

    targets = set(pids)
    windows: list[int] = []

    @WNDENUMPROC
    def collect_window(hwnd: int, _lparam: int) -> bool:
        process_id = wintypes.DWORD()
        USER32.GetWindowThreadProcessId(hwnd, ctypes.byref(process_id))
        if process_id.value in targets and USER32.IsWindowVisible(hwnd):
            windows.append(hwnd)
        return True

    if not USER32.EnumWindows(collect_window, 0):
        raise ctypes.WinError(ctypes.get_last_error())
    if not windows:
        raise ManagerError(f"找到了 WinDbgX 进程 {pids}，但没有找到可关闭的主窗口。")
    for hwnd in windows:
        if not USER32.PostMessageW(hwnd, WM_CLOSE, 0, 0):
            raise ctypes.WinError(ctypes.get_last_error())

    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        remaining = targets.intersection(managed_windbg_pids())
        if not remaining:
            return pids
        time.sleep(0.1)
    raise ManagerError(f"WinDbgX 没有在正常关闭请求后退出：{sorted(remaining)}")


def finish_stopped_session(detail: str, vm_is_running: bool) -> str:
    closed = close_managed_windbg_windows()
    terminate_surrogates_after_gdb_disconnect(vm_is_running)
    if closed:
        detail += f" 已关闭 WinDbgX PID={','.join(str(pid) for pid in closed)}。"
    return detail


def shutdown_dormant_skill() -> None:
    status = query_skill_status()
    if status is None:
        return
    if status.get("state") != "no_target":
        raise ManagerError(f"windbgskill {WINDBGSKILL_PORT} 已被活动调试会话占用。")
    try:
        skill_request("/api/shutdown", "", timeout=3)
    except (urllib.error.URLError, TimeoutError, OSError):
        pass


def launch_windbg(target: Target) -> int:
    validate_runtime_artifacts()
    register_com()
    shutdown_dormant_skill()
    existing = surrogate_pids()
    if existing:
        raise ManagerError(f"发现未释放的 EXDI COM surrogate：{existing}")
    close_managed_windbg_windows()

    environment = os.environ.copy()
    environment["EXDI_GDBSRV_XML_CONFIG_FILE"] = str(EXDI_CONFIG_XML)
    environment["EXDI_SYSTEM_REGISTERS_MAP_XML_FILE"] = str(SYSTEM_REGISTERS_XML)
    extension_path = environment.get("_NT_DEBUGGER_EXTENSION_PATH")
    environment["_NT_DEBUGGER_EXTENSION_PATH"] = (
        str(ROOT) if not extension_path else str(ROOT) + os.pathsep + extension_path
    )

    dllhost = Path(os.environ["SystemRoot"]) / "System32" / "dllhost.exe"
    surrogate = subprocess.Popen(
        [str(dllhost), f"/Processid:{APPID}"],
        cwd=str(ROOT),
        env=environment,
        creationflags=subprocess.CREATE_NO_WINDOW,
    )
    time.sleep(0.2)
    if surrogate.poll() is not None:
        raise ManagerError("EXDI COM surrogate 启动后立即退出。")

    startup_command = f".load windbgskill.dll; !windbgskill start 127.0.0.1 {WINDBGSKILL_PORT}"
    title = f"VMware EXDI - {target.vm_name} / {target.snapshot_name}"
    try:
        subprocess.Popen(
            [str(find_windbg()), "-v", "-T", title, "-kx", WINDBG_CONNECTION, "-c", startup_command],
            cwd=str(ROOT),
            env=environment,
            creationflags=subprocess.CREATE_NO_WINDOW,
        )
    except Exception:
        surrogate.terminate()
        raise
    return surrogate.pid


def stop_exdi_session(vm_is_running: bool) -> str:
    status = query_skill_status(timeout=2)
    if status is None:
        return finish_stopped_session(
            "没有活动的 EXDI 调试会话；已清理空闲 COM surrogate。",
            vm_is_running,
        )

    for _ in range(50):
        state = status.get("state")
        if state == "no_target":
            shutdown_dormant_skill()
            return finish_stopped_session("EXDI 调试会话已经结束。", vm_is_running)
        if state == "running":
            try:
                skill_request("/api/break", "", timeout=3)
            except urllib.error.HTTPError as exc:
                if exc.code != 409:
                    raise
            time.sleep(0.1)
            status = query_skill_status(timeout=2)
            if status is None:
                raise ManagerError("中断目标后失去了 windbgskill 连接。")
            continue
        if state in {"broken", "stepping"}:
            skill_request("/api/exec?timeout=30", "bc *", timeout=35)
            try:
                skill_request("/api/exec?timeout=30", "q", timeout=35)
            except (urllib.error.URLError, TimeoutError, OSError):
                pass
            for _ in range(50):
                final_status = query_skill_status(timeout=1)
                if final_status is None:
                    return finish_stopped_session(
                        "已清除断点并正常结束 EXDI 调试会话。",
                        vm_is_running,
                    )
                if final_status.get("state") == "no_target":
                    shutdown_dormant_skill()
                    return finish_stopped_session(
                        "已清除断点并正常结束 EXDI 调试会话。",
                        vm_is_running,
                    )
                time.sleep(0.1)
            raise ManagerError("执行 q 后 WinDbg 没有进入 no_target 状态。")
        raise ManagerError(f"WinDbg 当前状态不允许安全关闭：{state}")
    raise ManagerError("WinDbg 未能进入可安全关闭的状态。")


class Watcher:
    def __init__(self, config: ManagerConfig, notify: Callable[[str], None] | None = None) -> None:
        self._lock = threading.RLock()
        self._stop_event = threading.Event()
        self._scan_event = threading.Event()
        self._thread: threading.Thread | None = None
        self._notify = notify or (lambda _message: None)
        self.config = config
        self.vmware_state = "等待首次扫描"
        self.target_matches = False
        self.session_phase = "checking"
        self.session_state = "等待首次扫描"
        self._automatic_start_consumed = False
        self._launch_started_at: float | None = None
        self._session_observed_active = False
        self._idle_message: str | None = None
        self._vmx_pid: int | None = None
        self._vmx_listener_missing = False

    def start(self) -> None:
        if self._thread and self._thread.is_alive():
            return
        self._thread = threading.Thread(target=self._run, name="vmware-exdi-watcher", daemon=True)
        self._thread.start()

    def request_scan(self) -> None:
        self._scan_event.set()

    def update_config(self, config: ManagerConfig) -> None:
        with self._lock:
            self.config = config
            self.vmware_state = "等待扫描"
            self.target_matches = False
            self.session_phase = "checking"
            self.session_state = "等待扫描"
            self._automatic_start_consumed = False
            self._launch_started_at = None
            self._session_observed_active = False
            self._idle_message = None
            self._vmx_pid = None
            self._vmx_listener_missing = False
        self.request_scan()

    def set_auto_start(self, enabled: bool) -> ManagerConfig:
        with self._lock:
            previous = self.config.auto_start
            self.config = ManagerConfig(self.config.target, enabled)
            if enabled and not previous and self.session_phase in {"idle", "error"}:
                self._automatic_start_consumed = False
                self._idle_message = None
        self.request_scan()
        return self.config

    def snapshot(self) -> WatcherSnapshot:
        with self._lock:
            return WatcherSnapshot(
                config=self.config,
                vmware_state=self.vmware_state,
                target_matches=self.target_matches,
                session_phase=self.session_phase,
                session_state=self.session_state,
            )

    def try_snapshot(self) -> WatcherSnapshot | None:
        if not self._lock.acquire(blocking=False):
            return None
        try:
            return WatcherSnapshot(
                config=self.config,
                vmware_state=self.vmware_state,
                target_matches=self.target_matches,
                session_phase=self.session_phase,
                session_state=self.session_state,
            )
        finally:
            self._lock.release()

    def _run(self) -> None:
        while not self._stop_event.is_set():
            try:
                self.poll_once()
            except Exception as exc:
                with self._lock:
                    self.session_phase = "error"
                    self.session_state = f"错误：{exc}"
                self._notify(f"轮询失败：{exc}")
            self._scan_event.wait(POLL_INTERVAL_SECONDS)
            self._scan_event.clear()

    def poll_once(self) -> None:
        with self._lock:
            config = self.config
            target = config.target
            previous_matches = self.target_matches
            previous_vmx_pid = self._vmx_pid
        vmx_pid = gdb_server_pid()
        vm_running, matches, vmware_state = target_vmware_state(target)
        vmx_replaced = (
            previous_vmx_pid is not None
            and vmx_pid is not None
            and vmx_pid != previous_vmx_pid
        )
        vmx_temporarily_missing = (
            matches and previous_matches and previous_vmx_pid is not None and vmx_pid is None
        )
        if vmx_temporarily_missing:
            with self._lock:
                self.vmware_state = "目标快照正在运行；等待 VMware GDB 监听恢复"
                self.target_matches = True
                self.session_phase = "checking"
                self.session_state = "VMX 正在恢复快照或重新启动"
                if not self._vmx_listener_missing:
                    self._vmx_listener_missing = True
                    self._notify("VMware GDB 监听暂时消失，正在等待 VMX 恢复。")
            return
        status = None if vmx_replaced else query_skill_status()
        active = status is not None and status.get("state") != "no_target"

        with self._lock:
            self.vmware_state = vmware_state
            self.target_matches = matches
            if vmx_pid is not None:
                self._vmx_pid = vmx_pid
                self._vmx_listener_missing = False

            if vmx_replaced:
                self._notify(
                    f"检测到 VMware VMX 已重建（PID {previous_vmx_pid} -> {vmx_pid}），"
                    "判定虚拟机已恢复快照或重新启动。"
                )
                self._notify(discard_stale_session())
                self._automatic_start_consumed = False
                self._launch_started_at = None
                self._session_observed_active = False
                self._idle_message = None

            if not matches:
                if active or surrogate_pids() or managed_windbg_pids():
                    detail = stop_exdi_session(vm_running)
                    self._notify(detail)
                self.session_phase = "idle"
                self.session_state = "无活动会话；等待目标快照"
                self._automatic_start_consumed = False
                self._launch_started_at = None
                self._session_observed_active = False
                self._idle_message = None
                return

            if active:
                self.session_phase = "active"
                self.session_state = f"已连接，windbgskill={WINDBGSKILL_PORT}，状态={status.get('state')}"
                self._automatic_start_consumed = True
                self._launch_started_at = None
                self._session_observed_active = True
                self._idle_message = None
                return

            if self._session_observed_active:
                self._notify("检测到 WinDbgX 会话已结束，正在清理残留状态。")
                detail = stop_exdi_session(vm_running)
                self._notify(detail)
                self._session_observed_active = False
                self._launch_started_at = None
                self._automatic_start_consumed = True
                self._idle_message = "WinDbgX 会话已结束；可点击“启动 WinDbg”重新打开"

            if self._launch_started_at is not None:
                elapsed = time.time() - self._launch_started_at
                if elapsed <= SESSION_START_TIMEOUT_SECONDS:
                    self.session_phase = "starting"
                    self.session_state = f"WinDbgX 正在启动（{int(elapsed)} 秒）"
                    return
                self.session_phase = "error"
                self.session_state = "启动失败：90 秒内没有检测到 windbgskill；可点击“启动 WinDbg”重试"
                return

            if config.auto_start and not self._automatic_start_consumed:
                self._automatic_start_consumed = True
                try:
                    surrogate_pid = launch_windbg(target)
                except Exception:
                    self.session_phase = "error"
                    self.session_state = "自动启动 WinDbgX 失败"
                    raise
                self._launch_started_at = time.time()
                self.session_phase = "starting"
                self.session_state = f"已启动 WinDbgX，COM surrogate PID={surrogate_pid}"
                self._notify(
                    f"检测到 {target.vm_name} / {target.snapshot_name}；"
                    f"已自动启动 WinDbgX，GDB={GDB_PORT}，windbgskill={WINDBGSKILL_PORT}。"
                )
                return

            self.session_phase = "idle"
            self.session_state = self._idle_message or "无活动会话"

    def start_session(self) -> str:
        with self._lock:
            target = self.config.target
            vm_running, matches, vmware_state = target_vmware_state(target)
            self.vmware_state = vmware_state
            self.target_matches = matches
            if not matches:
                raise ManagerError(f"目标快照当前不可用：{vmware_state}")

            status = query_skill_status(timeout=2)
            if status is not None and status.get("state") != "no_target":
                self.session_phase = "active"
                self.session_state = f"已连接，windbgskill={WINDBGSKILL_PORT}，状态={status.get('state')}"
                self._automatic_start_consumed = True
                self._launch_started_at = None
                self._session_observed_active = True
                return "WinDbgX 已经在运行。"

            if (
                self.session_phase == "starting"
                and self._launch_started_at is not None
                and time.time() - self._launch_started_at <= SESSION_START_TIMEOUT_SECONDS
            ):
                return "WinDbgX 正在启动，无需重复操作。"

            if status is not None or surrogate_pids() or managed_windbg_pids():
                stop_exdi_session(vm_running)

            surrogate_pid = launch_windbg(target)
            self._automatic_start_consumed = True
            self._launch_started_at = time.time()
            self._session_observed_active = False
            self._idle_message = None
            self.session_phase = "starting"
            self.session_state = f"已启动 WinDbgX，COM surrogate PID={surrogate_pid}"
            detail = (
                f"已手动启动 {target.vm_name} / {target.snapshot_name}；"
                f"GDB={GDB_PORT}，windbgskill={WINDBGSKILL_PORT}。"
            )
            return detail

    def stop_session(self) -> str:
        with self._lock:
            target = self.config.target
            self._automatic_start_consumed = True
            self._session_observed_active = False
            self.session_phase = "stopping"
            vm_running = normalized_path(target.vmx) in running_vm_paths()
            try:
                detail = stop_exdi_session(vm_running)
            except Exception as exc:
                self.session_phase = "error"
                self.session_state = f"结束失败：{exc}"
                raise
            self.session_phase = "idle"
            self.session_state = detail
            self._launch_started_at = None
            self._idle_message = detail
        return detail

    def restart_session(self) -> str:
        with self._lock:
            target = self.config.target
            vm_running, matches, vmware_state = target_vmware_state(target)
            self.vmware_state = vmware_state
            self.target_matches = matches
            if not matches:
                raise ManagerError(f"目标快照当前不可用：{vmware_state}")

            status = query_skill_status(timeout=2)
            if status is None and not surrogate_pids() and not managed_windbg_pids():
                raise ManagerError("当前没有可重启的 WinDbgX 会话；请点击“启动 WinDbg”。")

            self.session_phase = "stopping"
            try:
                stop_exdi_session(vm_running)
            except Exception as exc:
                self.session_phase = "error"
                self.session_state = f"重启前清理失败：{exc}"
                raise
            surrogate_pid = launch_windbg(target)
            self._automatic_start_consumed = True
            self._launch_started_at = time.time()
            self._session_observed_active = False
            self._idle_message = None
            self.session_phase = "starting"
            self.session_state = f"已重新启动 WinDbgX，COM surrogate PID={surrogate_pid}"
            detail = (
                f"已手动重启 {target.vm_name} / {target.snapshot_name}；"
                f"GDB={GDB_PORT}，windbgskill={WINDBGSKILL_PORT}。"
            )
            return detail

    def shutdown(self) -> str:
        self._stop_event.set()
        self._scan_event.set()
        if self._thread and self._thread is not threading.current_thread():
            self._thread.join(timeout=POLL_INTERVAL_SECONDS + 3)
        return self.stop_session()


class TargetDialog(tk.Toplevel):
    def __init__(self, parent: tk.Misc, targets: list[Target], existing: Target | None) -> None:
        super().__init__(parent)
        self.title("选择监控快照")
        self.geometry("960x560")
        self.minsize(760, 420)
        self.transient(parent)
        self.grab_set()
        self.result: Target | None = None
        self.targets = targets

        root = ttk.Frame(self, padding=12)
        root.pack(fill=tk.BOTH, expand=True)
        ttk.Label(
            root,
            text="选择一个要监控的精确快照。GDB 固定使用 8864，windbgskill 固定使用 26700。",
        ).pack(fill=tk.X, pady=(0, 8))

        columns = ("vm", "snapshot", "uid", "memory", "vmx")
        self.tree = ttk.Treeview(root, columns=columns, show="headings", selectmode="browse")
        headings = {"vm": "虚拟机", "snapshot": "快照", "uid": "UID", "memory": "内存快照", "vmx": "VMX"}
        widths = {"vm": 140, "snapshot": 180, "uid": 60, "memory": 80, "vmx": 430}
        for column in columns:
            self.tree.heading(column, text=headings[column])
            self.tree.column(column, width=widths[column], anchor=tk.W)
        self.tree.pack(fill=tk.BOTH, expand=True)

        existing_identity = (
            (normalized_path(existing.vmx), existing.snapshot_uid) if existing is not None else None
        )
        selected_iid: str | None = None
        for index, target in enumerate(targets):
            iid = str(index)
            self.tree.insert(
                "",
                tk.END,
                iid=iid,
                values=(
                    target.vm_name,
                    target.snapshot_name,
                    target.snapshot_uid,
                    "是" if target.includes_memory else "否",
                    target.vmx,
                ),
            )
            if existing_identity == (normalized_path(target.vmx), target.snapshot_uid):
                selected_iid = iid
        if selected_iid is not None:
            self.tree.selection_set(selected_iid)
            self.tree.see(selected_iid)
        self.tree.bind("<Double-1>", lambda _event: self._save())

        buttons = ttk.Frame(root)
        buttons.pack(fill=tk.X, pady=(10, 0))
        ttk.Button(buttons, text="取消", command=self.destroy).pack(side=tk.RIGHT)
        ttk.Button(buttons, text="保存", command=self._save).pack(side=tk.RIGHT, padx=(0, 8))

    def _save(self) -> None:
        selection = self.tree.selection()
        if not selection:
            messagebox.showerror("选择监控快照", "请选择一个快照。", parent=self)
            return
        self.result = self.targets[int(selection[0])]
        self.destroy()


class ManagerWindow(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title(APP_NAME)
        self.geometry("920x600")
        self.minsize(760, 500)
        self.protocol("WM_DELETE_WINDOW", self._request_close)
        self.task_running = False
        self.watcher: Watcher | None = None
        self._last_snapshot: WatcherSnapshot | None = None
        self._ui_events: queue.SimpleQueue[tuple[Callable[..., None], tuple[Any, ...]]] = queue.SimpleQueue()
        self._closed = False
        self._build_ui()
        self.after(0, self._initialize)
        self.after(250, self._refresh_ui)

    def _build_ui(self) -> None:
        root = ttk.Frame(self, padding=12)
        root.pack(fill=tk.BOTH, expand=True)
        self.target_var = tk.StringVar(value="正在初始化……")
        self.vmware_var = tk.StringVar()
        self.session_var = tk.StringVar()
        self.auto_start_var = tk.BooleanVar(value=True)
        ttk.Label(root, textvariable=self.target_var, font=("Segoe UI", 11, "bold"), wraplength=880).pack(fill=tk.X)
        ttk.Label(root, textvariable=self.vmware_var, wraplength=880).pack(fill=tk.X, pady=(10, 0))
        ttk.Label(root, textvariable=self.session_var, wraplength=880).pack(fill=tk.X, pady=(4, 14))

        options = ttk.Frame(root)
        options.pack(fill=tk.X, pady=(0, 10))
        self.auto_start_check = ttk.Checkbutton(
            options,
            text="自动启动 WinDbg",
            variable=self.auto_start_var,
            command=self._auto_start_changed,
        )
        self.auto_start_check.pack(side=tk.LEFT)

        buttons = ttk.Frame(root)
        buttons.pack(fill=tk.X, pady=(0, 12))
        self.select_button = ttk.Button(buttons, text="选择监控快照", command=self._configure)
        self.select_button.pack(side=tk.LEFT)
        self.start_button = ttk.Button(buttons, text="启动 WinDbg", command=self._start_session)
        self.start_button.pack(side=tk.LEFT, padx=(8, 0))
        self.stop_button = ttk.Button(buttons, text="结束 WinDbg", command=self._stop_session)
        self.stop_button.pack(side=tk.LEFT, padx=(8, 0))
        self.restart_button = ttk.Button(buttons, text="重启 WinDbg", command=self._restart_session)
        self.restart_button.pack(side=tk.LEFT, padx=(8, 0))

        ttk.Separator(root).pack(fill=tk.X, pady=(0, 10))
        ttk.Label(root, text="操作记录").pack(anchor=tk.W)
        self.log = tk.Text(root, wrap=tk.WORD, state=tk.DISABLED)
        self.log.pack(fill=tk.BOTH, expand=True)

    def _initialize(self) -> None:
        try:
            self._append_log(f"{APP_NAME} 启动；运行目录：{ROOT}")
            validate_runtime_artifacts()
            find_windbg()
            find_vmrun()
            register_com()
            config = load_config()
            if config is None:
                self._append_log("首次运行：请选择一个要监控的虚拟机快照。")
                self._configure(first_run=True)
                return
            self._start_watcher(config)
        except Exception as exc:
            self.target_var.set("初始化失败")
            self._append_log(f"初始化失败：{exc}")
            messagebox.showerror(APP_NAME, str(exc), parent=self)

    def _start_watcher(self, config: ManagerConfig) -> None:
        self.auto_start_var.set(config.auto_start)
        if self.watcher is None:
            self.watcher = Watcher(config, notify=lambda message: self._post_ui(self._append_log, message))
            self._last_snapshot = self.watcher.snapshot()
            self.watcher.start()
        else:
            self.watcher.update_config(config)
            self._last_snapshot = self.watcher.snapshot()
        target = config.target
        self._append_log(
            f"开始监控：{target.vm_name} / {target.snapshot_name} "
            f"(UID {target.snapshot_uid})；自动启动={'开启' if config.auto_start else '关闭'}；"
            f"GDB={GDB_PORT}；windbgskill={WINDBGSKILL_PORT}"
        )

    def _configure(self, first_run: bool = False) -> None:
        if self.task_running:
            return
        if self.watcher:
            status = query_skill_status()
            if status is not None and status.get("state") != "no_target":
                messagebox.showinfo(APP_NAME, "请先结束当前 EXDI 会话，再重新选择快照。", parent=self)
                return
        try:
            existing = load_config()
            dialog = TargetDialog(self, enumerate_targets(), existing.target if existing else None)
            self.wait_window(dialog)
        except Exception as exc:
            messagebox.showerror("选择监控快照", str(exc), parent=self)
            return
        if dialog.result is not None:
            config = ManagerConfig(dialog.result, self.auto_start_var.get())
            save_config(config)
            self._start_watcher(config)
            self._append_log(f"配置已保存到 {CONFIG_PATH}")
        elif first_run:
            self.target_var.set("尚未选择监控快照")

    def _auto_start_changed(self) -> None:
        if self.watcher is None or self.task_running:
            return
        enabled = self.auto_start_var.get()
        try:
            current = self._last_snapshot.config if self._last_snapshot else self.watcher.snapshot().config
            save_config(ManagerConfig(current.target, enabled))
            self.watcher.set_auto_start(enabled)
        except Exception as exc:
            self.auto_start_var.set(not enabled)
            messagebox.showerror("自动启动 WinDbg", str(exc), parent=self)
            return
        self._append_log(f"自动启动 WinDbg 已{'开启' if enabled else '关闭'}。")

    def _start_session(self) -> None:
        if self.watcher and not self.task_running:
            self._run_task("启动 WinDbg", self.watcher.start_session)

    def _stop_session(self) -> None:
        if self.watcher and not self.task_running:
            self._run_task("结束 WinDbg", self.watcher.stop_session)

    def _restart_session(self) -> None:
        if self.watcher and not self.task_running:
            self._run_task("重启 WinDbg", self.watcher.restart_session)

    def _run_task(self, title: str, operation: Callable[[], Any], on_success: Callable[[], None] | None = None) -> None:
        if self.task_running:
            return
        self.task_running = True
        self._set_buttons_enabled(False)
        self._append_log(f"{title}开始。")

        def worker() -> None:
            try:
                result = operation()
                self._post_ui(done, result, None)
            except Exception as exc:
                self._post_ui(done, None, exc)

        def done(result: Any, error: Exception | None) -> None:
            self.task_running = False
            self._set_buttons_enabled(True)
            if error:
                self._append_log(f"{title}失败：{error}")
                messagebox.showerror(title, str(error), parent=self)
            else:
                if result:
                    self._append_log(str(result))
                if on_success:
                    on_success()

        threading.Thread(target=worker, daemon=True).start()

    def _set_buttons_enabled(self, enabled: bool) -> None:
        if not enabled:
            for control in (
                self.auto_start_check,
                self.select_button,
                self.start_button,
                self.stop_button,
                self.restart_button,
            ):
                control.configure(state=tk.DISABLED)
            return

        self.auto_start_check.configure(state=tk.NORMAL)
        if self.watcher is None:
            self.select_button.configure(state=tk.NORMAL)
            for button in (self.start_button, self.stop_button, self.restart_button):
                button.configure(state=tk.DISABLED)
            return

        snapshot = self._last_snapshot
        if snapshot is None:
            for button in (self.select_button, self.start_button, self.stop_button, self.restart_button):
                button.configure(state=tk.DISABLED)
            return
        can_select = snapshot.session_phase not in {"starting", "active", "stopping"}
        can_start = snapshot.target_matches and snapshot.session_phase in {"idle", "error"}
        can_stop = snapshot.session_phase in {"starting", "active", "error"}
        can_restart = snapshot.target_matches and snapshot.session_phase == "active"
        self.select_button.configure(state=tk.NORMAL if can_select else tk.DISABLED)
        self.start_button.configure(state=tk.NORMAL if can_start else tk.DISABLED)
        self.stop_button.configure(state=tk.NORMAL if can_stop else tk.DISABLED)
        self.restart_button.configure(state=tk.NORMAL if can_restart else tk.DISABLED)

    def _append_log(self, message: str) -> None:
        record = append_operation_log(message)
        self.log.configure(state=tk.NORMAL)
        self.log.insert(tk.END, record + "\n")
        self.log.see(tk.END)
        self.log.configure(state=tk.DISABLED)

    def _post_ui(self, callback: Callable[..., None], *args: Any) -> None:
        self._ui_events.put((callback, args))

    def _drain_ui_events(self) -> None:
        while True:
            try:
                callback, args = self._ui_events.get_nowait()
            except queue.Empty:
                return
            callback(*args)

    def _refresh_ui(self) -> None:
        self._drain_ui_events()
        if self._closed:
            return
        if self.watcher:
            snapshot = self.watcher.try_snapshot()
            if snapshot is not None:
                self._last_snapshot = snapshot
            else:
                snapshot = self._last_snapshot
            if snapshot is None:
                self.after(250, self._refresh_ui)
                return
            target = snapshot.config.target
            self.target_var.set(
                f"{target.vm_name} / {target.snapshot_name} (UID {target.snapshot_uid})\n{target.vmx}"
            )
            self.vmware_var.set(f"VMware：{snapshot.vmware_state}；GDB 固定端口 {GDB_PORT}")
            self.session_var.set(
                f"WinDbg：{snapshot.session_state}；windbgskill 固定端口 {WINDBGSKILL_PORT}"
            )
        self._set_buttons_enabled(not self.task_running)
        self.after(250, self._refresh_ui)

    def _finish_close(self) -> None:
        self._closed = True
        self.destroy()

    def _request_close(self) -> None:
        if self.task_running:
            return
        if self.watcher is None:
            self.destroy()
            return
        self._run_task("关闭管理器", self.watcher.shutdown, on_success=self._finish_close)


def status_payload() -> dict[str, Any]:
    validate_runtime_artifacts()
    config = load_config()
    registration, threading_model, appid = read_com_registration()
    skill_status = query_skill_status()
    pids = surrogate_pids()
    gdb_pids = surrogate_gdb_pids(pids)
    if skill_status is not None and skill_status.get("state") != "no_target":
        session_state = "active"
    elif gdb_pids:
        session_state = "surrogate_with_gdb_connection_without_active_skill"
    elif pids:
        session_state = "idle_surrogate"
    elif skill_status is not None:
        session_state = "no_target"
    else:
        session_state = "none"

    vmware_state = None
    if config is not None:
        vm_running, snapshot_matches, detail = target_vmware_state(config.target)
        vmware_state = {
            "running": vm_running,
            "snapshot_matches": snapshot_matches,
            "detail": detail,
            "vmx_pid": gdb_server_pid(),
        }

    return {
        "root": str(ROOT),
        "config": str(CONFIG_PATH),
        "log": str(LOG_PATH),
        "manager_running": manager_instance_running(),
        "target": asdict(config.target) if config else None,
        "auto_start": config.auto_start if config else None,
        "vmware": vmware_state,
        "gdb_port": GDB_PORT,
        "windbgskill_port": WINDBGSKILL_PORT,
        "session_state": session_state,
        "windbgskill_status": skill_status,
        "com": {"target": registration, "threading_model": threading_model, "appid": appid},
        "surrogate_pids": pids,
        "surrogate_gdb_pids": gdb_pids,
        "windbg_pids": managed_windbg_pids(),
        "windbg": str(find_windbg()),
        "vmrun": str(find_vmrun()),
    }


def start_once() -> dict[str, Any]:
    config = load_config()
    if config is None:
        raise ManagerError(f"尚未生成配置文件：{CONFIG_PATH}")
    watcher = Watcher(config)
    return {"detail": watcher.start_session()}


def stop_once() -> dict[str, Any]:
    config = load_config()
    vm_running = bool(config and normalized_path(config.target.vmx) in running_vm_paths())
    return {"detail": stop_exdi_session(vm_running)}


def restart_once() -> dict[str, Any]:
    config = load_config()
    if config is None:
        raise ManagerError(f"尚未生成配置文件：{CONFIG_PATH}")
    watcher = Watcher(config)
    return {"detail": watcher.restart_session()}


def emit_json(payload: dict[str, Any], *, error: bool = False) -> None:
    stream = sys.stderr if error else sys.stdout
    if hasattr(stream, "reconfigure"):
        stream.reconfigure(encoding="utf-8", errors="replace")
    print(json.dumps(payload, ensure_ascii=False, indent=2), file=stream)


def run_cli(action: str) -> int:
    try:
        if action != "status" and manager_instance_running():
            raise ManagerError("管理器 GUI 正在运行；请在 GUI 中执行该操作，命令行仅保留 status 诊断。")
        if action == "status":
            result = status_payload()
        elif action == "start":
            result = start_once()
        elif action == "stop":
            result = stop_once()
        elif action == "restart":
            result = restart_once()
        else:
            unregister_com()
            result = {"unregistered": True}
        emit_json({"ok": True, "action": action, **result})
        return 0
    except Exception as exc:
        emit_json({"ok": False, "action": action, "error": str(exc)}, error=True)
        return 1


def main() -> int:
    parser = argparse.ArgumentParser(description="单快照 VMware WinDbgX EXDI 管理器")
    parser.add_argument(
        "action",
        nargs="?",
        choices=("status", "start", "stop", "restart", "unregister"),
        help="不指定 action 时启动图形界面",
    )
    args = parser.parse_args()
    if args.action:
        return run_cli(args.action)
    mutex = acquire_gui_mutex()
    if mutex is None:
        message = "VMware EXDI Manager 已经在本机运行，不能重复启动。"
        append_operation_log(message)
        dialog = tk.Tk()
        dialog.withdraw()
        messagebox.showinfo(APP_NAME, message, parent=dialog)
        dialog.destroy()
        return 1
    try:
        app = ManagerWindow()
        app.mainloop()
        return 0
    finally:
        close_handle(mutex)


if __name__ == "__main__":
    raise SystemExit(main())
