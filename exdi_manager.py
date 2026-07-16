# -*- coding: utf-8 -*-
import datetime
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import tkinter as tk
import winreg
from pathlib import Path
from tkinter import messagebox, ttk


CLSID = "{29f9906e-9dbe-4d4b-b0fb-6acf7fb6d014}"
REG_PATH = rf"Software\Classes\CLSID\{CLSID}\InprocServer32"
WINDBG_KX = f"exdi:CLSID={CLSID},Kd=NTBaseAddr,DataBreaks=Exdi"

ROOT = Path(__file__).resolve().parent
EXDI_ROOT = ROOT / "Exdi" / "exdigdbsrv"
RELEASE_DIR = EXDI_ROOT / "Release" / "x64"
INSTALL_DIR = EXDI_ROOT / "local-install" / "x64"

RELEASE_DLL = RELEASE_DIR / "ExdiGdbSrv.dll"
RELEASE_PDB = RELEASE_DIR / "ExdiGdbSrv.pdb"
INSTALL_DLL = INSTALL_DIR / "ExdiGdbSrv.dll"
INSTALL_PDB = INSTALL_DIR / "ExdiGdbSrv.pdb"
INSTALL_CONFIG_XML = INSTALL_DIR / "exdiConfigData.xml"
CONFIG_XML = EXDI_ROOT / "GdbSrvControllerLib" / "exdiConfigData.xml"
VMWARE_LOG = Path(tempfile.gettempdir()) / "ExdiGdbSrv-vmware.log"

WINDBG_PROCESS_NAMES = (
    "windbgx.exe",
    "WinDbgX.exe",
    "DbgX.Shell.exe",
    "windbg.exe",
)


def run_command(args, cwd=ROOT, check=False):
    flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
    proc = subprocess.run(
        args,
        cwd=str(cwd),
        capture_output=True,
        text=True,
        encoding="mbcs",
        errors="replace",
        creationflags=flags,
    )
    output = (proc.stdout or "") + (proc.stderr or "")
    if check and proc.returncode:
        command = " ".join(map(str, args))
        raise RuntimeError(f"{command}\n\n{output.strip()}")
    return proc.returncode, output.strip()


def first_existing(paths):
    for path in paths:
        if path and Path(path).exists():
            return str(path)
    return None


def find_windbg():
    env_path = os.environ.get("EXDI_WINDBG")
    if env_path and Path(env_path).exists():
        return env_path

    candidates = [
        shutil.which("windbgx.exe"),
        shutil.which("windbg.exe"),
        Path(os.environ.get("LOCALAPPDATA", "")) / "Microsoft" / "WindowsApps" / "windbgx.exe",
        r"C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\windbg.exe",
        r"C:\Program Files\Windows Kits\10\Debuggers\x64\windbg.exe",
    ]
    return first_existing(candidates) or "windbgx.exe"


def read_registry_target():
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, REG_PATH) as key:
            target, _ = winreg.QueryValueEx(key, None)
            threading_model, _ = winreg.QueryValueEx(key, "ThreadingModel")
            return target, threading_model
    except FileNotFoundError:
        return None, None
    except OSError:
        return None, None


def path_equals(left, right):
    try:
        return Path(left).resolve() == Path(right).resolve()
    except OSError:
        return False


def enable_com():
    if not INSTALL_DLL.exists():
        raise RuntimeError(f"部署 DLL 不存在，不能注册 COM：\n{INSTALL_DLL}")

    with winreg.CreateKeyEx(winreg.HKEY_CURRENT_USER, REG_PATH, 0, winreg.KEY_SET_VALUE) as key:
        winreg.SetValueEx(key, None, 0, winreg.REG_SZ, str(INSTALL_DLL))
        winreg.SetValueEx(key, "ThreadingModel", 0, winreg.REG_SZ, "Apartment")


def delete_registry_tree(root, path):
    try:
        with winreg.OpenKey(root, path, 0, winreg.KEY_READ | winreg.KEY_WRITE) as key:
            while True:
                try:
                    subkey = winreg.EnumKey(key, 0)
                except OSError:
                    break
                delete_registry_tree(root, path + "\\" + subkey)
        winreg.DeleteKey(root, path)
    except FileNotFoundError:
        return


def disable_com():
    delete_registry_tree(winreg.HKEY_CURRENT_USER, REG_PATH)


def launch_windbg():
    if not INSTALL_DLL.exists():
        raise RuntimeError(f"部署 DLL 不存在，不能启动 WinDbg：\n{INSTALL_DLL}")

    registry_target, _ = read_registry_target()
    if not registry_target or not path_equals(registry_target, INSTALL_DLL):
        raise RuntimeError("COM 没有指向当前部署 DLL。请先点击“启用 COM”。")

    windbg = find_windbg()
    subprocess.Popen(
        [windbg, "-v", "-kx", WINDBG_KX],
        cwd=str(ROOT),
        creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
    )


def kill_process_by_image(image_name):
    return run_command(["taskkill", "/F", "/IM", image_name, "/T"], check=False)


def pids_loading_exdi_dll():
    code, output = run_command(["tasklist", "/M", "ExdiGdbSrv.dll", "/FO", "CSV", "/NH"], check=False)
    if code:
        return []

    pids = []
    for line in output.splitlines():
        columns = [part.strip().strip('"') for part in line.split('","')]
        if len(columns) >= 2 and columns[1].isdigit():
            pids.append(columns[1])
    return pids


def stop_windbg():
    messages = []
    for image_name in WINDBG_PROCESS_NAMES:
        _, output = kill_process_by_image(image_name)
        if output:
            messages.append(output)

    for pid in pids_loading_exdi_dll():
        _, output = run_command(["taskkill", "/F", "/PID", pid, "/T"], check=False)
        if output:
            messages.append(output)

    return "\n".join(messages).strip() or "没有发现需要结束的 WinDbg 进程。"


def file_state(path):
    if not path.exists():
        return "缺失"

    stat = path.stat()
    size = f"{stat.st_size:,} bytes"
    mtime = datetime.datetime.fromtimestamp(stat.st_mtime).strftime("%Y-%m-%d %H:%M:%S")
    return f"存在，{size}，{mtime}"


def git_line(args):
    code, output = run_command(["git", "-C", str(ROOT), *args], check=False)
    if code:
        return "不可用"
    return output or "无"


def parse_vmware_config():
    if not CONFIG_XML.exists():
        return {}

    text = CONFIG_XML.read_text(encoding="utf-8-sig", errors="replace")
    config = {}

    current = re.search(r"<ExdiTargets\s+CurrentTarget\s*=\s*\"([^\"]+)\"", text)
    if current:
        config["CurrentTarget"] = current.group(1)

    vmware = re.search(r"<ExdiTarget\s+Name\s*=\s*\"VMWare\"[\s\S]*?</ExdiTarget>", text)
    if not vmware:
        return config

    block = vmware.group(0)
    for key in ("HostNameAndPort", "displayCommPackets", "gdbMonitorCmdDoNotWaitOnOK"):
        attr = re.search(rf"{key}\s*=\s*\"([^\"]+)\"", block)
        if attr:
            config[key] = attr.group(1)

    return config


def collect_status_text():
    registry_target, threading_model = read_registry_target()
    com_ready = bool(registry_target and path_equals(registry_target, INSTALL_DLL))
    release_ready = RELEASE_DLL.exists()
    install_ready = INSTALL_DLL.exists()
    config = parse_vmware_config()

    if not release_ready:
        advice = "Release DLL 缺失：需要先让 Codex 或开发环境构建项目。"
    elif not install_ready:
        advice = "部署 DLL 缺失：需要先让 Codex 把构建产物部署到 local-install。"
    elif not com_ready:
        advice = "DLL 已就绪：点击“启用 COM”后再启动 WinDbg。"
    else:
        advice = "DLL 和 COM 已就绪：可以启动 WinDbg。"

    lines = [
        f"仓库：{ROOT}",
        f"分支：{git_line(['branch', '--show-current'])}",
        f"最近提交：\n{git_line(['log', '--oneline', '--decorate', '-3'])}",
        "",
        "构建状态",
        f"  Release DLL：{file_state(RELEASE_DLL)}",
        f"  Release PDB：{file_state(RELEASE_PDB)}",
        f"  部署 DLL：{file_state(INSTALL_DLL)}",
        f"  部署 PDB：{file_state(INSTALL_PDB)}",
        f"  部署配置：{file_state(INSTALL_CONFIG_XML)}",
        "",
        "COM 状态",
        f"  CLSID：{CLSID}",
        f"  注册目标：{registry_target or '未注册'}",
        f"  ThreadingModel：{threading_model or '未设置'}",
        f"  指向当前部署：{'是' if com_ready else '否'}",
        "",
        "VMware EXDI 配置",
        f"  CurrentTarget：{config.get('CurrentTarget', '未知')}",
        f"  HostNameAndPort：{config.get('HostNameAndPort', '未知')}",
        f"  displayCommPackets：{config.get('displayCommPackets', '未知')}",
        f"  gdbMonitorCmdDoNotWaitOnOK：{config.get('gdbMonitorCmdDoNotWaitOnOK', '未知')}",
        "",
        "运行状态",
        f"  WinDbg：{find_windbg()}",
        f"  VMware 日志：{file_state(VMWARE_LOG)}",
        "",
        f"建议：{advice}",
    ]
    return "\n".join(lines)


class ExdiManager(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("VMware EXDI Manager")
        self.geometry("980x680")
        self.minsize(860, 560)
        self.buttons = []
        self.task_running = False
        self.build_ui()
        self.refresh_status()
        self.after(2000, self.auto_refresh)

    def build_ui(self):
        root = ttk.Frame(self, padding=10)
        root.pack(fill=tk.BOTH, expand=True)

        ttk.Label(root, text="状态").pack(anchor=tk.W)
        self.status_text = tk.Text(root, height=22, wrap=tk.WORD)
        self.status_text.pack(fill=tk.BOTH, expand=True)

        buttons = ttk.Frame(root)
        buttons.pack(fill=tk.X, pady=(10, 8))
        self.add_button(buttons, "启用 COM", lambda: self.run_task("启用 COM", self.enable_com_action))
        self.add_button(buttons, "停用 COM", lambda: self.run_task("停用 COM", self.disable_com_action))
        self.add_button(buttons, "启动 WinDbg", lambda: self.run_task("启动 WinDbg", self.launch_windbg_action))
        self.add_button(buttons, "结束 WinDbg", lambda: self.run_task("结束 WinDbg", self.stop_windbg_action))

        ttk.Label(root, text="操作记录").pack(anchor=tk.W)
        self.log_text = tk.Text(root, height=8, wrap=tk.WORD)
        self.log_text.pack(fill=tk.BOTH, expand=False)

    def add_button(self, parent, text, command):
        button = ttk.Button(parent, text=text, command=command)
        button.pack(side=tk.LEFT, padx=(0, 8))
        self.buttons.append(button)

    def set_buttons_enabled(self, enabled):
        state = tk.NORMAL if enabled else tk.DISABLED
        for button in self.buttons:
            button.configure(state=state)

    def refresh_status(self):
        self.status_text.configure(state=tk.NORMAL)
        self.status_text.delete("1.0", tk.END)
        self.status_text.insert(tk.END, collect_status_text())
        self.status_text.configure(state=tk.DISABLED)

    def auto_refresh(self):
        if not self.task_running:
            self.refresh_status()
        self.after(2000, self.auto_refresh)

    def append_log(self, text):
        timestamp = datetime.datetime.now().strftime("%H:%M:%S")
        self.log_text.insert(tk.END, f"[{timestamp}] {text.rstrip()}\n")
        self.log_text.see(tk.END)

    def run_task(self, title, func):
        if self.task_running:
            return

        self.task_running = True
        self.set_buttons_enabled(False)
        self.append_log(f"{title} 开始")

        def worker():
            try:
                result = func()
                self.after(0, self.task_done, title, result, None)
            except Exception as exc:
                self.after(0, self.task_done, title, "", exc)

        threading.Thread(target=worker, daemon=True).start()

    def task_done(self, title, result, error):
        if error:
            self.append_log(f"{title} 失败：{error}")
            messagebox.showerror(title, str(error))
        else:
            if result:
                self.append_log(str(result))
            self.append_log(f"{title} 完成")

        self.task_running = False
        self.set_buttons_enabled(True)
        self.refresh_status()

    def enable_com_action(self):
        enable_com()
        return f"COM 已指向：{INSTALL_DLL}"

    def disable_com_action(self):
        disable_com()
        return "COM 注册已删除。"

    def launch_windbg_action(self):
        launch_windbg()
        return "WinDbg 已启动。"

    def stop_windbg_action(self):
        return stop_windbg()


def print_status():
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except AttributeError:
        pass
    print(collect_status_text())


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--status":
        print_status()
        return

    app = ExdiManager()
    app.mainloop()


if __name__ == "__main__":
    main()
