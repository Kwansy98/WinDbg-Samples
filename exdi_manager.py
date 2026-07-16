# -*- coding: utf-8 -*-
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
SOLUTION = EXDI_ROOT / "ExdiGdbSrv.sln"
RELEASE_DIR = EXDI_ROOT / "Release" / "x64"
INSTALL_DIR = EXDI_ROOT / "local-install" / "x64"
RELEASE_DLL = RELEASE_DIR / "ExdiGdbSrv.dll"
RELEASE_PDB = RELEASE_DIR / "ExdiGdbSrv.pdb"
INSTALL_DLL = INSTALL_DIR / "ExdiGdbSrv.dll"
INSTALL_PDB = INSTALL_DIR / "ExdiGdbSrv.pdb"
CONFIG_XML = EXDI_ROOT / "GdbSrvControllerLib" / "exdiConfigData.xml"
SYSTEM_REGISTERS_XML = EXDI_ROOT / "GdbSrvControllerLib" / "systemregisters.xml"
INSTALL_CONFIG_XML = INSTALL_DIR / "exdiConfigData.xml"
INSTALL_SYSTEM_REGISTERS_XML = INSTALL_DIR / "systemregisters.xml"
VMWARE_LOG = Path(tempfile.gettempdir()) / "ExdiGdbSrv-vmware.log"
DOC_PATH = ROOT / "docs" / "vmware-exdi-kernel-debugging.md"


def run_command(args, cwd=ROOT, check=True):
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
    output = ""
    if proc.stdout:
        output += proc.stdout
    if proc.stderr:
        output += proc.stderr
    if check and proc.returncode != 0:
        raise RuntimeError(f"Command failed ({proc.returncode}): {' '.join(map(str, args))}\n{output}")
    return proc.returncode, output


def first_existing(paths):
    for path in paths:
        if path and Path(path).exists():
            return str(path)
    return None


def find_msbuild():
    env_path = os.environ.get("EXDI_MSBUILD")
    if env_path and Path(env_path).exists():
        return env_path

    candidates = [
        r"C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe",
        r"C:\Program Files\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\amd64\MSBuild.exe",
        r"C:\Program Files\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\amd64\MSBuild.exe",
        r"C:\Program Files\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe",
        r"C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\MSBuild\Current\Bin\amd64\MSBuild.exe",
        r"C:\Program Files (x86)\Microsoft Visual Studio\2019\Professional\MSBuild\Current\Bin\amd64\MSBuild.exe",
        r"C:\Program Files (x86)\Microsoft Visual Studio\2019\Enterprise\MSBuild\Current\Bin\amd64\MSBuild.exe",
        r"C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe",
    ]
    found = first_existing(candidates)
    if found:
        return found

    vswhere = Path(r"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe")
    if vswhere.exists():
        try:
            _, output = run_command(
                [
                    str(vswhere),
                    "-latest",
                    "-products",
                    "*",
                    "-requires",
                    "Microsoft.Component.MSBuild",
                    "-find",
                    r"MSBuild\**\Bin\amd64\MSBuild.exe",
                ],
                check=False,
            )
            for line in output.splitlines():
                line = line.strip()
                if line and Path(line).exists():
                    return line
        except Exception:
            pass

    return shutil.which("MSBuild.exe")


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
    found = first_existing(candidates)
    return found or "windbgx.exe"


def read_registry_target():
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, REG_PATH) as key:
            value, _ = winreg.QueryValueEx(key, None)
            threading_model, _ = winreg.QueryValueEx(key, "ThreadingModel")
            return value, threading_model
    except FileNotFoundError:
        return None, None
    except OSError:
        return None, None


def set_registry_target():
    with winreg.CreateKeyEx(winreg.HKEY_CURRENT_USER, REG_PATH, 0, winreg.KEY_SET_VALUE) as key:
        winreg.SetValueEx(key, None, 0, winreg.REG_SZ, str(INSTALL_DLL))
        winreg.SetValueEx(key, "ThreadingModel", 0, winreg.REG_SZ, "Apartment")


def delete_registry_tree(root, path):
    try:
        with winreg.OpenKey(root, path, 0, winreg.KEY_READ | winreg.KEY_WRITE) as key:
            while True:
                try:
                    subkey = winreg.EnumKey(key, 0)
                    delete_registry_tree(root, path + "\\" + subkey)
                except OSError:
                    break
        winreg.DeleteKey(root, path)
    except FileNotFoundError:
        return


def parse_config():
    if not CONFIG_XML.exists():
        return {}
    text = CONFIG_XML.read_text(encoding="utf-8-sig", errors="replace")
    result = {}
    m = re.search(r"<ExdiTargets\s+CurrentTarget\s*=\s*\"([^\"]+)\"", text)
    if m:
        result["CurrentTarget"] = m.group(1)
    vmware = re.search(r"<ExdiTarget\s+Name\s*=\s*\"VMWare\"[\s\S]*?</ExdiTarget>", text)
    if vmware:
        block = vmware.group(0)
        for key in ("displayCommPackets", "gdbMonitorCmdDoNotWaitOnOK"):
            attr = re.search(rf"{key}\s*=\s*\"([^\"]+)\"", block)
            if attr:
                result[key] = attr.group(1)
        host = re.search(r"HostNameAndPort\s*=\s*\"([^\"]+)\"", block)
        if host:
            result["HostNameAndPort"] = host.group(1)
    return result


def file_status(path):
    if not path.exists():
        return "missing"
    stat = path.stat()
    return f"ok, {stat.st_size} bytes, {format_mtime(stat.st_mtime)}"


def format_mtime(ts):
    import datetime

    return datetime.datetime.fromtimestamp(ts).strftime("%Y-%m-%d %H:%M:%S")


def git_line(args):
    try:
        _, output = run_command(["git", "-C", str(ROOT)] + args, check=False)
        return output.strip()
    except Exception as exc:
        return f"git unavailable: {exc}"


def collect_status_text():
    registry_target, threading_model = read_registry_target()
    config = parse_config()
    msbuild = find_msbuild()
    windbg = find_windbg()

    lines = [
        f"仓库根目录: {ROOT}",
        f"Git 分支: {git_line(['branch', '--show-current'])}",
        f"最近提交: {git_line(['log', '--oneline', '--decorate', '-3'])}",
        "",
        f"MSBuild: {msbuild or '未找到'}",
        f"WinDbg: {windbg}",
        "",
        f"Release DLL: {file_status(RELEASE_DLL)}",
        f"Install DLL: {file_status(INSTALL_DLL)}",
        f"Install PDB: {file_status(INSTALL_PDB)}",
        f"Install config: {file_status(INSTALL_CONFIG_XML)}",
        "",
        f"COM 注册: {registry_target or '未注册'}",
        f"ThreadingModel: {threading_model or '未设置'}",
        f"COM 指向当前部署: {'是' if registry_target and Path(registry_target).resolve() == INSTALL_DLL.resolve() else '否'}",
        "",
        f"CurrentTarget: {config.get('CurrentTarget', '未知')}",
        f"VMware HostNameAndPort: {config.get('HostNameAndPort', '未知')}",
        f"displayCommPackets: {config.get('displayCommPackets', '未知')}",
        f"gdbMonitorCmdDoNotWaitOnOK: {config.get('gdbMonitorCmdDoNotWaitOnOK', '未知')}",
        "",
        f"VMware 日志: {file_status(VMWARE_LOG)}",
    ]
    return "\n".join(lines)


class ExdiManager(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("VMware EXDI Manager")
        self.geometry("980x720")
        self.minsize(820, 560)
        self.task_running = False
        self.buttons = []
        self._build_ui()
        self.refresh_status()

    def _build_ui(self):
        root = ttk.Frame(self, padding=10)
        root.pack(fill=tk.BOTH, expand=True)

        ttk.Label(root, text="状态").pack(anchor=tk.W)
        self.status_text = tk.Text(root, height=14, wrap=tk.WORD)
        self.status_text.pack(fill=tk.BOTH, expand=False)

        button_frame = ttk.Frame(root)
        button_frame.pack(fill=tk.X, pady=8)
        self._add_button(button_frame, "刷新状态", self.refresh_status)
        self._add_button(button_frame, "构建 Release", lambda: self.run_task("构建 Release", self.build_release))
        self._add_button(button_frame, "安装/部署", lambda: self.run_task("构建并部署", self.build_and_deploy))
        self._add_button(button_frame, "仅部署现有构建", lambda: self.run_task("部署现有构建", self.deploy_current_build))
        self._add_button(button_frame, "清理卸载", lambda: self.confirm_uninstall())
        self._add_button(button_frame, "启动 WinDbg", lambda: self.run_task("启动 WinDbg", self.start_windbg))
        self._add_button(button_frame, "清空日志", lambda: self.run_task("清空日志", self.clear_log))
        self._add_button(button_frame, "打开日志", self.open_log)
        self._add_button(button_frame, "打开文档", self.open_doc)

        ttk.Label(root, text="操作日志").pack(anchor=tk.W)
        self.log_text = tk.Text(root, height=18, wrap=tk.WORD)
        self.log_text.pack(fill=tk.BOTH, expand=True)

    def _add_button(self, frame, text, command):
        button = ttk.Button(frame, text=text, command=command)
        button.pack(side=tk.LEFT, padx=3, pady=3)
        self.buttons.append(button)

    def set_buttons_enabled(self, enabled):
        state = tk.NORMAL if enabled else tk.DISABLED
        for button in self.buttons:
            button.configure(state=state)

    def log(self, message):
        def append():
            self.log_text.insert(tk.END, message.rstrip() + "\n")
            self.log_text.see(tk.END)

        self.after(0, append)

    def refresh_status(self):
        status = collect_status_text()
        self.status_text.configure(state=tk.NORMAL)
        self.status_text.delete("1.0", tk.END)
        self.status_text.insert(tk.END, status)
        self.status_text.configure(state=tk.DISABLED)

    def run_task(self, title, func):
        if self.task_running:
            return
        self.task_running = True
        self.set_buttons_enabled(False)
        self.log(f"== {title} ==")

        def worker():
            try:
                func()
                self.log(f"{title} 完成")
            except Exception as exc:
                self.log(f"{title} 失败: {exc}")
                self.after(0, lambda: messagebox.showerror(title, str(exc)))
            finally:
                self.after(0, self.refresh_status)
                self.after(0, lambda: self.set_buttons_enabled(True))
                self.task_running = False

        threading.Thread(target=worker, daemon=True).start()

    def build_release(self):
        msbuild = find_msbuild()
        if not msbuild:
            raise RuntimeError("未找到 MSBuild。安装 Visual Studio Build Tools，或设置 EXDI_MSBUILD 环境变量。")
        args = [
            msbuild,
            str(SOLUTION),
            "/m",
            "/nr:false",
            "/p:Configuration=Release",
            "/p:Platform=x64",
            "/p:PlatformToolset=v142",
            "/v:minimal",
        ]
        self.log("运行: " + " ".join(args))
        _, output = run_command(args, cwd=ROOT, check=True)
        self.log(output or "MSBuild 没有输出。")
        if not RELEASE_DLL.exists():
            raise RuntimeError(f"构建完成但未找到 {RELEASE_DLL}")

    def kill_debuggers(self):
        process_names = ["windbgx.exe", "WinDbgX.exe", "DbgX.Shell.exe", "windbg.exe"]
        for name in process_names:
            run_command(["taskkill", "/IM", name, "/F"], check=False)
        _, output = run_command(["tasklist", "/m", "ExdiGdbSrv.dll"], check=False)
        for line in output.splitlines():
            match = re.match(r"^\S+\s+(\d+)\s", line)
            if match:
                run_command(["taskkill", "/PID", match.group(1), "/F"], check=False)

    def copy_deployment_files(self):
        if not RELEASE_DLL.exists():
            raise RuntimeError(f"未找到 {RELEASE_DLL}。请先构建。")
        if not RELEASE_PDB.exists():
            raise RuntimeError(f"未找到 {RELEASE_PDB}。请先构建。")
        INSTALL_DIR.mkdir(parents=True, exist_ok=True)
        shutil.copy2(RELEASE_DLL, INSTALL_DLL)
        shutil.copy2(RELEASE_PDB, INSTALL_PDB)
        shutil.copy2(CONFIG_XML, INSTALL_CONFIG_XML)
        shutil.copy2(SYSTEM_REGISTERS_XML, INSTALL_SYSTEM_REGISTERS_XML)

    def deploy_current_build(self):
        self.log("结束 WinDbg / DbgX / 已加载 ExdiGdbSrv.dll 的进程")
        self.kill_debuggers()
        self.log("复制 DLL/PDB/XML 到 local-install")
        self.copy_deployment_files()
        self.log("写入 HKCU COM 注册")
        set_registry_target()
        self.clear_log_file_only()

    def build_and_deploy(self):
        self.build_release()
        self.deploy_current_build()

    def confirm_uninstall(self):
        if not messagebox.askyesno("清理卸载", "这会结束 WinDbg、删除 HKCU COM 注册、删除 local-install 中的部署文件，并清空 VMware EXDI 日志。继续？"):
            return
        self.run_task("清理卸载", self.uninstall)

    def uninstall(self):
        self.kill_debuggers()
        delete_registry_tree(winreg.HKEY_CURRENT_USER, rf"Software\Classes\CLSID\{CLSID}")
        for path in (INSTALL_DLL, INSTALL_PDB, INSTALL_CONFIG_XML, INSTALL_SYSTEM_REGISTERS_XML):
            try:
                path.unlink()
            except FileNotFoundError:
                pass
        self.clear_log_file_only()

    def start_windbg(self):
        registry_target, _ = read_registry_target()
        if not INSTALL_DLL.exists():
            raise RuntimeError("当前未部署 ExdiGdbSrv.dll。请先点击“安装/部署”。")
        if not registry_target or Path(registry_target).resolve() != INSTALL_DLL.resolve():
            raise RuntimeError("COM 注册未指向当前 local-install。请先点击“安装/部署”。")

        windbg = find_windbg()
        args = [windbg, "-v", "-kx", WINDBG_KX]
        self.log("启动: " + " ".join(map(str, args)))
        subprocess.Popen(args, cwd=str(ROOT))

    def clear_log_file_only(self):
        try:
            VMWARE_LOG.unlink()
        except FileNotFoundError:
            pass

    def clear_log(self):
        self.clear_log_file_only()
        self.log_text.delete("1.0", tk.END)

    def open_log(self):
        if not VMWARE_LOG.exists():
            messagebox.showinfo("打开日志", f"日志不存在:\n{VMWARE_LOG}")
            return
        os.startfile(str(VMWARE_LOG))

    def open_doc(self):
        if not DOC_PATH.exists():
            messagebox.showinfo("打开文档", f"文档不存在:\n{DOC_PATH}")
            return
        os.startfile(str(DOC_PATH))


def main():
    if "--status" in sys.argv:
        print(collect_status_text())
        return
    app = ExdiManager()
    app.mainloop()


if __name__ == "__main__":
    main()
