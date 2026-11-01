"""Give Windows native-size icon handles; Tk iconbitmap reuses the large handle."""
import ctypes as C
from ctypes import wintypes as W
from pathlib import Path
import logging

class WindowIcons:
    def __init__(self,root):
        self.root=root;self.handles={};self.applied=None;self.pending=False;self.closed=False
        self.user=C.WinDLL('user32',use_last_error=True)
        for name,args,result in (
            ('GetDpiForWindow',[W.HWND],W.UINT),
            ('GetSystemMetricsForDpi',[C.c_int,W.UINT],C.c_int),
            ('LoadImageW',[W.HINSTANCE,W.LPCWSTR,W.UINT,C.c_int,C.c_int,W.UINT],W.HANDLE),
            ('SendMessageW',[W.HWND,W.UINT,W.WPARAM,W.LPARAM],C.c_ssize_t),
            ('DestroyIcon',[W.HICON],W.BOOL)):
            fn=getattr(self.user,name);fn.argtypes=args;fn.restype=result
        root.bind('<Map>',self.schedule,add='+')
        root.bind('<Configure>',self.schedule,add='+')
        root.bind('<Destroy>',self.destroy,add='+')
        self.schedule()

    def schedule(self,event=None):
        if event is not None and event.widget is not self.root:return
        if event is not None and str(event.type)=='19':self.applied=None
        if self.closed or self.pending:return
        self.pending=True;self.root.after_idle(self.apply)

    def apply(self):
        self.pending=False
        if self.closed:return
        hwnd=int(self.root.wm_frame(),0);dpi=self.user.GetDpiForWindow(hwnd) or 96
        if self.applied==(hwnd,dpi):return
        try:
            for kind,metric in ((0,49),(1,11)):
                size=self.user.GetSystemMetricsForDpi(metric,dpi)
                if size not in self.handles:
                    icon=self.user.LoadImageW(None,str(Path(__file__).with_name('assets')/'lx.ico'),1,size,size,0x10)
                    if not icon:raise C.WinError(C.get_last_error())
                    self.handles[size]=icon
                self.user.SendMessageW(hwnd,0x80,kind,self.handles[size])
            self.applied=(hwnd,dpi)
        except OSError:logging.getLogger('lx.lifecycle').exception('native window icon failed')

    def destroy(self,event):
        if event.widget is not self.root:return
        self.closed=True
        for handle in self.handles.values():self.user.DestroyIcon(handle)
        self.handles.clear()
