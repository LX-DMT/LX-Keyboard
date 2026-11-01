"""LX Keyboard Setup: provisioning, Xiaozhi, custom keys, lighting and load monitoring."""
import argparse
import ctypes as C
import json
import queue
import threading
import time
import logging
from pathlib import Path
import tkinter as tk
from device import devices
APP_TITLE='LX Keyboard Setup · 蓝星键盘配置版'

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ui-smoke-output',type=Path,help='UI only; no device commands')
    parser.add_argument('--load-sync-check',type=Path,help='Three real telemetry samples; no typing or configuration')
    parser.add_argument('--load-channel',choices=('auto','usb','ble','wifi','now'),default='auto',help='Channel for diagnostic load check only')
    parser.add_argument('--startup',action='store_true',help='Launch at Windows user logon')
    args=parser.parse_args()
    logging.getLogger('lx.lifecycle').info('launch startup=%s',args.startup)
    if args.load_sync_check:
        from load_routes import LoadRoutes
        from metrics import Metrics,wire_values
        routes=LoadRoutes();sampler=None;report={'samples':[],'errors':[]}
        try:
            sampler=Metrics()
            for _ in range(3):
                time.sleep(1);values=sampler.sample();start=time.monotonic()
                status=routes.send(wire_values(values),args.load_channel)
                report['samples'].append({'values':values,'status':status,'elapsed_ms':round((time.monotonic()-start)*1000)})
        except Exception as error:report['errors'].append(type(error).__name__+': '+str(error))
        finally:
            routes.close()
            if sampler:sampler.close()
        args.load_sync_check.parent.mkdir(parents=True,exist_ok=True)
        args.load_sync_check.write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf8')
        return
    user32=C.WinDLL('user32')
    try:
        user32.SetProcessDpiAwarenessContext.argtypes=[C.c_void_p]
        user32.SetProcessDpiAwarenessContext(C.c_void_p(-4))
    except AttributeError:user32.SetProcessDPIAware()
    instance=None
    try:
        if not args.ui_smoke_output:
            from startup import SingleInstance
            instance=SingleInstance()
            if instance.existing:
                logging.getLogger('lx.lifecycle').info('existing instance; restore=%s',not args.startup)
                if not args.startup:instance.show_existing(APP_TITLE)
                return
        App(args).run()
    finally:
        if instance:instance.close()

class App:
    def __init__(self,args):
        self.args=args;self.root=tk.Tk()
        self.start_hidden=bool(getattr(args,'startup',False) and not args.ui_smoke_output)
        if self.start_hidden:self.root.withdraw()
        self.scale=self.root.winfo_fpixels('1i')/96
        self.root.title(APP_TITLE)
        from tray import ICON_PATH
        if ICON_PATH.is_file():self.root.iconbitmap(str(ICON_PATH))
        self.tray=None;self.hidden_children=[]
        self.jobs=queue.Queue();self.work_thread=None;self.working=False;self.closing=False
        self.device_lock=threading.Lock();self.load_events=queue.Queue();self.load_thread=None
        self.load_stop=threading.Event();self.load_enabled=threading.Event();self.load_enabled.set()
        from load_monitor import ChannelSelection
        self.load_selection=ChannelSelection()
        self.available=[];self.next_refresh=0
        from appearance import build
        build(self);self.refresh();self.refresh_startup()
        from window_icons import WindowIcons
        self.window_icons=WindowIcons(self.root)
        self.root.protocol('WM_DELETE_WINDOW',self.hide_to_tray);self.root.after(100,self.poll)

    @staticmethod
    def alive(thread):return thread is not None and thread.is_alive()

    def refresh_startup(self):
        from startup import is_enabled
        try:
            self.startup_enabled=is_enabled()
            self.startup_button.configure(on=self.startup_enabled)
        except OSError:
            self.startup_enabled=None
            self.startup_button.configure(on=False)

    def toggle_startup(self):
        from startup import is_enabled,set_enabled
        from tkinter import messagebox
        try:
            enabled=not is_enabled()
            set_enabled(enabled)
            if is_enabled()!=enabled:raise OSError('Startup setting verification failed')
        except OSError:
            self.refresh_startup()
            messagebox.showerror('开机自启动','未能修改开机自启动设置，请稍后重试。',parent=self.root)
            return
        self.refresh_startup()

    def hide_to_tray(self):
        if self.closing or self.root.state()=='withdrawn':return
        if not self.tray or not self.tray.available:
            self.close();return
        self.hidden_children=[]
        for name in ('setup_window','activation_window'):
            dialog=getattr(self,name,None)
            if dialog and not dialog.closed and dialog.window.winfo_exists():
                self.hidden_children.append((dialog.window,dialog.window.grab_current() is dialog.window))
                if dialog.window.grab_current() is dialog.window:dialog.window.grab_release()
                dialog.window.withdraw()
        self.root.withdraw()

    def show_window(self):
        if self.closing:return
        self.root.deiconify();self.root.lift()
        for window,grabbed in self.hidden_children:
            if window.winfo_exists():
                window.deiconify();window.lift()
                if grabbed:window.grab_set()
        self.hidden_children=[]

    def start_tray(self):
        from tray import Tray
        self.tray=Tray(lambda action:self.jobs.put({'tray_action':action}))
        return self.tray.start()

    def navigate(self,page):
        for name,frame in self.pages.items():
            frame.pack_forget();self.nav_buttons[name].configure(selected=name==page)
        self.pages[page].pack(fill='both',expand=True);self.current_page=page
        self.root.update_idletasks();self.canvas.yview_moveto(0)

    def refresh(self):
        self.available=devices()

    def candidate(self):
        self.refresh()
        keyboards=[d for d in self.available if d['role']=='keyboard']
        return keyboards[0] if len(keyboards)==1 else None

    def require_keyboard_usb(self,parent=None,expected=None):
        from notices import show_notice
        self.refresh()
        keyboards=[d for d in self.available if d['role']=='keyboard']
        if expected is not None:
            if any(all(d.get(k)==expected.get(k) for k in ('port','role','serial')) for d in keyboards):return expected
        elif len(keyboards)==1:return keyboards[0]
        if not keyboards:
            show_notice('请连接键盘 USB','请先将键盘切换到 USB 模式并连接电脑。',parent=parent or self.root,compact=True)
        elif expected is not None:
            show_notice('键盘连接已变化','请重新选择或读取要配置的键盘后再操作。',parent=parent or self.root,icon='keyboard')
        else:
            show_notice('请选择一台键盘','检测到多台键盘，请只连接一台要配置的键盘后重试。',parent=parent or self.root,icon='keyboard')
        return None

    def begin_job(self,task):
        if self.working:return
        self.working=True
        for widget in self.action_widgets:widget['state']='disabled'
        def run():
            try:
                with self.device_lock:task()
            finally:self.jobs.put({'done':True})
        self.work_thread=threading.Thread(target=run,daemon=True);self.work_thread.start()

    def start_monitor(self):
        if self.alive(self.load_thread):return
        from load_monitor import run
        self.load_stop.clear()
        self.load_thread=threading.Thread(target=run,args=(self.load_stop,self.load_events,self.device_lock,self.load_enabled,self.load_selection),daemon=True)
        self.load_thread.start()

    def toggle_load(self):
        if self.load_enabled.is_set():self.load_enabled.clear()
        else:self.load_enabled.set()
        self.load_button.configure(text='暂停同步' if self.load_enabled.is_set() else '开启同步')
        self.show_load_status('正在确认键盘连接…' if self.load_enabled.is_set() else '旋钮同步已暂停；电脑负载继续监视')

    def show_load_status(self,status,note=''):
        from load_routes import CHANNELS
        channel,_=self.load_selection.get()
        if not self.load_enabled.is_set():status='旋钮同步已暂停；电脑负载继续监视'
        confirmed=status.startswith('键盘已接收 · ')
        title=CHANNELS[channel]
        if confirmed and channel=='auto':title+=' · '+status.split(' · ',1)[1]
        choices=list(CHANNELS.values())
        if channel=='auto':choices[0]=title
        if tuple(self.load_channel_box['values'])!=tuple(choices):self.load_channel_box.configure(values=choices)
        self.load_channel.set(title)
        message=note.lstrip(' ·') if confirmed else status+note
        if self.load_status.get()!=message:self.load_status.set(message)

    def choose_load_channel(self,event=None):
        from load_routes import CHANNELS
        selected=self.load_channel.get()
        channel='auto' if selected.startswith(CHANNELS['auto']) else next(key for key,label in CHANNELS.items() if label==selected)
        self.load_selection.set(channel)
        self.show_load_status('正在连接 · '+CHANNELS[channel] if self.load_enabled.is_set() else '旋钮同步已暂停；电脑负载继续监视')

    def setup(self):
        if self.working:return
        from setup_wizard import SetupWizard
        current=getattr(self,'setup_window',None)
        if current and not current.closed:current.window.lift();return
        self.refresh()
        if not any(d['role']=='keyboard' for d in self.available):
            self.require_keyboard_usb();return
        SetupWizard(self)

    def activation(self):
        if self.working:return
        current=getattr(self,'activation_window',None)
        if current and not current.closed:current.window.lift();return
        candidate=self.require_keyboard_usb()
        if not candidate:return
        from activation import ActivationDialog
        self.activation_window=ActivationDialog(self,candidate)

    def poll(self):
        while not self.load_events.empty():
            event=self.load_events.get_nowait()
            for key,var in self.load_values.items():
                value=event.get(key);var.set('—' if value is None else f'{value}%')
            note=' · GPU 未取得数据，旋钮暂显示 0' if event.get('gpu') is None else ''
            if event.get('channel_revision',self.load_selection.get()[1])==self.load_selection.get()[1]:
                self.show_load_status(event['status'],note)
        if not self.working and time.monotonic()>=self.next_refresh:
            self.refresh();self.next_refresh=time.monotonic()+1
        while not self.jobs.empty():
            event=self.jobs.get_nowait()
            if event.get('tray_action')=='show':self.show_window()
            elif event.get('tray_action')=='quit':
                self.close()
                if self.closing:return
            if 'config_status' in event:self.config_status.set(event['config_status'])
            if event.get('refresh'):self.next_refresh=0
            if event.get('done'):
                self.working=False
                for widget in self.action_widgets:widget['state']='normal'
        if not self.closing:self.root.after(100,self.poll)

    def close(self):
        wizard=getattr(self,'setup_window',None)
        if wizard and wizard.busy:
            self.show_window();wizard.status.set('正在验证或回退配置，请等待本步结束。');return
        activation=getattr(self,'activation_window',None)
        if activation and not activation.closed:activation.close()
        self.closing=True;self.load_stop.set()
        if self.alive(self.work_thread) or self.alive(self.load_thread):self.root.after(100,self.close)
        else:
            if self.tray:self.tray.stop()
            logging.getLogger('lx.lifecycle').info('exit completed')
            self.root.destroy()

    def run(self):
        ui_output=getattr(self.args,'ui_smoke_output',None)
        if ui_output:
            errors=[];visited=[]
            self.root.report_callback_exception=lambda kind,value,tb:errors.append(str(value))
            def visit(page):
                self.navigate(page);visited.append(page)
                from PIL import ImageGrab
                ui_output.parent.mkdir(parents=True,exist_ok=True);self.root.update_idletasks()
                ImageGrab.grab(window=self.root.winfo_id()).save(ui_output.parent/f'packaged-{page}.png')
            def finish_ui():
                import sys
                forbidden=[name for name in ('wifi_server','input_win','identity','ble_control','mode_control') if name in sys.modules]
                ui_output.write_text(json.dumps({'pages':visited,'errors':errors,'forbidden_modules':forbidden,
                    'scope':'UI only; no device commands or input','passed':not errors and not forbidden and len(visited)==4},ensure_ascii=False,indent=2),encoding='utf8')
                self.close()
            for i,page in enumerate(('network','voice','keys','lighting')):self.root.after(350*(i+1),lambda p=page:visit(p))
            def check_selector():
                from load_routes import CHANNELS
                for key,label in CHANNELS.items():
                    self.load_channel.set(label);self.load_channel_box.event_generate('<<ComboboxSelected>>')
                    assert self.load_selection.get()[0]==key
                self.load_channel.set(CHANNELS['auto']);self.choose_load_channel()
                self.show_load_status('键盘已接收 · 接收器 ESP-NOW')
                assert self.load_channel.get()=='自动选择 · 接收器 ESP-NOW'
                assert not self.load_status.get()
                self.choose_load_channel() # Decorated automatic option remains selectable.
                assert self.load_selection.get()[0]=='auto'
                self.show_load_status('设备未响应，正在自动重试')
                assert self.load_channel.get()=='自动选择' and self.load_status.get()
                self.show_load_status('键盘已接收 · 接收器 ESP-NOW')
                for key,value in (('cpu','25%'),('gpu','17%'),('ram','51%')):self.load_values[key].set(value)
                self.root.update_idletasks()
                self.root.after(100,self.load_channel_box.open_menu)
            def capture_selector():
                from PIL import ImageGrab
                ImageGrab.grab(window=self.root.winfo_id()).save(ui_output.parent/'packaged-channels.png')
                assert self.load_channel_box.popup is not None
                ImageGrab.grab(window=self.load_channel_box.popup.winfo_id()).save(ui_output.parent/'packaged-channel-menu.png')
                self.load_channel_box.close_menu()
            self.root.after(1700,check_selector)
            self.root.after(2100,capture_selector)
            self.root.after(2500,finish_ui)
        else:
            tray_ready=self.start_tray()
            if self.start_hidden and not tray_ready:self.show_window()
            self.root.after(300,self.start_monitor)
            logging.getLogger('lx.lifecycle').info('window ready; tray=%s; hidden=%s',tray_ready,self.root.state()=='withdrawn')
        self.root.mainloop()

if __name__=='__main__':
    from startup import setup_log
    log=setup_log()
    try:main()
    except Exception:
        log.exception('startup failed')
        raise
