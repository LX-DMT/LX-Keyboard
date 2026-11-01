"""Two onboard mappings, configured through keyboard product USB only."""
import queue
import tkinter as tk
from device import Device,device_present
from lx_protocol import CUSTOM_KEYS,CUSTOM_KEYS_REPLY

ACTIONS=((0x46,'PrtSc · 截屏'),(0x47,'Scroll Lock · 滚动锁定'),(0x48,'Pause · 暂停'),
         (0x49,'Insert · 插入'),(0x4a,'Home · 行首'),(0x4b,'Page Up · 上翻页'),
         (0x4c,'Delete · 删除'),(0x4d,'End · 行尾'),(0x4e,'Page Down · 下翻页'),
         (0,'旋钮显示电量'))
LABELS=dict(ACTIONS)
VALUES={label:value for value,label in ACTIONS}
DEFAULTS=(0x4c,0)

def validate(actions):
    if len(actions)!=2 or any(type(v) is not int or v not in LABELS for v in actions):
        raise ValueError('请选择两个有效的按键功能。')
    return tuple(actions)

def request(device,actions=None):
    if device.role!='keyboard':raise ValueError('自定义按键只能写入键盘。')
    op=0 if actions is None else 1
    data=bytes((1,op))+(b'' if actions is None else bytes(validate(actions)))
    try:packet=device.exchange(CUSTOM_KEYS,data,CUSTOM_KEYS_REPLY)
    except TimeoutError as error:
        message='键盘未响应，请检查 USB 连接并确认固件支持自定义按键。' if op==0 else '保存结果未确认，请重新读取键盘后检查。'
        raise RuntimeError(message) from error
    p=packet.payload
    if len(p)!=6 or p[:2]!=bytes((1,op)):raise ValueError('自定义按键响应格式不正确。')
    result=int.from_bytes(p[2:4],'little')
    if result:raise RuntimeError(f'键盘未接受设置（0x{result:04x}），请结束当前配置操作后重试。')
    return validate(tuple(p[4:6]))

def transfer(candidate,actions=None,*,factory=Device,present=device_present):
    if not candidate or candidate.get('role')!='keyboard':raise ValueError('请连接键盘产品 USB。')
    desired=None if actions is None else validate(actions)
    if not present(candidate):raise RuntimeError('目标键盘已断开，请重新读取。')
    device=factory(candidate['port'])
    try:
        current=request(device)
        if desired is None:return current
        if not present(candidate):raise RuntimeError('目标键盘已改变，请重新读取。')
        if current!=desired:
            acknowledged=request(device,desired)
            if acknowledged!=desired:raise RuntimeError('键盘返回的设置不一致，请重新读取。')
        confirmed=request(device)
        if confirmed!=desired:raise RuntimeError('保存后校验不一致，请重新读取。')
        if not present(candidate):raise RuntimeError('保存结果未确认，请重新连接并读取键盘。')
        return confirmed
    finally:device.close()

class KeyPositionDiagram(tk.Canvas):
    """Front view of the six physical keys: custom / Up / custom, Left / Down / Right."""
    def __init__(self,parent):
        from ui_widgets import scale,rounded_image,font,GREEN,SOFT,LINE,MUTED
        s=scale(parent);px=lambda n:round(n*s)
        super().__init__(parent,bg=parent.cget('background'),width=px(144),height=px(86),highlightthickness=0,bd=0)
        self.tiles=[rounded_image(self,px(44),px(40),px(9),SOFT,GREEN),
                    rounded_image(self,px(44),px(40),px(9),'#f7f9f7',LINE)]
        for row,col,text in ((0,0,'1'),(0,1,'↑'),(0,2,'2'),(1,0,'←'),(1,1,'↓'),(1,2,'→')):
            custom=row==0 and col!=1
            x,y=px(col*50),px(row*46)
            self.create_image(x,y,image=self.tiles[0 if custom else 1],anchor='nw')
            self.create_text(x+px(22),y+px(20),text=text,font=font(self,16,custom),fill=GREEN if custom else MUTED)


class CustomKeysPage:
    def __init__(self,app,page):
        from appearance import label,frame,section_header
        from ui_widgets import Surface,Button,SOFT
        from dropdown import Dropdown
        self.app=app;self.busy=False;self.loaded=None;self.saved=None;self.events=queue.Queue()
        px=lambda n:round(n*app.scale)
        card=Surface(page,radius=16,padding=(26,14));card.pack(fill='x')
        section_header(card.body,'自定义按键','keyboard')
        row=frame(card.body);row.pack(fill='x',pady=(px(16),0))
        row.columnconfigure(0,weight=1)
        fields=frame(row);fields.grid(row=0,column=0,sticky='nsew',padx=(0,px(28)))
        fields.columnconfigure(1,weight=1)
        fields.rowconfigure(0,minsize=px(40));fields.rowconfigure(1,minsize=px(6));fields.rowconfigure(2,minsize=px(40))
        diagram=KeyPositionDiagram(row);diagram.grid(row=0,column=1,sticky='ne')
        self.variables=[];self.boxes=[]
        for i,title in enumerate(('1 · 上方向键左侧','2 · 上方向键右侧')):
            label(fields,title,size=12,bold=True).grid(row=i*2,column=0,sticky='w',padx=(0,px(16)))
            field=Surface(fields,radius=10,padding=(8,4));field.grid(row=i*2,column=1,sticky='nsew')
            var=tk.StringVar(value='—');self.variables.append(var)
            box=Dropdown(field.body,textvariable=var,values=[label for _,label in ACTIONS],state='disabled',style='Load.TCombobox',width=18)
            box.field_surface=field;box.pack(fill='x',expand=True);self.boxes.append(box)
            var.trace_add('write',lambda *_:self.update_buttons())
        buttons=frame(card.body);buttons.pack(fill='x',pady=(px(18),0))
        self.read_button=Button(buttons,text='读取键盘',height=32,command=self.read);self.read_button.pack(side='left')
        self.save_button=Button(buttons,text='保存到键盘',kind='primary',height=32,command=self.save);self.save_button.pack(side='left',padx=px(10))
        self.reset_button=Button(buttons,text='恢复默认',height=32,command=self.reset);self.reset_button.pack(side='right')
        self.status=tk.StringVar()
        self.status_label=label(card.body,variable=self.status,muted=True,size=11,wrap=px(640))
        def show_status(*_):
            if self.status.get():self.status_label.pack(anchor='w',pady=(px(12),0))
            else:self.status_label.pack_forget()
        self.status.trace_add('write',show_status)
        self.update_buttons();app.root.after(150,self.poll)

    def candidate(self):
        devices=[d for d in self.app.available if d['role']=='keyboard']
        return devices[0] if len(devices)==1 else None

    def selection(self):
        return tuple(VALUES.get(v.get()) for v in self.variables)

    def update_buttons(self):
        if not hasattr(self,'save_button'):return
        free=not self.busy and not self.app.working
        loaded=self.loaded is not None and self.loaded==self.candidate()
        self.read_button['state']='normal' if free else 'disabled'
        self.reset_button['state']='normal' if free and loaded else 'disabled'
        chosen=self.selection()
        self.save_button['state']='normal' if free and (not loaded or (all(v in LABELS for v in chosen) and chosen!=self.saved)) else 'disabled'
        for box in self.boxes:box.configure(state='readonly' if free and loaded else 'disabled')

    def run(self,actions=None):
        if self.busy or self.app.working:return
        candidate=self.app.require_keyboard_usb()
        if not candidate:return
        if not candidate or (actions is not None and candidate!=self.loaded):
            self.loaded=None;self.saved=None
            self.status.set('请连接同一台键盘的产品 USB，并重新读取。');self.update_buttons();return
        self.busy=True;self.update_buttons()
        self.status.set('正在读取…' if actions is None else '正在保存并校验…')
        def work():
            try:self.events.put({'map':transfer(candidate,actions),'device':candidate,'write':actions is not None})
            except Exception as error:self.events.put({'error':str(error)})
            finally:self.events.put({'done':True})
        self.app.begin_job(work)

    def read(self):self.run()
    def save(self):
        if self.busy or self.app.working:return
        candidate=self.app.require_keyboard_usb()
        if not candidate:return
        if self.loaded!=candidate:
            from notices import show_notice
            show_notice('请先读取键盘','请先点击“读取键盘”，选择按键功能后再保存。',parent=self.app.root,icon='keyboard');return
        chosen=self.selection()
        if self.loaded is not None and chosen!=self.saved and all(v in LABELS for v in chosen):self.run(chosen)
    def reset(self):
        if self.busy or self.app.working or self.loaded is None:return
        for var,action in zip(self.variables,DEFAULTS):var.set(LABELS[action])
        self.status.set('已选回默认功能，点击“保存到键盘”后生效。')

    def poll(self):
        while not self.events.empty():
            event=self.events.get_nowait()
            if 'map' in event:
                self.loaded=event['device'];self.saved=event['map']
                for var,action in zip(self.variables,self.saved):var.set(LABELS[action])
                self.status.set('已保存，重新开机后仍有效。' if event['write'] else '已读取键盘设置。')
            if 'error' in event:
                self.loaded=None;self.saved=None;self.status.set(event['error'])
            if event.get('done'):self.busy=False
        if not self.busy and self.loaded is not None and self.loaded!=self.candidate():
            self.loaded=None;self.saved=None;self.status.set('')
        self.update_buttons()
        if not self.app.closing:self.app.root.after(150,self.poll)
