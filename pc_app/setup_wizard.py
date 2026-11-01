"""USB setup wizard; all I/O is off Tk's UI thread."""
import queue
import threading
import time
import tkinter as tk
from device import Device
from setup_device import Setup,inspect_devices,provision,activate_receiver

class SetupWizard:
    def __init__(self,app):
        self.app=app;app.setup_window=self;self.events=queue.Queue();self.busy=False;self.found=[];self.closed=False
        self.discovery_signature=None;self.next_presence_check=0.;self.discovery_retries=0;self.retry_discovery=False
        self.window=tk.Toplevel(app.root);self.window.title('添加新设备 / 更新网络');self.window.transient(app.root);self.window.grab_set()
        self.step=0
        self.saved=False;self.validation_started=False
        from setup_view import build
        build(self)
        self.show_step()
        self.window.update_idletasks()
        self.window.geometry(f'+{max(0,(self.window.winfo_screenwidth()-self.window.winfo_width())//2)}+{max(0,(self.window.winfo_screenheight()-self.window.winfo_height())//2-30)}')
        self.window.protocol('WM_DELETE_WINDOW',self.close)
        self.window.bind('<Escape>',lambda e:self.close())
        self.window.after(100,self.poll);self.discover()

    def show_step(self):
        from ui_widgets import GREEN,MUTED
        for page in self.pages:page.pack_forget()
        self.pages[self.step].pack(fill='both',expand=True)
        for i,text in enumerate(self.step_labels):text.configure(fg=GREEN if i==self.step else MUTED)
        self.back['text']='取消' if self.step==0 else '上一步'
        self.next.pack_forget();self.go.pack_forget();self.log_card.pack_forget()
        if self.step==2:
            keyboard=self.selected(self.keyboard,'keyboard');receiver=self.selected(self.receiver,'receiver')
            network=self.ssid.get()
            self.summary.set('键盘：'+(keyboard['label'] if keyboard else '未选择')+'\n\n接收器：'+(receiver['label'] if receiver else '本次不添加')+'\n\n网络：'+network)
            self.go.pack(side='right')
            if self.validation_started:self.log_card.pack(fill='x')
        else:self.next.pack(side='right')
        self.update_controls();self.window.update_idletasks();self.canvas.yview_moveto(0)

    def update_controls(self):
        state='disabled' if self.busy else 'normal'
        self.sbox.configure(state=state);self.password_entry.configure(state=state)
        self.scan_button['state']=state
        self.kbox.configure(state='disabled' if self.busy else 'readonly')
        self.rbox.configure(state='disabled' if self.busy else 'readonly')
        self.go['state']='disabled' if self.busy or self.saved else 'normal'
        self.go['text']='已保存' if self.saved else '验证并保存'

    def previous(self):
        if self.busy:return
        if self.step==0:self.close()
        else:
            self.saved=False;self.validation_started=False;self.step-=1;self.show_step()

    def advance(self):
        if self.busy:return
        if self.step==0 and not self.selected(self.keyboard,'keyboard'):
            self.status.set('请先选择要配置的键盘。');return
        if self.step==1:
            from device import config_payload
            try:
                if not self.ssid.get():raise ValueError('请选择或填写 Wi-Fi 名称。')
                config_payload('ssid',self.ssid.get());config_payload('pass',self.password.get())
            except ValueError as error:self.status.set(str(error));return
        self.step=min(2,self.step+1);self.show_step()
        self.status.set('请确认网络设置。' if self.step==1 else '确认后验证网络并保存设备配对。')

    def task(self,action):
        if self.busy:return
        self.busy=True
        for widget in (self.go,self.refresh,self.scan_button,self.back,self.next):widget['state']='disabled'
        self.update_controls()
        def run():
            try:
                with self.app.device_lock:action()
            except Exception as error:self.events.put(('status',str(error)))
            finally:self.events.put(('done',None))
        threading.Thread(target=run,daemon=True).start()
    @staticmethod
    def signature(candidates):
        return tuple(sorted((x['port'],x['role'],x.get('serial','')) for x in candidates))
    def discover(self,automatic=False):
        if self.busy:return
        if not automatic:self.discovery_retries=0
        self.retry_discovery=False;self.next_presence_check=time.monotonic()+1
        self.status.set('正在读取产品 USB 设备…')
        def work():self.events.put(('devices',inspect_devices()))
        self.task(work)
    def selected(self,variable,role):
        return next((x for x in self.found if x.get('label')==variable.get() and x['role']==role),None)
    def scan(self):
        if self.busy:return
        candidate=self.selected(self.keyboard,'keyboard')
        if not candidate:self.status.set('请先选择支持配网的键盘。');return
        if not self.app.require_keyboard_usb(parent=self.window,expected=candidate):return
        def work():
            device=Device(candidate['port']);session=Setup(device)
            try:
                if session.info()['mac']!=candidate['mac']:raise RuntimeError('键盘身份已变化，请刷新')
                session.begin();self.events.put(('scan',session.scan()))
            finally:
                try:session.abort()
                finally:device.close()
        self.task(work)
    def save(self):
        if self.busy or self.saved:return
        keyboard=self.selected(self.keyboard,'keyboard');receiver=self.selected(self.receiver,'receiver')
        ssid=self.ssid.get();password=self.password.get()
        if not keyboard:self.status.set('请选择键盘。');return
        if not self.app.require_keyboard_usb(parent=self.window,expected=keyboard):return
        from device import config_payload
        try:
            if not ssid:raise ValueError('请选择或填写 Wi-Fi 名称。')
            config_payload('ssid',ssid);config_payload('pass',password)
        except ValueError as error:self.status.set(str(error));return
        self.validation_started=True
        self.log.configure(state='normal');self.log.delete('1.0','end');self.log.insert('end','正在开始验证…\n');self.log.configure(state='disabled')
        self.log_card.pack(fill='x')
        def notify(text):self.events.put(('status',text))
        def work():
            provision(keyboard,receiver,ssid,password,notify)
            if receiver:activate_receiver(receiver,notify)
            self.events.put(('success','网络与设备配对已保存，键盘模式保持不变。请在键盘或旋钮上选择需要的模式。'))
        self.task(work)
    def poll(self):
        while not self.events.empty():
            kind,value=self.events.get_nowait()
            if kind=='devices':
                previous={role:self.selected(variable,role) for variable,role in ((self.keyboard,'keyboard'),(self.receiver,'receiver'))}
                self.discovery_signature=self.signature(value);self.next_presence_check=time.monotonic()+1
                self.found=[];failures=[]
                for candidate in value:
                    if 'error' in candidate:
                        failures.append(candidate['port']+'：'+candidate['error'])
                        self.log.configure(state='normal');self.log.insert('end',candidate['port']+'：'+candidate['error']+'\n');self.log.configure(state='disabled');continue
                    candidate['label']=f"{candidate['port']} · {candidate['mac'][-6:].upper()} · {'已配网' if candidate['configured'] else '新设备'}";self.found.append(candidate)
                for box,variable,role in ((self.kbox,self.keyboard,'keyboard'),(self.rbox,self.receiver,'receiver')):
                    options=[x['label'] for x in self.found if x['role']==role]
                    box['values']=options if role=='keyboard' else ['暂不添加接收器']+options
                    old=previous[role]
                    same=next((x['label'] for x in self.found if old and x['role']==role and x.get('serial')==old.get('serial') and x['mac']==old['mac']),None)
                    variable.set(same or (options[0] if role=='keyboard' and len(options)==1 else '' if role=='keyboard' else '暂不添加接收器'))
                counts={role:sum(x['role']==role for x in self.found) for role in ('keyboard','receiver')}
                detail=f"键盘：已识别 {counts['keyboard']} 台；接收器：已识别 {counts['receiver']} 台。"
                if failures:detail+='\n读取失败：'+'\n'.join(failures)
                elif not counts['receiver']:detail+='\n未检测到接收器产品 USB；请检查接收器数据连接。'
                self.discovery_detail.set(detail)
                self.retry_discovery=bool(failures) and self.discovery_retries<2
                self.status.set('部分设备读取失败，请查看上方原因。' if failures else '已识别设备，请继续设置网络。' if counts['keyboard'] else '等待键盘产品 USB。请先切至 USB 模式并连接数据口。')
            elif kind=='scan':self.sbox['values']=list(dict.fromkeys(x['ssid'] for x in value if x['ssid']));self.status.set(f'扫描到 {len(value)} 个 2.4 GHz 网络，也可手动填写隐藏网络。')
            elif kind in ('status','success'):
                self.status.set(value);self.log.configure(state='normal');self.log.insert('end',value+'\n');self.log.see('end');self.log.configure(state='disabled')
                if kind=='success':
                    self.saved=True;self.password.set('');self.app.config_status.set(value);self.update_controls()
            elif kind=='done':
                self.busy=False
                for widget in (self.refresh,self.back,self.next):widget['state']='normal'
                self.update_controls()
        if self.step==0 and not self.busy and time.monotonic()>=self.next_presence_check:
            self.next_presence_check=time.monotonic()+1
            if self.signature(self.app.available)!=self.discovery_signature:
                self.discovery_retries=0;self.discover(automatic=True)
            elif self.retry_discovery:
                self.discovery_retries+=1;self.discover(automatic=True)
        if not self.closed:self.window.after(100,self.poll)
    def close(self):
        if self.busy:self.status.set('正在验证或回退配置，请等待本步结束。');return
        self.closed=True;self.app.setup_window=None;self.password.set('');self.window.destroy()
