"""One-Hz load sampling and USB/receiver telemetry; never injects input."""
import time
import queue
import threading
from serial import SerialException
from load_routes import LoadRoutes,CHANNELS
from load_protocol import SyncUnavailable
from metrics import Metrics,wire_values

def sync_error(error,candidate):
    role=('键盘 USB' if candidate['role']=='keyboard' else '接收器 USB') if candidate else '设备'
    if isinstance(error,TimeoutError):return role+' 未响应，正在自动重试'
    if isinstance(error,(OSError,SerialException)):
        return role+' 连接暂不可用，正在自动重试'
    return '旋钮同步暂未完成，正在自动重试'

class ChannelSelection:
    """Shared by Tk and the sender; revision rejects stale in-flight results."""
    def __init__(self):self.lock=threading.Lock();self.channel='auto';self.revision=0
    def get(self):
        with self.lock:return self.channel,self.revision
    def set(self,channel):
        if channel not in CHANNELS:raise ValueError('Unknown load channel')
        with self.lock:
            self.channel=channel;self.revision+=1

def forward(values,lock,routes,channel='auto'):
    payload=wire_values(values)
    if payload is None:return '等待有效 CPU / 内存数据'
    if not lock.acquire(blocking=False):return '正在配置设备，旋钮同步暂缓'
    try:
        return routes.send(payload) if channel=='auto' else routes.send(payload,channel)
    except SyncUnavailable as error:return str(error)
    except Exception as error:
        message=sync_error(error,None)
        return message if channel=='auto' else CHANNELS[channel]+' · '+message
    finally:lock.release()

def run(stop,updates,lock,enabled,selection=None):
    selection=selection or ChannelSelection()
    sampler=None;sender=None;latest=queue.Queue(maxsize=1)
    status_lock=threading.Lock();last_status=['正在确认键盘连接…',time.monotonic(),-1]
    def send_latest():
        routes=None
        try:
            routes=LoadRoutes()
            while not stop.is_set():
                try:values=latest.get(timeout=.1)
                except queue.Empty:continue
                if stop.is_set():break
                channel,revision=selection.get()
                if enabled.is_set():status=forward(values,lock,routes,channel)
                else:routes.ble.disconnect();status='旋钮同步已暂停；电脑负载继续监视'
                with status_lock:last_status[:]=[status,time.monotonic(),revision]
        finally:
            if routes:routes.close()
    try:
        sampler=Metrics()
        sender=threading.Thread(target=send_latest,daemon=True);sender.start()
        next_sample=time.monotonic()+1
        while not stop.wait(max(0,next_sample-time.monotonic())):
            try:
                values=sampler.sample()
                try:latest.get_nowait()
                except queue.Empty:pass
                latest.put_nowait(values)
                channel,revision=selection.get()
                with status_lock:status,confirmed_at,status_revision=last_status
                if not enabled.is_set():status='旋钮同步已暂停；电脑负载继续监视'
                elif status_revision!=revision or time.monotonic()-confirmed_at>3:status='正在连接 · '+CHANNELS[channel]
                updates.put({**values,'status':status,'channel_revision':revision})
            except Exception:updates.put({'cpu':None,'gpu':None,'ram':None,'status':'暂时无法读取电脑负载，正在自动重试'})
            # Sampling stays at 1 Hz even when GATT or receiver confirmation stalls.
            next_sample=max(next_sample+1,time.monotonic()+.01)
    except Exception:updates.put({'cpu':None,'gpu':None,'ram':None,'status':'电脑负载监视未能启动，请重新打开上位机'})
    finally:
        if sampler:sampler.close()
        stop.set()
        if sender:sender.join(timeout=9)
