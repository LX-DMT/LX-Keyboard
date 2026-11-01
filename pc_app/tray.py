"""Native Windows notification icon. Callbacks are queued back to Tk's thread."""
import ctypes as C
from ctypes import wintypes as W
from pathlib import Path
import threading

CLASS_NAME='LXKeyboardSetupTray'
CALLBACK_MESSAGE=0x8014
SHOW_MESSAGE=0x8015
STOP_MESSAGE=0x8016
ICON_PATH=Path(__file__).with_name('assets')/'lx.ico'
TRAY_ICON_PATH=Path(__file__).with_name('assets')/'tray.ico'
WNDPROC=C.WINFUNCTYPE(C.c_ssize_t,W.HWND,W.UINT,W.WPARAM,W.LPARAM)

class WindowClass(C.Structure):
    _fields_=[('style',W.UINT),('proc',WNDPROC),('cls_extra',C.c_int),('win_extra',C.c_int),
              ('instance',W.HINSTANCE),('icon',W.HICON),('cursor',W.HANDLE),('background',W.HBRUSH),
              ('menu',W.LPCWSTR),('name',W.LPCWSTR)]

class NotifyIcon(C.Structure):
    _fields_=[('size',W.DWORD),('window',W.HWND),('id',W.UINT),('flags',W.UINT),('callback',W.UINT),
              ('icon',W.HICON),('tip',W.WCHAR*128),('state',W.DWORD),('mask',W.DWORD),
              ('info',W.WCHAR*256),('timeout',W.UINT),('info_title',W.WCHAR*64),
              ('info_flags',W.DWORD),('guid',C.c_byte*16),('balloon_icon',W.HICON)]

class Tray:
    def __init__(self,emit):
        self.emit=emit;self.window=None;self.thread=None;self.available=False;self.error=None
        self.ready=threading.Event();self.stopping=threading.Event()
        self.user=C.WinDLL('user32',use_last_error=True)
        self.shell=C.WinDLL('shell32',use_last_error=True)
        self.kernel=C.WinDLL('kernel32',use_last_error=True)
        signatures={
            'RegisterClassW':([C.POINTER(WindowClass)],W.ATOM),
            'CreateWindowExW':([W.DWORD,W.LPCWSTR,W.LPCWSTR,W.DWORD,C.c_int,C.c_int,C.c_int,C.c_int,W.HWND,W.HMENU,W.HINSTANCE,C.c_void_p],W.HWND),
            'DefWindowProcW':([W.HWND,W.UINT,W.WPARAM,W.LPARAM],C.c_ssize_t),
            'LoadImageW':([W.HINSTANCE,W.LPCWSTR,W.UINT,C.c_int,C.c_int,W.UINT],W.HANDLE),
            'GetSystemMetrics':([C.c_int],C.c_int),
            'RegisterWindowMessageW':([W.LPCWSTR],W.UINT),
            'GetMessageW':([C.POINTER(W.MSG),W.HWND,W.UINT,W.UINT],W.BOOL),
            'TranslateMessage':([C.POINTER(W.MSG)],W.BOOL),
            'DispatchMessageW':([C.POINTER(W.MSG)],C.c_ssize_t),
            'PostMessageW':([W.HWND,W.UINT,W.WPARAM,W.LPARAM],W.BOOL),
            'PostQuitMessage':([C.c_int],None),
            'DestroyWindow':([W.HWND],W.BOOL),
            'DestroyIcon':([W.HICON],W.BOOL),
            'UnregisterClassW':([W.LPCWSTR,W.HINSTANCE],W.BOOL),
            'CreatePopupMenu':([],W.HMENU),
            'AppendMenuW':([W.HMENU,W.UINT,C.c_size_t,W.LPCWSTR],W.BOOL),
            'GetCursorPos':([C.POINTER(W.POINT)],W.BOOL),
            'SetForegroundWindow':([W.HWND],W.BOOL),
            'TrackPopupMenu':([W.HMENU,W.UINT,C.c_int,C.c_int,C.c_int,W.HWND,C.c_void_p],W.UINT),
            'DestroyMenu':([W.HMENU],W.BOOL),
        }
        for name,(args,result) in signatures.items():
            fn=getattr(self.user,name);fn.argtypes=args;fn.restype=result
        self.kernel.GetModuleHandleW.argtypes=[W.LPCWSTR];self.kernel.GetModuleHandleW.restype=W.HMODULE
        self.shell.Shell_NotifyIconW.argtypes=[W.DWORD,C.POINTER(NotifyIcon)]
        self.shell.Shell_NotifyIconW.restype=W.BOOL

    def start(self):
        self.thread=threading.Thread(target=self._run,name='LX tray',daemon=True);self.thread.start()
        if not self.ready.wait(4):
            self.stop();return False
        return self.available

    def _add(self):
        self.available=bool(self.shell.Shell_NotifyIconW(0,C.byref(self.data)))

    def _menu(self,window):
        menu=self.user.CreatePopupMenu()
        if not menu:return
        try:
            self.user.AppendMenuW(menu,0,1,'打开上位机')
            self.user.AppendMenuW(menu,0x800,0,None)
            self.user.AppendMenuW(menu,0,2,'退出')
            point=W.POINT();self.user.GetCursorPos(C.byref(point))
            self.user.SetForegroundWindow(window)
            chosen=self.user.TrackPopupMenu(menu,0x182,point.x,point.y,0,window,None)
            self.user.PostMessageW(window,0,0,0)
            if chosen:self.emit('show' if chosen==1 else 'quit')
        finally:self.user.DestroyMenu(menu)

    def _run(self):
        instance=self.kernel.GetModuleHandleW(None);icon=None;registered=False
        taskbar=self.user.RegisterWindowMessageW('TaskbarCreated')
        @WNDPROC
        def proc(window,message,wp,lp):
            try:
                if message==CALLBACK_MESSAGE:
                    if lp==0x202:self.emit('show') # Left-button release.
                    elif lp in (0x205,0x7b):self._menu(window)
                    return 0
                if message==SHOW_MESSAGE:self.emit('show');return 0
                if message==STOP_MESSAGE:self.user.DestroyWindow(window);return 0
                if taskbar and message==taskbar:self._add();return 0
                if message==2:self.window=None;self.user.PostQuitMessage(0);return 0
            except Exception as error:self.error=error
            return self.user.DefWindowProcW(window,message,wp,lp)
        self.proc=proc
        try:
            cls=WindowClass(proc=proc,instance=instance,name=CLASS_NAME)
            if not self.user.RegisterClassW(C.byref(cls)):raise C.WinError(C.get_last_error())
            registered=True
            self.window=self.user.CreateWindowExW(0,CLASS_NAME,'LX Keyboard Setup tray',0,0,0,0,0,None,None,instance,None)
            if not self.window:raise C.WinError(C.get_last_error())
            if hasattr(self.user,'GetSystemMetricsForDpi'):
                self.user.GetDpiForSystem.argtypes=[];self.user.GetDpiForSystem.restype=W.UINT
                self.user.GetSystemMetricsForDpi.argtypes=[C.c_int,W.UINT];self.user.GetSystemMetricsForDpi.restype=C.c_int
                dpi=self.user.GetDpiForSystem()
                width=self.user.GetSystemMetricsForDpi(49,dpi);height=self.user.GetSystemMetricsForDpi(50,dpi)
            else:
                width=self.user.GetSystemMetrics(49);height=self.user.GetSystemMetrics(50)
            self.icon_size=(width,height)
            icon=self.user.LoadImageW(None,str(TRAY_ICON_PATH),1,width,height,0x10)
            if not icon:raise C.WinError(C.get_last_error())
            self.data=NotifyIcon(size=C.sizeof(NotifyIcon),window=self.window,id=1,flags=7,
                                 callback=CALLBACK_MESSAGE,icon=icon,tip='蓝星键盘 · 右键退出')
            self._add()
            if not self.available:raise OSError('Unable to register Windows tray icon')
            if self.stopping.is_set():return
            self.ready.set();message=W.MSG()
            while True:
                result=self.user.GetMessageW(C.byref(message),None,0,0)
                if result==0:break
                if result==-1:raise C.WinError(C.get_last_error())
                self.user.TranslateMessage(C.byref(message));self.user.DispatchMessageW(C.byref(message))
        except Exception as error:self.error=error
        finally:
            if hasattr(self,'data'):self.shell.Shell_NotifyIconW(2,C.byref(self.data))
            self.available=False;self.ready.set()
            if self.window:self.user.DestroyWindow(self.window);self.window=None
            if icon:self.user.DestroyIcon(icon)
            if registered:self.user.UnregisterClassW(CLASS_NAME,instance)

    def stop(self):
        self.stopping.set()
        if self.window:self.user.PostMessageW(self.window,STOP_MESSAGE,0,0)
        if self.thread and self.thread is not threading.current_thread():self.thread.join(3)
