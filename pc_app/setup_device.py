"""USB-assisted configuration transactions. Never prints or returns passwords."""
import secrets
import socket
import time
from device import Device,devices,config_payload
from lx_protocol import SETUP,SETUP_REPLY

class Setup:
    def __init__(self,device):self.device=device;self.token=None
    def call(self,op,data=b'',timeout=3):
        payload=bytes([op])+(self.token if op not in (1,2) and self.token else b'')+data
        reply=self.device.exchange(SETUP,payload,SETUP_REPLY,timeout=timeout).payload
        if len(reply)<3 or reply[0]!=op:raise RuntimeError('配置协议响应不匹配')
        code=int.from_bytes(reply[1:3],'little')
        if code:raise RuntimeError(f'设备未完成配置步骤 {op}（0x{code:04x}）；原配置会回退')
        return reply[3:]
    def info(self):
        data=self.call(1)
        if len(data)!=41 or data[0]!=1:raise RuntimeError('设备不支持此向导，请先升级固件')
        return {'role':'receiver' if data[1] else 'keyboard','flags':data[2],'configured':bool(data[3]),
                'mac':data[4:10].hex(),'ready':bool(data[10]),'channel':data[11],
                'ip':socket.inet_ntoa(data[14:18]),'peer':data[18:24].hex(),
                'pc_id':data[24:40].hex(),'pending':bool(data[40])}
    def begin(self):
        token=secrets.token_bytes(16);self.call(2,token);self.token=token
    def abort(self):
        if self.token:self.call(9);self.token=None
    def scan(self):
        count=self.call(11,timeout=15)[0];result=[]
        for i in range(count):
            row=self.call(12,bytes([i]));result.append({'channel':row[0],'rssi':int.from_bytes(row[1:2],'little',signed=True),
                                                     'secure':bool(row[2]),'ssid':row[3:].decode('utf8',errors='replace')})
        return result
    def wifi(self,ssid,password):
        config_payload('ssid',ssid);config_payload('pass',password)
        if not ssid:raise ValueError('请选择或填写 Wi-Fi 名称')
        a,b=ssid.encode('utf8'),password.encode('utf8');self.call(3,bytes([len(a),len(b)])+a+b)
    def pair(self,mac,key):self.call(4,bytes.fromhex(mac)+key)
    def trial(self):self.call(6,timeout=22)
    def commit(self):self.call(8)
    def seal(self):self.call(10);self.token=None

def inspect_devices():
    found=[]
    for candidate in devices():
        device=None
        try:
            device=Device(candidate['port']);info=Setup(device).info()
            if info['role']!=candidate['role']:raise RuntimeError('USB 角色与设备身份不一致')
            found.append({**candidate,**info})
        except Exception as error:found.append({**candidate,'error':str(error)})
        finally:
            if device:device.close()
    return found

def activate_receiver(candidate,notify=lambda text:None,timeout=15):
    """Reload the sealed pair key in ESP-NOW, without changing keyboard mode."""
    if candidate['role']!='receiver':raise ValueError('只能重启已配置的接收器')
    device=Device(candidate['port'])
    try:
        before=Setup(device).info()
        if before['role']!='receiver' or before['mac']!=candidate['mac']:
            raise RuntimeError('接收器身份已改变，未发送重启命令')
        if before['pending']:raise RuntimeError('接收器配置尚未完整保存，未重启')
        notify('配对已保存，正在重启接收器以加载新密钥…')
        device.configure('restart')
    finally:device.close()
    # Firmware delays reset to let the USB acknowledgement leave first.
    time.sleep(.6);until=time.monotonic()+timeout
    while time.monotonic()<until:
        current=next((d for d in devices() if d['role']=='receiver' and d['serial']==candidate['serial']),None)
        probe=None
        try:
            if current:
                probe=Device(current['port']);after=Setup(probe).info()
                if all(after[k]==before[k] for k in ('role','mac','peer','pc_id','flags')) and not after['pending']:
                    notify('接收器已重新连接，保存的配对身份核对通过。')
                    return after
        except (OSError,RuntimeError,TimeoutError,ValueError):pass
        finally:
            if probe:probe.close()
        time.sleep(.3)
    raise TimeoutError('配对已保存，但接收器重启后未确认恢复；请拔插接收器后重试连接，无需重新配对。')


def provision(keyboard,receiver,ssid=None,password='',notify=lambda text:None,allow_offline=False):
    """Prepare both, then persist both, then seal. Interruptions are explicit.

    Each device atomically stores current+previous; unsealed transactions revert
    on boot/120s timeout. A disconnect during the final two-device seal can leave
    a partial pair, reported for USB recovery rather than declared successful.
    """
    opened=[];sessions=[];sealed=[]
    try:
        for candidate in (keyboard,receiver):
            if candidate is None:continue
            device=Device(candidate['port']);opened.append(device);session=Setup(device)
            info=session.info()
            if info['mac']!=candidate['mac'] or info['role']!=candidate['role']:raise RuntimeError('设备身份已改变，请重新选择')
            session.begin();sessions.append(session)
        if not sessions or keyboard['role']!='keyboard':raise ValueError('请先选择键盘')
        if receiver:
            pair_key=secrets.token_bytes(16)
            sessions[0].pair(receiver['mac'],pair_key);sessions[1].pair(keyboard['mac'],pair_key)
        channel_states=[]
        for index,session in enumerate(sessions):
            label='键盘' if index==0 else '接收器'
            if ssid is not None:session.wifi(ssid,password)
            if allow_offline and ssid is None:session.call(6,b'\x01')
            else:notify(label+'：正在验证 Wi-Fi 与 IP 地址…');session.trial()
            info=session.info()
            channel_states.append(info)
            if not info['ready']:
                if not allow_offline:raise RuntimeError(label+'未取得 IP 地址')
                notify(label+'：离线绑定，未验证 Wi-Fi')
            else:notify(f"{label}：已连接，IP {info['ip']}，信道 {info['channel']}")
        if receiver and channel_states[0]['channel']!=channel_states[1]['channel']:
            raise RuntimeError('键盘和接收器的无线信道不同，请在向导中为两台设备配置同一个 Wi-Fi 后重试。')
        for session in sessions:session.commit()
        for session in sessions:session.seal();sealed.append(session)
        return [session.info() for session in sessions]
    except Exception as error:
        rollback_errors=[]
        for session in sessions:
            try:session.abort()
            except Exception:rollback_errors.append(session.device.role)
        if sealed or rollback_errors:
            raise RuntimeError('配置未完整确认；请保持两台设备 USB 连接并重新运行向导。未确认设备会在超时或重启后回退，已确认设备可能已保存新设置。') from error
        raise
    finally:
        for device in opened:device.close()

