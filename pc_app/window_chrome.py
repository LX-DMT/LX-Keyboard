"""Integrated header with native Windows resizing, taskbar and minimize behavior."""
import ctypes as C
from ctypes import wintypes as W
import tkinter as tk
from PIL import Image, ImageDraw, ImageTk
from ui_widgets import GREEN, SOFT, scale

class MonitorInfo(C.Structure):
    _fields_=[('size',W.DWORD),('monitor',W.RECT),('work',W.RECT),('flags',W.DWORD)]


class WindowChrome:
    def __init__(self,app,header,brand):
        self.app=app;self.root=app.root;self.pending=False
        self.drag_origin=None;self.caption_zoomed=None
        self.user=C.WinDLL('user32',use_last_error=True)
        for name,args,result in (
            ('GetWindowLongPtrW',[W.HWND,C.c_int],C.c_ssize_t),
            ('SetWindowLongPtrW',[W.HWND,C.c_int,C.c_ssize_t],C.c_ssize_t),
            ('SetWindowPos',[W.HWND,W.HWND,C.c_int,C.c_int,C.c_int,C.c_int,W.UINT],W.BOOL),
            ('MonitorFromWindow',[W.HWND,W.DWORD],W.HANDLE),
            ('GetMonitorInfoW',[W.HANDLE,C.POINTER(MonitorInfo)],W.BOOL),
            ('GetWindowRect',[W.HWND,C.POINTER(W.RECT)],W.BOOL),
            ):
            fn=getattr(self.user,name);fn.argtypes=args;fn.restype=result
        controls=tk.Frame(header,bg=header.cget('background'))
        controls.pack(side='right',padx=(0,round(8*scale(header))))
        self.buttons=[]
        for name,command in (('minimize',self.root.iconify),('maximize',self.toggle_maximize),('close',app.hide_to_tray)):
            button=CaptionButton(controls,name,command,self.root)
            button.pack(side='left');self.buttons.append(button)
        def draggable(widget):
            widget.bind('<ButtonPress-1>',self.drag,add='+')
            widget.bind('<B1-Motion>',self.drag_motion,add='+')
            widget.bind('<ButtonRelease-1>',self.end_drag,add='+')
            widget.bind('<Double-Button-1>',lambda event:self.toggle_maximize(),add='+')
            for child in widget.winfo_children():draggable(child)
        draggable(brand)
        header.bind('<ButtonPress-1>',self.drag,add='+')
        header.bind('<B1-Motion>',self.drag_motion,add='+')
        header.bind('<ButtonRelease-1>',self.end_drag,add='+')
        header.bind('<Double-Button-1>',lambda event:self.toggle_maximize(),add='+')
        self.root.bind('<Map>',self.schedule,add='+')
        self.root.bind('<Configure>',self.schedule,add='+')
        self.schedule()

    def schedule(self,event=None):
        if event is not None and event.widget is not self.root:return
        if self.pending:return
        self.pending=True;self.root.after_idle(self.apply)

    def apply(self):
        self.pending=False
        if self.app.closing:return
        hwnd=int(self.root.wm_frame(),0)
        style=self.user.GetWindowLongPtrW(hwnd,-16)
        # Remove only the native caption. Keep thick frame/system menu/buttons
        # so Windows still owns resizing, maximize bounds and taskbar behavior.
        if style & 0x00C00000:
            self.user.SetWindowLongPtrW(hwnd,-16,style & ~0x00C00000)
            self.user.SetWindowPos(hwnd,None,0,0,0,0,0x37)
        # Keep Tk's native window procedure intact. Adjust maximized bounds from
        # the Tk event loop rather than re-entering Tcl through a ctypes callback.
        if self.root.state()=='zoomed':
            monitor=MonitorInfo();monitor.size=C.sizeof(monitor);rect=W.RECT()
            handle=self.user.MonitorFromWindow(hwnd,2)
            if self.user.GetMonitorInfoW(handle,C.byref(monitor)) and self.user.GetWindowRect(hwnd,C.byref(rect)):
                work=monitor.work
                if (rect.left,rect.top,rect.right,rect.bottom)!=(work.left,work.top,work.right,work.bottom):
                    self.user.SetWindowPos(hwnd,None,work.left,work.top,work.right-work.left,work.bottom-work.top,0x14)
        zoomed=self.root.state()=='zoomed'
        if self.caption_zoomed!=zoomed:
            self.caption_zoomed=zoomed
            for button in self.buttons:button.draw()

    def drag(self,event):
        self.drag_origin=(event.x_root,event.y_root,self.root.winfo_x(),self.root.winfo_y())
        return 'break'

    def drag_motion(self,event):
        if self.drag_origin is None:return
        sx,sy,x,y=self.drag_origin;dx=event.x_root-sx;dy=event.y_root-sy
        if abs(dx)+abs(dy)<4:return
        if self.root.state()=='zoomed':
            fraction=(sx-self.root.winfo_x())/max(1,self.root.winfo_width())
            self.root.state('normal');self.root.update_idletasks()
            x=round(event.x_root-fraction*self.root.winfo_width());y=event.y_root-round(24*scale(self.root))
            self.drag_origin=(event.x_root,event.y_root,x,y);dx=dy=0
        self.user.SetWindowPos(int(self.root.wm_frame(),0),None,x+dx,y+dy,0,0,0x15)
        return 'break'

    def end_drag(self,event):self.drag_origin=None

    def toggle_maximize(self):
        self.drag_origin=None
        self.root.state('normal' if self.root.state()=='zoomed' else 'zoomed')
        self.schedule()


class CaptionButton(tk.Canvas):
    def __init__(self,parent,name,command,root):
        self.name=name;self.command=command;self.root=root;self.hover=False;self.factor=scale(parent)
        self.background=parent.cget('background')
        super().__init__(parent,width=round(38*self.factor),height=round(34*self.factor),
                         bg=self.background,highlightthickness=0,bd=0,takefocus=True)
        self.bind('<Enter>',lambda e:self.set_hover(True))
        self.bind('<Leave>',lambda e:self.set_hover(False))
        self.bind('<ButtonRelease-1>',lambda e:self.command() if 0<=e.x<self.winfo_width() and 0<=e.y<self.winfo_height() else None)
        self.bind('<Return>',lambda e:self.command())
        self.bind('<space>',lambda e:self.command())
        self.bind('<FocusIn>',lambda e:self.draw());self.bind('<FocusOut>',lambda e:self.draw())
        self.draw()

    def set_hover(self,value):self.hover=value;self.draw()

    def draw(self):
        active=self.hover or self.focus_get() is self
        self.configure(bg='#c84242' if active and self.name=='close' else SOFT if active else self.background)
        color='white' if active and self.name=='close' else GREEN
        n=round(16*self.factor);factor=4;k=n*factor/16
        im=Image.new('RGBA',(n*factor,n*factor));d=ImageDraw.Draw(im);stroke=max(1,round(1.3*k))
        def line(points):d.line([(x*k,y*k) for x,y in points],fill=color,width=stroke)
        if self.name=='minimize':line([(3,8),(13,8)])
        elif self.name=='close':line([(3,3),(13,13)]);line([(13,3),(3,13)])
        elif self.root.state()=='zoomed':
            line([(5,4),(5,2),(14,2),(14,11),(12,11)])
            d.rectangle((2*k,5*k,11*k,14*k),outline=color,width=stroke)
        else:d.rectangle((3*k,3*k,13*k,13*k),outline=color,width=stroke)
        self.picture=ImageTk.PhotoImage(im.resize((n,n),Image.Resampling.LANCZOS),master=self)
        self.delete('all');self.create_image(round(19*self.factor),round(17*self.factor),image=self.picture)
