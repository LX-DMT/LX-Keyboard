"""Persisted keyboard lighting and policy; product USB configuration only."""
import colorsys
import math
import queue
import tkinter as tk
from device import Device,device_present
from lx_protocol import LIGHTING,LIGHTING_REPLY

def request(device,enabled=None):
    if device.role!='keyboard':raise ValueError('灯光设置只能写入键盘。')
    if enabled is not None and type(enabled) is not bool:raise ValueError('无效的灯光设置。')
    op=0 if enabled is None else 1
    data=bytes((1,op))+(b'' if enabled is None else bytes((enabled,)))
    try:payload=device.exchange(LIGHTING,data,LIGHTING_REPLY).payload
    except TimeoutError as error:
        raise RuntimeError('键盘未响应，请确认已升级支持灯光开关的固件。' if op==0 else '保存结果未确认，请重新读取键盘设置。') from error
    if len(payload)!=5 or payload[:2]!=bytes((1,op)) or payload[4]>1:raise ValueError('灯光设置响应格式不正确。')
    result=int.from_bytes(payload[2:4],'little')
    if result:raise RuntimeError(f'键盘未接受设置（0x{result:04x}），请结束当前配置操作后重试。')
    return bool(payload[4])

def transfer(candidate,enabled=None,*,factory=Device,present=device_present):
    if not candidate or candidate.get('role')!='keyboard':raise ValueError('请连接键盘产品 USB。')
    if enabled is not None and type(enabled) is not bool:raise ValueError('无效的灯光设置。')
    if not present(candidate):raise RuntimeError('目标键盘已断开，请重新读取。')
    device=factory(candidate['port'])
    try:
        current=request(device)
        if enabled is None:return current
        if not present(candidate):raise RuntimeError('目标键盘已改变，请重新读取。')
        if current!=enabled and request(device,enabled)!=enabled:raise RuntimeError('保存结果不一致，请重新读取。')
        confirmed=request(device)
        if confirmed!=enabled or not present(candidate):raise RuntimeError('保存结果未确认，请重新读取。')
        return confirmed
    finally:device.close()

# Wire IDs are stable: removing menu entries must not renumber saved modes.
MODE_LABELS=('常亮','呼吸','律动','旧闪烁效果','旧渐变效果','关闭装饰灯')
AVAILABLE_MODES=(0,1,2,5)
EFFECTS=tuple(MODE_LABELS[mode] for mode in AVAILABLE_MODES)

def validate_effect(value):
    if len(value)!=5 or any(type(x) is not int for x in value):raise ValueError('无效的灯效参数。')
    mode,h,s,v,speed=value
    if not (0<=mode<len(MODE_LABELS) and all(0<=x<=255 for x in (h,s,v)) and 1<=speed<=100):raise ValueError('灯效参数超出范围。')
    return tuple(value)

def effect_request(device,value=None):
    if device.role!='keyboard':raise ValueError('灯光设置只能写入键盘。')
    op=2 if value is None else 3
    data=bytes((1,op))+(b'' if value is None else bytes(validate_effect(value)))
    try:p=device.exchange(LIGHTING,data,LIGHTING_REPLY).payload
    except TimeoutError as error:raise RuntimeError('键盘未响应，请确认已升级支持调色和灯效的固件。') from error
    if len(p)!=9 or p[:2]!=bytes((1,op)):raise ValueError('灯效响应格式不正确，请确认固件已更新。')
    result=int.from_bytes(p[2:4],'little')
    if result:raise RuntimeError(f'键盘未接受灯效设置（0x{result:04x}）。')
    return validate_effect(tuple(p[4:]))

def effect_transfer(candidate,value=None,*,factory=Device,present=device_present):
    if not candidate or candidate.get('role')!='keyboard':raise ValueError('请连接键盘产品 USB。')
    desired=None if value is None else validate_effect(value)
    if not present(candidate):raise RuntimeError('目标键盘已断开，请重新读取。')
    device=factory(candidate['port'])
    try:
        current=effect_request(device)
        if desired is not None:
            if not present(candidate):raise RuntimeError('目标键盘已改变，请重新读取。')
            if current!=desired and effect_request(device,desired)!=desired:raise RuntimeError('灯效保存结果不一致。')
            current=effect_request(device)
            if current!=desired:raise RuntimeError('灯效回读校验失败，请重新读取。')
        if not present(candidate):raise RuntimeError('键盘已断开，结果未确认。')
        return current
    finally:device.close()

class ColorWheel(tk.Canvas):
    SECTORS=24
    RINGS=5
    WHITE_RADIUS=.08

    @classmethod
    def snap(cls,h,s):
        ring=min(cls.RINGS,max(0,math.floor(s/255*cls.RINGS+.5)))
        if ring==0:return 0,0
        sector=math.floor(h/255*cls.SECTORS+.5)%cls.SECTORS
        return round(sector/cls.SECTORS*255),round(ring/cls.RINGS*255)

    @classmethod
    def color_at(cls,dx,dy,radius):
        distance=min(1,math.hypot(dx,dy)/radius)
        if distance<cls.WHITE_RADIUS:return 0,0
        sector=math.floor((math.atan2(-dy,dx)/(2*math.pi))%1*cls.SECTORS+.5)%cls.SECTORS
        ring=min(cls.RINGS-1,int((distance-cls.WHITE_RADIUS)/(1-cls.WHITE_RADIUS)*cls.RINGS))
        return round(sector/cls.SECTORS*255),round((ring+1)/cls.RINGS*255)

    def __init__(self,parent,command):
        from PIL import Image,ImageDraw,ImageTk
        from ui_widgets import scale
        self.size=round(112*scale(parent));self.center=self.size/2;self.radius=self.center-5
        super().__init__(parent,width=self.size,height=self.size,bg=parent.cget('background'),highlightthickness=0)
        self.command=command;self.enabled=False;self.h=0;self.s=255
        # Render the same discrete HSV cells used by hit testing.
        factor=4;n=self.size*factor;im=Image.new('RGBA',(n,n));pixels=im.load();r=self.radius*factor;c=n/2
        for y in range(n):
            for x in range(n):
                dx,dy=x-c,y-c
                if math.hypot(dx,dy)<=r:
                    h,s=self.color_at(dx,dy,r)
                    pixels[x,y]=(*[round(v*255) for v in colorsys.hsv_to_rgb(h/255,s/255,1)],255)
        draw=ImageDraw.Draw(im);line=max(1,round(.65*scale(parent)*factor))
        for i in range(self.SECTORS):
            angle=(i+.5)/self.SECTORS*2*math.pi
            dx,dy=math.cos(angle),-math.sin(angle)
            draw.line((c+dx*r*self.WHITE_RADIUS,c+dy*r*self.WHITE_RADIUS,c+dx*r,c+dy*r),fill='white',width=line)
        for i in range(self.RINGS):
            rr=r*(self.WHITE_RADIUS+i*(1-self.WHITE_RADIUS)/self.RINGS)
            draw.ellipse((c-rr,c-rr,c+rr,c+rr),outline='white',width=line)
        self.image=ImageTk.PhotoImage(im.resize((self.size,self.size),Image.Resampling.LANCZOS),master=self)
        self.create_image(0,0,image=self.image,anchor='nw')
        self.marker_edge=self.create_oval(0,0,0,0,outline='#344d43',width=4)
        self.marker=self.create_oval(0,0,0,0,outline='white',width=2)
        self.bind('<Button-1>',self.pick);self.bind('<B1-Motion>',self.pick)
        self.set(0,255)
    def set(self,h,s):
        self.h=h;self.s=s
        marker_h,marker_s=self.snap(h,s)
        sector=round(marker_h/255*self.SECTORS)%self.SECTORS
        a=sector/self.SECTORS*2*math.pi
        r=0 if marker_s==0 else self.radius*(self.WHITE_RADIUS+(marker_s/255-.5/self.RINGS)*(1-self.WHITE_RADIUS))
        x=self.center+math.cos(a)*r;y=self.center-math.sin(a)*r
        for item in (self.marker_edge,self.marker):self.coords(item,x-5,y-5,x+5,y+5)
    def pick(self,event):
        if not self.enabled:return
        dx,dy=event.x-self.center,event.y-self.center
        h,s=self.color_at(dx,dy,self.radius)
        self.set(h,s);self.command(h,s)

class Slider(tk.Canvas):
    def __init__(self,parent,variable,minimum,command):
        from ui_widgets import scale
        self.factor=scale(parent);self.variable=variable;self.minimum=minimum;self.command=command;self.enabled=False
        super().__init__(parent,height=round(22*self.factor),width=100,bg='white',highlightthickness=0,takefocus=True)
        self.bind('<Configure>',lambda e:self.draw());self.bind('<Button-1>',self.pick);self.bind('<B1-Motion>',self.pick)
        self.bind('<Left>',lambda e:self.step(-1));self.bind('<Right>',lambda e:self.step(1))
        variable.trace_add('write',lambda *_:self.draw())
    def configure(self,cnf=None,**kw):
        if 'state' in kw:self.enabled=kw.pop('state')=='normal';self.draw()
        if cnf or kw:super().configure(cnf,**kw)
    def draw(self):
        from ui_widgets import GREEN
        self.delete('all');r=6*self.factor;x0=r+2;x1=max(x0+1,self.winfo_width()-r-2);y=11*self.factor
        x=x0+(x1-x0)*(self.variable.get()-self.minimum)/(100-self.minimum)
        self.create_line(x0,y,x1,y,fill='#e4eee9',width=4*self.factor,capstyle='round')
        self.create_line(x0,y,x,y,fill=GREEN if self.enabled else '#b7ccc2',width=4*self.factor,capstyle='round')
        self.create_oval(x-r,y-r,x+r,y+r,fill=GREEN if self.enabled else '#b7ccc2',outline='white',width=2)
    def step(self,delta):
        if self.enabled:self.variable.set(max(self.minimum,min(100,self.variable.get()+delta)));self.command()
    def pick(self,event):
        if not self.enabled:return
        self.focus_set();margin=6*self.factor+2
        fraction=max(0,min(1,(event.x-margin)/max(1,self.winfo_width()-2*margin)))
        self.variable.set(round(self.minimum+(100-self.minimum)*fraction));self.command()

class LightingPage:
    def __init__(self,app,page):
        from appearance import label,frame,section_header
        from ui_widgets import Surface,Button,GREEN,LINE
        from dropdown import Dropdown
        self.app=app;self.busy=False;self.loaded=None;self.saved=None;self.effect_saved=None;self.events=queue.Queue()
        self.h=0;self.s=255;self.raw_v=128;self.loading=False;self.protection_value=True
        px=lambda n:round(n*app.scale)
        card=Surface(page,radius=16,padding=(26,14));card.pack(fill='x')
        top=section_header(card.body,'灯光','sun')
        self.save_button=Button(top,text='保存灯效',kind='primary',height=32,command=self.save_effect)
        self.save_button.pack(side='right')
        self.read_button=Button(top,text='读取键盘',height=32,command=self.read);self.read_button.pack(side='right',padx=px(10))
        body=frame(card.body);body.pack(fill='x',pady=(px(10),px(8)))
        palette=frame(body);palette.pack(side='left',padx=(0,px(28)))
        self.wheel=ColorWheel(palette,self.choose_color);self.wheel.pack()
        # One canvas shares the wheel's width: avoid per-item padding/rounding drift.
        self.swatches=tk.Canvas(palette,width=self.wheel.size,height=px(18),bg='white',highlightthickness=0,bd=0)
        self.swatches.pack(anchor='center',pady=(px(2),0));self.swatch_items=[]
        from PIL import Image,ImageDraw,ImageTk
        colors=('#ff8e08','#08ff7c','#0882ff','#9908ff','#ff087c','#ffffff')
        edge=5+px(9);spacing=(self.wheel.size-2*edge)/5
        for i,color in enumerate(colors):
            h,s,_=colorsys.rgb_to_hsv(*[int(color[j:j+2],16)/255 for j in (1,3,5)])
            h,s=self.wheel.snap(round(h*255),round(s*255))
            color='#'+''.join(f'{round(v*255):02x}' for v in colorsys.hsv_to_rgb(h/255,s/255,1))
            pictures=[];size=px(18);factor=4
            for selected in (False,True):
                im=Image.new('RGBA',(size*factor,size*factor));draw=ImageDraw.Draw(im)
                def box(inset):return tuple(round(v*app.scale*factor) for v in (inset,inset,18-inset,18-inset))
                if selected:draw.ellipse(box(.75),outline=GREEN,width=max(1,round(1.5*app.scale*factor)))
                white=color=='#ffffff'
                draw.ellipse(box(3),fill=color,outline='#8f9c96' if white else LINE,
                             width=max(1,round((1.25 if white else .5)*app.scale*factor)))
                pictures.append(ImageTk.PhotoImage(im.resize((size,size),Image.Resampling.LANCZOS),master=self.swatches))
            item=self.swatches.create_image(edge+i*spacing,px(9),image=pictures[0])
            self.swatch_items.append((item,(h,s),pictures))
            self.swatches.tag_bind(item,'<Button-1>',lambda e,c=color:self.swatch(c))
        self.hex_text=tk.StringVar(value='#FF0000')
        label(palette,variable=self.hex_text,size=10,muted=True).pack(anchor='center')
        fields=frame(body);fields.pack(side='left',fill='both',expand=True,pady=(px(17),0))
        fields.columnconfigure(1,weight=1)
        label(fields,'效果',size=11,bold=True).grid(row=0,column=0,sticky='w',padx=(0,px(16)))
        box=Surface(fields,radius=9,padding=(7,2));box.grid(row=0,column=1,columnspan=2,sticky='ew')
        self.effect=tk.StringVar(value=EFFECTS[0])
        self.effect_box=Dropdown(box.body,textvariable=self.effect,values=EFFECTS,state='disabled',style='Load.TCombobox',width=16)
        self.effect_box.field_surface=box;self.effect_box.pack(fill='x')
        self.brightness=tk.IntVar(value=50);self.speed=tk.IntVar(value=60)
        self.sliders=[]
        for index,(title,var,lo,callback) in enumerate((('亮度',self.brightness,0,self.brightness_changed),('速度',self.speed,1,lambda *_:self.update())),1):
            label(fields,title,size=11,bold=True).grid(row=index,column=0,sticky='w',padx=(0,px(16)),pady=(px(12),0))
            value_label=label(fields,variable=var,size=10,muted=True);value_label.configure(width=3,anchor='e')
            value_label.grid(row=index,column=2,sticky='e',padx=(px(8),0),pady=(px(12),0))
            slider=Slider(fields,var,lo,callback)
            slider.grid(row=index,column=1,sticky='ew',pady=(px(12),0));self.sliders.append(slider)
        self.effect.trace_add('write',lambda *_:self.update())
        tk.Frame(card.body,bg=LINE,height=1).pack(fill='x',pady=(0,px(6)))
        row=frame(card.body);row.pack(fill='x')
        label(row,'低电量自动关闭灯光',size=12,bold=True).pack(side='left')
        self.toggle=Button(row,text='',kind='toggle',width=114,height=28,command=self.change)
        self.toggle.configure(on=True,state='disabled');self.toggle.pack(side='right')
        self.status=tk.StringVar()
        self.status_label=label(card.body,variable=self.status,muted=True,size=10,wrap=px(640))
        def show_status(*_):
            if self.status.get():self.status_label.pack(anchor='w',pady=(px(4),0))
            else:self.status_label.pack_forget()
        self.status.trace_add('write',show_status)
        self.update();app.root.after(150,self.poll)

    def candidate(self):
        found=[d for d in self.app.available if d['role']=='keyboard']
        return found[0] if len(found)==1 else None
    def selection(self):
        mode=MODE_LABELS.index(self.effect.get())
        return (mode,self.h,self.s,self.raw_v,self.speed.get())
    def editable(self):return not self.busy and not self.app.working
    def update(self):
        if not hasattr(self,'toggle'):return
        ready=self.editable();mode=MODE_LABELS.index(self.effect.get())
        self.read_button.configure(state='normal' if ready else 'disabled')
        self.toggle.configure(state='normal' if ready else 'disabled')
        self.effect_box.configure(state='readonly' if ready else 'disabled')
        self.wheel.enabled=ready and mode in (0,1,2)
        self.sliders[0].configure(state='normal' if ready and mode in (0,1,2) else 'disabled')
        self.sliders[1].configure(state='normal' if ready and mode==1 else 'disabled')
        self.save_button.configure(state='normal' if ready and mode in AVAILABLE_MODES and (not self.candidate() or self.selection()!=self.effect_saved) else 'disabled')
    def choose_color(self,h,s):
        self.h=h;self.s=s
        for item,(preset_h,preset_s),pictures in self.swatch_items:
            selected=s==preset_s and (s==0 or h==preset_h)
            self.swatches.itemconfigure(item,image=pictures[int(selected)])
        self.hex_text.set('#'+''.join(f'{round(x*255):02X}' for x in colorsys.hsv_to_rgb(h/255,s/255,1)))
        self.update()
    def swatch(self,color):
        if not self.wheel.enabled:return
        h,s,_=colorsys.rgb_to_hsv(*[int(color[i:i+2],16)/255 for i in (1,3,5)])
        h,s=self.wheel.snap(round(h*255),round(s*255))
        self.wheel.set(h,s);self.choose_color(h,s)
    def brightness_changed(self,*_):
        if not self.loading:self.raw_v=round(self.brightness.get()*255/100)
        self.update()
    def read(self):self.run('read')
    def change(self):
        self.run('protection',not self.protection_value)
    def save_effect(self):
        if self.editable() and self.selection()[0] in AVAILABLE_MODES and (not self.candidate() or self.selection()!=self.effect_saved):self.run('effect',self.selection())
    def run(self,kind,value=None):
        if self.busy or self.app.working:return
        candidate=self.app.require_keyboard_usb()
        if not candidate:return
        self.busy=True;self.update();self.status.set('正在读取…' if kind=='read' else '正在保存并校验…')
        def work():
            try:
                result={'device':candidate,'write':kind!='read'}
                if kind in ('read','protection'):result['protection']=transfer(candidate,value if kind=='protection' else None)
                if kind in ('read','effect'):result['effect']=effect_transfer(candidate,value if kind=='effect' else None)
                self.events.put(result)
            except Exception as error:self.events.put({'error':str(error)})
            finally:self.events.put({'done':True})
        self.app.begin_job(work)
    def poll(self):
        while not self.events.empty():
            event=self.events.get_nowait()
            if 'device' in event:
                self.loaded=event['device']
                if 'protection' in event:
                    self.saved=event['protection'];self.protection_value=self.saved;self.toggle.configure(on=self.saved,text='开启' if self.saved else '关闭')
                if 'effect' in event:
                    self.loading=True;self.effect_saved=event['effect']
                    mode,self.h,self.s,self.raw_v,speed=self.effect_saved
                    self.effect.set(MODE_LABELS[mode]);self.brightness.set(round(self.raw_v*100/255));self.speed.set(speed)
                    self.wheel.set(self.h,self.s);self.choose_color(self.h,self.s)
                    self.loading=False
                self.status.set('')
            if 'error' in event:
                self.loaded=None;self.saved=None;self.effect_saved=None;self.status.set(event['error'])
            if event.get('done'):self.busy=False
        if not self.busy and self.loaded is not None and self.loaded!=self.candidate():
            self.loaded=None;self.saved=None;self.effect_saved=None;self.protection_value=True
            self.toggle.configure(on=True,text='');self.status.set('')
        self.update()
        if not self.app.closing:self.app.root.after(150,self.poll)

