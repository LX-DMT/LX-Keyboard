"""Compact configuration notices sharing the app's rounded visual style."""
import tkinter as tk
from ui_widgets import BG, WHITE, GREEN, SOFT, Surface, Button, icon_image, scale


class NoticeDialog:
    def __init__(self,parent,title,message,icon='usb',compact=False):
        from appearance import label,frame
        self.parent=parent.winfo_toplevel();self.closed=False
        self.previous_grab=self.parent.grab_current()
        self.previous_focus=self.parent.focus_get()
        # A child layer cannot fall behind its owner when Snipping Tool or
        # another application takes focus, unlike a borderless Toplevel.
        self.window=window=tk.Frame(self.parent,bg=BG)
        px=lambda n:round(n*scale(window))
        card=Surface(window,fill=WHITE,radius=18,padding=(24,22));card.pack(fill='both',expand=True)
        top=frame(card.body);top.pack(fill='x')
        emblem=Surface(top,fill=SOFT,radius=13,padding=(8,8) if compact else (10,10),border=SOFT)
        emblem.pack(side='left',padx=(0,px(12 if compact else 14)))
        picture=icon_image(window,icon,22 if compact else 26,GREEN)
        mark=tk.Label(emblem.body,image=picture,bg=SOFT,bd=0);mark.image=picture;mark.pack()
        if compact:
            label(top,message,size=13,wrap=px(312)).pack(side='left')
        else:
            label(top,title,size=19,bold=True).pack(side='left')
            text,note_sep,note=message.partition('\n')
            label(card.body,text,size=12,wrap=px(362)).pack(fill='x',pady=(px(18),0))
            if note_sep and note:
                hint=Surface(card.body,fill=SOFT,radius=10,padding=(12,10),border=SOFT)
                hint.pack(fill='x',pady=(px(12),0))
                label(hint.body,note,muted=True,size=10,wrap=px(338)).pack(fill='x')
        row=frame(card.body);row.pack(fill='x',pady=(px(22),0))
        self.confirm=Button(row,text='知道了',kind='primary',width=104,height=36,command=self.close)
        self.confirm.pack(side='right')
        self.key_tag=f'ConfigNotice_{id(self)}'
        def tag_widgets(widget):
            widget.bindtags((self.key_tag,)+widget.bindtags())
            for child in widget.winfo_children():tag_widgets(child)
        tag_widgets(window)
        window.bind_class(self.key_tag,'<Escape>',self.close)
        window.bind_class(self.key_tag,'<Return>',self.close)
        window.bind_class(self.key_tag,'<Tab>',self.cycle_focus)
        window.bind_class(self.key_tag,'<Shift-Tab>',self.cycle_focus)
        window.bind_class(self.key_tag,'<MouseWheel>',lambda e:'break')
        window.update_idletasks()
        width=px(410);height=window.winfo_reqheight()
        window.place(relx=.5,rely=.5,anchor='center',width=width,height=height)
        window.update_idletasks()
        self.unmap_binding=self.parent.bind('<Unmap>',self.parent_hidden,add='+')
        self.focus_binding=self.parent.bind('<FocusIn>',self.parent_focused,add='+')
        window.lift();window.grab_set();self.confirm.focus_set()

    def parent_focused(self,event):
        if event.widget is self.parent and not self.closed:
            self.window.lift();self.confirm.focus_set()

    def cycle_focus(self,event=None):
        self.confirm.focus_set();return 'break'

    def parent_hidden(self,event):
        if event.widget is self.parent:self.close()

    def close(self,event=None):
        if self.closed:return 'break'
        self.closed=True
        if self.parent.winfo_exists():
            self.parent.unbind('<Unmap>',self.unmap_binding)
            self.parent.unbind('<FocusIn>',self.focus_binding)
            if getattr(self.parent,'_config_notice',None) is self:self.parent._config_notice=None
        if self.window.winfo_exists():
            if self.window.grab_current() is self.window:self.window.grab_release()
            for event in ('<Escape>','<Return>','<Tab>','<Shift-Tab>','<MouseWheel>'):
                self.window.unbind_class(self.key_tag,event)
            self.window.destroy()
        if self.previous_grab is not None and self.previous_grab.winfo_exists() and self.previous_grab.winfo_viewable():
            self.previous_grab.grab_set()
        if self.previous_focus is not None and self.previous_focus.winfo_exists() and self.previous_focus.winfo_viewable():
            self.previous_focus.focus_set()
        return 'break'


def show_notice(title,message,*,parent,icon='usb',compact=False):
    owner=parent.winfo_toplevel()
    current=getattr(owner,'_config_notice',None)
    if current and not current.closed and current.window.winfo_exists():
        current.window.lift();current.confirm.focus_set();return current
    notice=NoticeDialog(owner,title,message,icon,compact)
    owner._config_notice=notice
    return notice
