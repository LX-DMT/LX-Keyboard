"""Device configuration and global load synchronization."""
import tkinter as tk
from tkinter import ttk
from pathlib import Path
from PIL import Image,ImageTk
from ui_widgets import BG, WHITE, INK, MUTED, GREEN, SOFT, LINE, Surface, Button, SlimScrollbar, font, icon_image, rounded_image

def theme(root):
    root.configure(bg=BG)
    style=ttk.Style(root);style.theme_use('clam')
    style.configure('.',font=font(root,12),background=BG,foreground=INK)
    style.configure('TFrame',background=BG)
    style.configure('TLabel',background=BG)
    style.configure('Muted.TLabel',foreground=MUTED)
    style.configure('Title.TLabel',font=font(root,25,True))
    style.configure('Section.TLabel',font=font(root,14,True))
    style.configure('TButton',padding=(15,10),background='white',bordercolor=LINE,lightcolor=LINE,darkcolor=LINE,focuscolor=GREEN)
    style.map('TButton',background=[('active',SOFT)])
    style.configure('TEntry',fieldbackground='white',padding=7,bordercolor=LINE)
    style.configure('TCombobox',fieldbackground='white',padding=7,bordercolor=LINE,arrowsize=14)
    style.map('TCombobox',fieldbackground=[('readonly','white')],selectbackground=[('readonly',SOFT)],selectforeground=[('readonly',INK)])

def label(parent,text='',*,variable=None,muted=False,size=12,bold=False,wrap=0,color=None):
    kw={'textvariable':variable} if variable is not None else {'text':text}
    return tk.Label(parent,**kw,bg=parent.cget('background'),fg=color or (MUTED if muted else INK),font=font(parent,size,bold),wraplength=wrap,justify='left',anchor='w',bd=0,padx=0,pady=0)

def frame(parent,**kw):return tk.Frame(parent,bg=parent.cget('background'),**kw)

def icon(parent,name,size=22,color=GREEN):
    image=icon_image(parent,name,size,color)
    w=tk.Label(parent,image=image,bg=parent.cget('background'),bd=0);w.image=image
    return w

def section_header(parent,title,glyph):
    """Shared title geometry keeps all device cards aligned across tabs."""
    s=parent.winfo_fpixels('1i')/96
    row=frame(parent,height=round(32*s));row.pack(fill='x');row.pack_propagate(False)
    icon(row,glyph,27).pack(side='left',padx=(0,round(12*s)))
    label(row,title,size=18,bold=True).pack(side='left')
    return row

def build(app):
    root=app.root;theme(root);px=lambda n:round(n*app.scale)
    root.geometry(f'{min(px(740),root.winfo_screenwidth()-60)}x{min(px(540),root.winfo_screenheight()-90)}')
    root.minsize(px(720),px(540))
    header=tk.Frame(root,bg=BG);header.pack(fill='x')
    brand=frame(header);brand.pack(side='left',padx=px(30),pady=px(16))
    mark=tk.Canvas(brand,bg=BG,width=px(32),height=px(32),highlightthickness=0)
    mark.pack(side='left')
    with Image.open(Path(__file__).with_name('assets')/'lx.png') as logo:
        mark.image=ImageTk.PhotoImage(logo.resize((px(32),px(32)),Image.Resampling.LANCZOS),master=mark)
    mark.create_image(0,0,image=mark.image,anchor='nw')
    label(brand,'蓝星键盘',size=18,bold=True).pack(side='left',padx=(px(12),px(10)))
    label(brand,'配置版',muted=True,size=11).pack(side='left')
    from window_chrome import WindowChrome
    app.window_chrome=WindowChrome(app,header,brand)
    app.startup_button=Button(header,text='开机自启动',kind='toggle',width=148,height=34,command=app.toggle_startup)
    app.startup_button.pack(side='right',padx=(px(12),px(18)),pady=px(16))
    tk.Frame(root,bg=BG,height=1).pack(fill='x')
    # This is a global utility, above and outside the device-page navigation.
    app.monitor_card=Surface(root,radius=16,padding=(26,12))
    app.monitor_card.pack(fill='x',padx=px(30),pady=(px(12),px(10)))
    monitor=app.monitor_card.body
    top=frame(monitor);top.pack(fill='x',pady=(0,px(8)))
    icon(top,'pc',20).pack(side='left',padx=(0,px(9)))
    label(top,'旋钮同步',size=15,bold=True).pack(side='left')
    app.load_button=Button(top,text='暂停同步',height=30,command=app.toggle_load)
    app.load_button.pack(side='right')
    values=frame(top);values.pack(side='left',padx=(px(24),0))
    app.load_values={}
    for name,title in (('cpu','CPU'),('gpu','GPU'),('ram','内存')):
        cell=frame(values);cell.pack(side='left',padx=(0,px(20)))
        label(cell,title,muted=True,size=11).pack(side='left',padx=(0,px(10)))
        var=tk.StringVar(value='—');app.load_values[name]=var
        value_label=label(cell,variable=var,size=16,bold=True)
        value_label.configure(width=4,anchor='e');value_label.pack(side='left')
    from dropdown import Dropdown
    from load_routes import CHANNELS
    channel_row=frame(monitor,height=px(34));channel_row.pack(fill='x');channel_row.pack_propagate(False)
    label(channel_row,'同步通道',muted=True,size=11).pack(side='left',padx=(0,px(12)))
    box=Surface(channel_row,radius=9,padding=(8,3));box.pack(side='left')
    style=ttk.Style(root)
    root.load_arrow=icon_image(root,'down',16,MUTED)
    style.element_create('LXLoad.downarrow','image',root.load_arrow,sticky='e')
    style.layout('Load.TCombobox',[('Combobox.padding',{'sticky':'nswe','children':[('LXLoad.downarrow',{'side':'right','sticky':'ns'}),('Combobox.textarea',{'sticky':'nswe'})]})])
    style.configure('Load.TCombobox',padding=5,borderwidth=0,background=WHITE,fieldbackground=WHITE,foreground=INK)
    style.map('Load.TCombobox',foreground=[('!disabled',INK)],fieldbackground=[('!disabled',WHITE)],background=[('!disabled',WHITE)],selectbackground=[('!disabled',SOFT)],selectforeground=[('!disabled',INK)])
    app.load_channel=tk.StringVar(value=CHANNELS['auto'])
    app.load_channel_box=Dropdown(box.body,textvariable=app.load_channel,values=list(CHANNELS.values()),state='readonly',style='Load.TCombobox',width=30,
        option_label=lambda value: CHANNELS['auto'] if value.startswith(CHANNELS['auto']) else value)
    app.load_channel_box.field_surface=box
    app.load_channel_box.pack(fill='x')
    app.load_channel_box.bind('<<ComboboxSelected>>',app.choose_load_channel)
    app.load_status=tk.StringVar()
    # Status occupies fixed space beside the route, never a new layout row.
    status_host=frame(channel_row);status_host.pack(side='left',fill='both',expand=True,padx=(px(16),0))
    app.load_status_display=tk.StringVar()
    app.load_status_label=label(status_host,variable=app.load_status_display,muted=True,size=10)
    app.load_status_label.place(x=0,rely=.5,anchor='w',relwidth=1)
    from tkinter.font import Font
    status_font=Font(font=app.load_status_label.cget('font'))
    tooltip={'window':None,'after':None}
    def hide_status_tip(event=None):
        if tooltip['after']:
            root.after_cancel(tooltip['after']);tooltip['after']=None
        if tooltip['window']:
            tooltip['window'].destroy();tooltip['window']=None
    def fit_status(*_):
        hide_status_tip();width=max(1,status_host.winfo_width());text=app.load_status.get()
        lines=['']
        for char in text:
            if char=='\n' or (lines[-1] and status_font.measure(lines[-1]+char)>width):
                if len(lines)==2:
                    while lines[-1] and status_font.measure(lines[-1]+'…')>width:lines[-1]=lines[-1][:-1]
                    lines[-1]+='…';break
                lines.append('')
            if char!='\n':lines[-1]+=char
        app.load_status_display.set('\n'.join(lines))
    def show_status_tip():
        tooltip['after']=None
        if not app.load_status.get() or app.closing:return
        tip=tk.Toplevel(root);tooltip['window']=tip;tip.overrideredirect(True)
        tip.configure(bg=LINE)
        tk.Label(tip,text=app.load_status.get(),font=font(root,10),bg=WHITE,fg=INK,
                 wraplength=px(320),justify='left',padx=px(10),pady=px(7)).pack(padx=1,pady=1)
        tip.update_idletasks()
        x=min(app.load_status_label.winfo_rootx(),root.winfo_screenwidth()-tip.winfo_reqwidth()-8)
        y=app.load_status_label.winfo_rooty()+app.load_status_label.winfo_height()+px(4)
        if y+tip.winfo_reqheight()>root.winfo_screenheight():y=app.load_status_label.winfo_rooty()-tip.winfo_reqheight()-px(4)
        tip.geometry(f'+{max(0,x)}+{max(0,y)}')
    def enter_status(event=None):
        hide_status_tip();tooltip['after']=root.after(450,show_status_tip)
    app.load_status.trace_add('write',fit_status)
    status_host.bind('<Configure>',fit_status)
    app.load_status_label.bind('<Enter>',enter_status)
    app.load_status_label.bind('<Leave>',hide_status_tip)
    app.load_status_label.bind('<Unmap>',hide_status_tip)
    app.config_navigation=frame(root)
    app.config_navigation.pack(fill='x',padx=px(30),pady=(px(4),0))
    config_heading=frame(app.config_navigation);config_heading.pack(side='left')
    label(config_heading,'设备配置',size=13,bold=True).pack(anchor='w')
    label(config_heading,'键盘需切至 USB 模式并接线',size=10,muted=True).pack(anchor='w',pady=(px(2),0))
    tabs=frame(app.config_navigation);tabs.pack(side='right')
    app.nav_buttons={}
    for name,title,glyph in [('network','配网','wifi'),('voice','小智','mic'),('keys','按键','keyboard'),('lighting','灯光','sun')]:
        button=Button(tabs,text=title,icon=glyph,kind='nav',height=38,width=104,command=lambda p=name:app.navigate(p))
        button.pack(side='left',padx=(px(6),0));app.nav_buttons[name]=button
    viewport=frame(root);viewport.pack(fill='both',expand=True)
    app.canvas=tk.Canvas(viewport,bg=BG,highlightthickness=0)
    scrollbar=SlimScrollbar(viewport,command=app.canvas.yview);app.scrollbar=scrollbar
    app.canvas.pack(side='left',fill='both',expand=True)
    def scroll_set(first,last):
        scrollbar.set(first,last)
        if float(first)<=0 and float(last)>=1:scrollbar.place_forget()
        elif not scrollbar.winfo_manager():
            scrollbar.place(relx=1,x=-px(4),y=px(6),anchor='ne',relheight=1,height=-px(12))
            scrollbar.tk.call('raise',scrollbar._w)
    app.canvas.configure(yscrollcommand=scroll_set)
    app.content=tk.Frame(app.canvas,bg=BG,padx=px(30),pady=px(10))
    window=app.canvas.create_window(0,0,window=app.content,anchor='nw')
    app.canvas.bind('<Configure>',lambda e:app.canvas.itemconfigure(window,width=e.width))
    app.content.bind('<Configure>',lambda e:app.canvas.configure(scrollregion=app.canvas.bbox('all')))
    def wheel(event):
        if event.widget.winfo_toplevel()==root and app.content.winfo_height()>app.canvas.winfo_height():app.canvas.yview_scroll(-int(event.delta/120),'units')
    root.bind_all('<MouseWheel>',wheel,add='+')
    app.pages={name:frame(app.content) for name in app.nav_buttons}
    app.config_status=tk.StringVar()
    def status_note(page):
        note=label(page,variable=app.config_status,muted=True,size=11,wrap=px(610))
        def update(*_):
            if app.config_status.get():note.pack(anchor='w',pady=(px(16),0))
            else:note.pack_forget()
        app.config_status.trace_add('write',update)
    page=app.pages['network']
    card=Surface(page,radius=16,padding=(26,14));card.pack(fill='x')
    section_header(card.body,'Wi-Fi 与接收器','wifi')
    label(card.body,'键盘联网用于小智与语音识别。\n添加接收器后，可使用接收器 Wi-Fi 或 ESP-NOW 输入。',muted=True,wrap=px(580)).pack(anchor='w',pady=(px(16),px(22)))
    steps=frame(card.body);steps.pack(fill='x',pady=(0,px(24)))
    for index,title in enumerate(('连接 USB','设置网络','保存配对')):
        if index:label(steps,'—',muted=True,size=11).pack(side='left',padx=px(20))
        label(steps,f'0{index+1}  {title}',size=12,color=GREEN).pack(side='left')
    app.network_button=Button(card.body,text='开始配网',icon='plus',kind='primary',height=32,command=app.setup)
    app.network_button.pack(anchor='w')
    status_note(page)
    page=app.pages['voice']
    card=Surface(page,radius=16,padding=(26,14));card.pack(fill='x')
    section_header(card.body,'官方激活与绑定','mic')
    label(card.body,'键盘播报六位激活码，对应数字键同步亮起。\n在小智网页填写数字，服务配置自动保存到键盘。',muted=True,wrap=px(580)).pack(anchor='w',pady=(px(16),px(22)))
    app.activation_button=Button(card.body,text='官方激活 / 绑定',kind='primary',height=32,command=app.activation)
    app.activation_button.pack(anchor='w')
    status_note(page)
    app.action_widgets=[app.network_button,app.activation_button]
    from custom_keys import CustomKeysPage
    app.custom_keys_page=CustomKeysPage(app,app.pages['keys'])
    from lighting import LightingPage
    app.lighting_page=LightingPage(app,app.pages['lighting'])
    app.navigate('network')
