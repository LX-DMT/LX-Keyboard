"""Presentation for provisioning; device transactions stay in setup_wizard."""
import tkinter as tk
from tkinter import ttk
from dropdown import Dropdown
from appearance import label,frame,icon
from ui_widgets import BG,WHITE,INK,MUTED,GREEN,SOFT,LINE,Surface,Button,SlimScrollbar,font,icon_image

def build(w):
    win=w.window;px=lambda n:round(n*w.app.scale)
    win.configure(bg=BG)
    win.geometry(f'{min(px(680),win.winfo_screenwidth()-60)}x{min(px(720),win.winfo_screenheight()-100)}')
    win.minsize(px(560),px(500))
    header=tk.Frame(win,bg=WHITE,padx=px(28),pady=px(20));header.pack(fill='x')
    label(header,'添加设备与网络',size=24,bold=True).pack(anchor='w')
    steps=frame(win);steps.pack(fill='x',padx=px(28),pady=px(18))
    w.step_labels=[]
    for i,title in enumerate(('选择设备','连接网络','确认保存')):
        steps.columnconfigure(i,weight=1)
        cell=Surface(steps,fill=SOFT,radius=10,padding=(12,12),border=None)
        cell.grid(row=0,column=i,sticky='ew',padx=(0,px(8) if i<2 else 0))
        text=label(cell.body,f'0{i+1}  {title}',size=12,bold=True)
        text.pack(anchor='center');w.step_labels.append(text)
    footer=frame(win);footer.pack(side='bottom',fill='x',padx=px(28),pady=(px(12),px(22)))
    w.status=tk.StringVar(value='正在识别产品 USB 设备…')
    w.status_label=label(footer,variable=w.status,muted=True,size=11,wrap=px(590))
    w.status_label.pack(fill='x',pady=(0,px(14)))
    w.actions=frame(footer);w.actions.pack(fill='x')
    w.back=Button(w.actions,text='取消',command=w.previous,width=100);w.back.pack(side='left')
    w.next=Button(w.actions,text='下一步',kind='primary',command=w.advance,width=120)
    w.go=Button(w.actions,text='验证并保存',kind='primary',command=w.save,width=140)
    viewport=frame(win);viewport.pack(fill='both',expand=True)
    w.canvas=tk.Canvas(viewport,bg=BG,highlightthickness=0);w.canvas.pack(fill='both',expand=True)
    scroll=SlimScrollbar(viewport,command=w.canvas.yview)
    def scroll_set(first,last):
        scroll.set(first,last)
        if float(first)<=0 and float(last)>=1:scroll.place_forget()
        else:scroll.place(relx=1,x=-px(3),y=0,anchor='ne',relheight=1);scroll.tk.call('raise',scroll._w)
    w.canvas.configure(yscrollcommand=scroll_set)
    body=tk.Frame(w.canvas,bg=BG,padx=px(28),pady=px(2))
    item=w.canvas.create_window(0,0,window=body,anchor='nw')
    w.canvas.bind('<Configure>',lambda e:w.canvas.itemconfigure(item,width=e.width))
    body.bind('<Configure>',lambda e:w.canvas.configure(scrollregion=w.canvas.bbox('all')))
    win.bind('<MouseWheel>',lambda e:w.canvas.yview_scroll(-int(e.delta/120),'units') if body.winfo_height()>w.canvas.winfo_height() else None)
    w.pages=[frame(body) for _ in range(3)]
    style=ttk.Style(win)
    if 'LXSetup.downarrow' not in style.element_names():
        w.app.root.setup_arrows=(icon_image(w.app.root,'down',16,MUTED),icon_image(w.app.root,'down',16,'#b7c0bb'))
        style.element_create('LXSetup.downarrow','image',w.app.root.setup_arrows[0],('disabled',w.app.root.setup_arrows[1]),sticky='e')
    style.layout('Setup.TCombobox',[('Combobox.padding',{'sticky':'nswe','children':[('LXSetup.downarrow',{'side':'right','sticky':'ns'}),('Combobox.textarea',{'sticky':'nswe'})]})])
    style.layout('Setup.TEntry',[('Entry.padding',{'sticky':'nswe','children':[('Entry.textarea',{'sticky':'nswe'})]})])
    for name in ('Setup.TCombobox','Setup.TEntry'):
        style.configure(name,padding=6,borderwidth=0,background='white',fieldbackground='white',
                        foreground=INK,bordercolor='white',lightcolor='white',darkcolor='white',
                        selectbackground=SOFT,selectforeground=INK)
        style.map(name,foreground=[('disabled',MUTED),('!disabled',INK)],
                  background=[('disabled','white'),('!disabled','white')],
                  fieldbackground=[('disabled','white'),('!disabled','white')],
                  selectbackground=[('!disabled',SOFT)],selectforeground=[('!disabled',INK)])
    def card(page,title,glyph):
        surface=Surface(page,radius=16,padding=(22,20));surface.pack(fill='x',pady=(0,px(12)))
        row=frame(surface.body);row.pack(fill='x',pady=(0,px(16)))
        icon(row,glyph,22).pack(side='left',padx=(0,px(10)))
        label(row,title,size=16,bold=True).pack(side='left');return surface.body
    def field(parent,title,var,readonly=False,password=False):
        label(parent,title,size=11,muted=True).pack(anchor='w',pady=(px(8),px(7)))
        box=Surface(parent,radius=9,padding=(8,5));box.pack(fill='x',pady=(0,px(6)))
        entry=ttk.Entry(box.body,textvariable=var,show='•',style='Setup.TEntry') if password else Dropdown(box.body,textvariable=var,state='readonly' if readonly else 'normal',style='Setup.TCombobox')
        entry.field_surface=box
        entry.pack(fill='x')
        def focus_border(active):
            from types import SimpleNamespace
            box.border='#75a48f' if active else LINE
            box.draw(SimpleNamespace(width=box.canvas.winfo_width(),height=box.canvas.winfo_height()))
        entry.bind('<FocusIn>',lambda e:focus_border(True),add='+')
        entry.bind('<FocusOut>',lambda e:focus_border(False),add='+')
        return entry
    page=w.pages[0];fields=card(page,'选择要配置的设备','usb')
    w.keyboard=tk.StringVar();w.receiver=tk.StringVar(value='暂不添加接收器')
    w.kbox=field(fields,'键盘',w.keyboard,True);w.rbox=field(fields,'接收器 · 可选',w.receiver,True)
    w.discovery_detail=tk.StringVar()
    label(fields,variable=w.discovery_detail,muted=True,size=11,wrap=px(550)).pack(fill='x',pady=px(10))
    w.refresh=Button(fields,text='重新识别',command=w.discover);w.refresh.pack(anchor='w')
    label(page,'键盘切到 USB 模式并连接产品数据口；接收器直接插电脑。\n下载线不用于配网，插拔后会自动更新设备列表。',muted=True,size=11,wrap=px(580)).pack(fill='x',pady=px(5))
    page=w.pages[1];fields=card(page,'网络设置','wifi')
    w.ssid=tk.StringVar();w.password=tk.StringVar()
    w.sbox=field(fields,'2.4 GHz Wi-Fi 名称',w.ssid)
    # Display-only hint: never becomes an SSID or reaches device configuration.
    w.ssid_hint=tk.Label(w.sbox,text='请选择或输入 2.4 GHz Wi-Fi 名称',bg='white',fg=MUTED,
                         font=font(win),bd=0,padx=0,anchor='w',cursor='xterm')
    def update_hint(*args):
        if not w.ssid.get() and w.sbox.focus_get()!=w.sbox:
            w.ssid_hint.place(x=6,rely=.5,anchor='w',relwidth=1,width=-px(32))
        else:w.ssid_hint.place_forget()
    w.ssid_hint.bind('<Button-1>',lambda e:w.sbox.focus_set() if not w.sbox.instate(['disabled']) else None)
    hint_trace=w.ssid.trace_add('write',update_hint)
    w.sbox.bind('<FocusIn>',update_hint,add='+');w.sbox.bind('<FocusOut>',update_hint,add='+')
    w.sbox.bind('<Destroy>',lambda e:w.ssid.trace_remove('write',hint_trace) if e.widget==w.sbox else None,add='+')
    update_hint()
    w.scan_button=Button(fields,text='扫描附近网络',icon='wifi',command=w.scan);w.scan_button.pack(anchor='w',pady=(px(4),px(8)))
    w.password_entry=field(fields,'Wi-Fi 密码',w.password,password=True)
    label(page,'密码仅通过 USB 发送到设备，不保存在电脑或日志中。',muted=True,size=11,wrap=px(580)).pack(fill='x')
    page=w.pages[2];fields=card(page,'确认本次配置','check')
    w.summary=tk.StringVar()
    label(fields,variable=w.summary,size=12,wrap=px(540)).pack(fill='x')
    label(fields,'保存时会验证网络连接。完成后，请在键盘或旋钮上选择使用模式。',muted=True,size=11,wrap=px(540)).pack(fill='x',pady=(px(18),0))
    w.log_card=Surface(page,radius=16,padding=(20,16))
    label(w.log_card.body,'验证进度',size=13,bold=True).pack(anchor='w',pady=(0,px(8)))
    w.log=tk.Text(w.log_card.body,height=5,state='disabled',wrap='word',relief='flat',bd=0,
                  background='white',foreground='#597263',font=font(win,11),highlightthickness=0)
    w.log.pack(fill='both',expand=True)
    win.bind('<Configure>',lambda e:w.status_label.configure(wraplength=max(px(300),e.width-px(56))) if e.widget==win else None)
    w.update_controls()
