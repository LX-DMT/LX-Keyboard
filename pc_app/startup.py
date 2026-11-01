"""Opt-in Windows logon startup for this configuration app, current user only."""
from pathlib import Path
import subprocess
import sys
import winreg
import os

RUN_KEY=r'Software\Microsoft\Windows\CurrentVersion\Run'
VALUE_NAME='LXKeyboardSetup'
APPROVED_KEY=r'Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved'

def launch_arguments():
    if getattr(sys,'frozen',False):
        arguments=[str(Path(sys.executable).resolve()),'--startup']
    else:
        python=Path(sys.executable).resolve()
        windowed=python.with_name('pythonw.exe')
        if windowed.is_file():python=windowed
        arguments=[str(python),str(Path(__file__).with_name('main.py').resolve()),'--startup']
    return arguments

def launch_command():return subprocess.list2cmdline(launch_arguments())

def shortcut_path():
    import ctypes as C
    from ctypes import wintypes as W
    shell=C.WinDLL('shell32')
    shell.SHGetFolderPathW.argtypes=[W.HWND,C.c_int,W.HANDLE,W.DWORD,W.LPWSTR]
    shell.SHGetFolderPathW.restype=C.c_long
    folder=C.create_unicode_buffer(260)
    if shell.SHGetFolderPathW(None,7,None,0,folder):raise OSError('Cannot locate Windows Startup folder')
    return Path(folder.value)/(VALUE_NAME+'.lnk')

def approved(kind,name):
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER,APPROVED_KEY+'\\'+kind,0,winreg.KEY_READ) as key:
            data,_=winreg.QueryValueEx(key,name)
            return not (isinstance(data,bytes) and data and data[0] in (3,7))
    except FileNotFoundError:return True

def legacy_enabled():
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER,RUN_KEY,0,winreg.KEY_READ) as key:
            value,kind=winreg.QueryValueEx(key,VALUE_NAME)
            return kind==winreg.REG_SZ and bool(value) and approved('Run',VALUE_NAME)
    except FileNotFoundError:return False

def is_enabled():
    path=shortcut_path()
    if path.is_file():return approved('StartupFolder',path.name)
    return legacy_enabled()

def remove_legacy():
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER,RUN_KEY,0,winreg.KEY_SET_VALUE) as key:
            winreg.DeleteValue(key,VALUE_NAME)
    except FileNotFoundError:pass

def set_enabled(enabled):
    if enabled:
        path=shortcut_path();path.parent.mkdir(parents=True,exist_ok=True)
        arguments=launch_arguments()
        env=os.environ.copy()
        env.update(LX_STARTUP_LINK=str(path),LX_STARTUP_EXE=arguments[0],
                   LX_STARTUP_ARGS=subprocess.list2cmdline(arguments[1:]),LX_STARTUP_DIR=str(Path(arguments[0]).parent))
        script="""$ErrorActionPreference='Stop'
$shell=New-Object -ComObject WScript.Shell
$link=$shell.CreateShortcut($env:LX_STARTUP_LINK)
$link.TargetPath=$env:LX_STARTUP_EXE
$link.Arguments=$env:LX_STARTUP_ARGS
$link.WorkingDirectory=$env:LX_STARTUP_DIR
$link.Description='蓝星键盘配置与旋钮同步'
$link.Save()
"""
        try:
            result=subprocess.run(['powershell.exe','-NoProfile','-NonInteractive','-Command',script],env=env,
                                  capture_output=True,creationflags=subprocess.CREATE_NO_WINDOW,timeout=15)
        except subprocess.TimeoutExpired as error:raise OSError('Windows Startup registration timed out') from error
        if result.returncode or not path.is_file():raise OSError('Cannot create Windows Startup shortcut')
        # Explicit enable also clears a Task Manager disable for this app only.
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER,APPROVED_KEY+r'\StartupFolder',0,winreg.KEY_SET_VALUE) as key:
                winreg.DeleteValue(key,path.name)
        except FileNotFoundError:pass
        remove_legacy()
    else:
        shortcut_path().unlink(missing_ok=True)
        remove_legacy()

def setup_log():
    import logging
    from logging.handlers import RotatingFileHandler
    logger=logging.getLogger('lx.lifecycle');logger.setLevel(logging.INFO)
    try:
        folder=Path(os.environ['LOCALAPPDATA'])/'LXKeyboardSetup'
        folder.mkdir(parents=True,exist_ok=True)
        handler=RotatingFileHandler(folder/'startup.log',maxBytes=65536,backupCount=1,encoding='utf8')
        handler.setFormatter(logging.Formatter('%(asctime)s %(levelname)s %(message)s'))
        logger.addHandler(handler)
    except (OSError,KeyError):logger.addHandler(logging.NullHandler())
    return logger

class SingleInstance:
    """Keep the mutex alive for the full app lifetime; reuse an existing window."""
    def __init__(self):
        import ctypes as C
        from ctypes import wintypes as W
        self.kernel=C.WinDLL('kernel32',use_last_error=True)
        self.kernel.CreateMutexW.argtypes=[C.c_void_p,W.BOOL,W.LPCWSTR]
        self.kernel.CreateMutexW.restype=W.HANDLE
        self.kernel.CloseHandle.argtypes=[W.HANDLE]
        self.handle=self.kernel.CreateMutexW(None,False,r'Local\LXKeyboardSetup.Standalone20261025')
        if not self.handle:raise C.WinError(C.get_last_error())
        self.existing=C.get_last_error()==183

    def show_existing(self,title):
        import ctypes as C
        from ctypes import wintypes as W
        user=C.WinDLL('user32',use_last_error=True)
        user.FindWindowW.argtypes=[W.LPCWSTR,W.LPCWSTR];user.FindWindowW.restype=W.HWND
        user.PostMessageW.argtypes=[W.HWND,W.UINT,W.WPARAM,W.LPARAM]
        from tray import CLASS_NAME,SHOW_MESSAGE
        tray_window=user.FindWindowW(CLASS_NAME,None)
        if tray_window:
            user.PostMessageW(tray_window,SHOW_MESSAGE,0,0);return
        user.IsIconic.argtypes=[W.HWND];user.ShowWindow.argtypes=[W.HWND,C.c_int]
        user.SetForegroundWindow.argtypes=[W.HWND]
        window=user.FindWindowW(None,title)
        if window:
            user.ShowWindow(window,9)
            user.SetForegroundWindow(window)

    def close(self):
        if self.handle:self.kernel.CloseHandle(self.handle);self.handle=None
