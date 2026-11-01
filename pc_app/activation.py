"""Explicit official activation over keyboard product USB. Never stores tokens."""
import queue
import secrets
import struct
import threading
import time
import tkinter as tk
import webbrowser
from device import Device,device_present
from lx_protocol import ACTIVATION,ACTIVATION_REPLY

STATES=('idle','requesting','code','ready','error','cancelled','expired')
def request(device,op,owner):
    if device.role!='keyboard':raise ValueError('官方激活只能连接键盘。')
    try:packet=device.exchange(ACTIVATION,struct.pack('<BBI',1,op,owner),ACTIVATION_REPLY)
    except TimeoutError as error:raise RuntimeError('键盘未响应官方激活指令；请检查 USB 连接及固件是否已升级。') from error
    p=packet.payload
    if len(p)!=18 or p[0]!=1 or p[1]>=len(STATES):raise ValueError('激活响应格式不正确')
    code=p[4:10].rstrip(b'\0').decode('ascii')
    if code and (len(code)!=6 or not code.isascii() or not code.isdigit()):raise ValueError('设备返回的激活码不是六位数字')
    state={'state':STATES[p[1]],'active':bool(p[2]),'digit':p[3], 'code':code,
           'error':int.from_bytes(p[10:12],'little'),'owner':int.from_bytes(p[12:16],'little'),
           'result':int.from_bytes(p[16:18],'little')}
    if state['result']:raise RuntimeError('无法开始配置：请先完成 Wi-Fi 配网，并结束当前语音会话后重试。')
    if owner and state['owner']!=owner:raise RuntimeError('配置会话已改变，请重新开始。')
    return state

def activate(candidate,stop,emit,*,factory=Device,present=device_present,wait=None,clock=time.monotonic):
    device=None;started=False;owner=secrets.randbits(32) or 1
    wait=wait or stop.wait
    try:
        if not present(candidate):raise RuntimeError('目标键盘已断开，请重新连接产品 USB。')
        device=factory(candidate['port'])
        # Cancellation before opening must not start an unintended session.
        if stop.is_set():return
        state=request(device,1,owner);started=True;until=clock()+135
        while True:
            emit(state)
            if state['state'] in ('ready','error','cancelled','expired'):return
            if stop.is_set():return
            if clock()>=until:raise TimeoutError('激活超时，请重新开始。')
            if wait(.5):return
            if not present(candidate):raise RuntimeError('键盘已断开，数字灯提示将在数秒内结束。')
            state=request(device,0,owner)
    finally:
        if device:
            if started:
                try:request(device,2,owner)
                except Exception:pass  # Firmware lease independently expires in 5s.
            device.close()

class ActivationDialog:
    def __init__(self,app,candidate):
        self.app=app;self.candidate=candidate;self.stop=threading.Event();self.events=queue.Queue()
        self.busy=False;self.closing=False;self.closed=False;self.had_code=False
        self.window=tk.Toplevel(app.root);self.window.title('小智官方激活 / 绑定');self.window.transient(app.root)
        from appearance import label,frame
        from ui_widgets import BG,WHITE,SOFT,Surface,Button,font
        px=lambda n:round(n*app.scale)
        self.window.configure(bg=BG)
        form=tk.Frame(self.window,bg=BG,padx=px(28),pady=px(26));form.pack(fill='both',expand=True)
        label(form,'绑定官方小智',size=24,bold=True).pack(anchor='w')
        label(form,'开始配置后，将播报六位激活码。',muted=True).pack(anchor='w',pady=(px(8),px(22)))
        card=Surface(form,radius=16,padding=(22,22));card.pack(fill='x')
        label(card.body,'激活码',size=13,bold=True).pack(anchor='w',pady=(0,px(16)))
        self.code=tk.StringVar(value='— — — — — —')
        digits=frame(card.body);digits.pack(fill='x')
        digit_labels=[]
        for i in range(6):
            digits.columnconfigure(i,weight=1,uniform='digit')
            cell=Surface(digits,fill=SOFT,radius=10,padding=(0,14),border=None)
            cell.grid(row=0,column=i,sticky='ew',padx=(0,px(7) if i<5 else 0))
            text=label(cell.body,'—',size=28,bold=True);text.configure(anchor='center');text.pack(fill='x');digit_labels.append(text)
        def show_code(*args):
            value=self.code.get().split()
            for i,text in enumerate(digit_labels):text.configure(text=value[i] if i<len(value) else '—')
        self.code.trace_add('write',show_code)
        label(card.body,'在小智网页添加设备，填写上方数字。',muted=True,size=11).pack(anchor='w',pady=(px(15),0))
        self.status=tk.StringVar(value='尚未开始配置')
        label(form,variable=self.status,wrap=px(540),size=11,muted=True).pack(fill='x',pady=(px(18),px(22)))
        row=frame(form);row.pack(fill='x')
        self.start_button=Button(row,text='开始配置',kind='primary',command=self.start);self.start_button.pack(side='left')
        Button(row,text='打开小智网页',command=lambda:webbrowser.open('https://xiaozhi.me/console/agents')).pack(side='left',padx=px(10))
        self.cancel_button=Button(row,text='关闭',command=self.close);self.cancel_button.pack(side='right')
        self.window.update_idletasks()
        width=px(620);height=self.window.winfo_reqheight()
        self.window.geometry(f'{width}x{height}+{max(0,(self.window.winfo_screenwidth()-width)//2)}+{max(0,(self.window.winfo_screenheight()-height)//2)}')
        self.window.resizable(False,False)
        self.window.protocol('WM_DELETE_WINDOW',self.close);self.window.bind('<Escape>',lambda e:self.close())
        self.window.after(100,self.poll)
    def start(self):
        if self.busy or self.app.working:return
        self.stop.clear();self.had_code=False;self.busy=True;self.start_button['state']='disabled';self.cancel_button['text']='取消配置'
        self.code.set('— — — — — —');self.status.set('正在开启音频并请求官方配置…')
        def task():
            try:activate(self.candidate,self.stop,self.events.put)
            except Exception as error:self.events.put({'message':str(error)})
            finally:self.events.put({'done':True})
        self.app.begin_job(task)
    def poll(self):
        while not self.events.empty():
            event=self.events.get_nowait()
            if 'state' in event:
                if event['code']:self.had_code=True
                self.code.set(' '.join(event['code']) if event['code'] else '— — — — — —')
                messages={'requesting':'正在准备音频并连接官方服务…','code':'等待网页完成绑定…',
                    'ready':'绑定完成，配置已保存。' if self.had_code else '设备已完成配置，如需更换请先去官网解除绑定',
                    'error':f"配置未完成（0x{event['error']:04x}），请检查网络后重试。",
                    'cancelled':'已取消配置','expired':'配置已超时，请重新开始。'}
                self.status.set(messages.get(event['state'],'尚未开始。'))
            if 'message' in event:self.status.set(event['message'])
            if event.get('done'):
                self.busy=False;self.start_button['state']='normal';self.cancel_button['text']='关闭'
                self.app.config_status.set(self.status.get())
        if self.closing and not self.busy:
            self.closed=True;self.window.destroy();return
        self.window.after(100,self.poll)
    def close(self):
        self.closing=True;self.stop.set()
        if self.busy:self.status.set('正在取消配置并恢复灯效…')
