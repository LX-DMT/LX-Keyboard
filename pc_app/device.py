"""Only discover the new product CDC interfaces; never use flashing ports."""
import sys
from pathlib import Path
import time
import secrets
import serial
from serial.tools import list_ports
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'shared'/'protocol'))
from lx_protocol import Packet,StreamDecoder,encode,HELLO,ACK,CONFIG,HOST_STATS

def config_payload(field,value=None):
    if field=='restart':return bytes([7])
    if field not in ('ssid','pass'):raise ValueError('仅支持网络配置；小智请使用官方激活')
    code={'ssid':2,'pass':3}[field]
    data=value.encode('utf8')
    if b'\0' in data:raise ValueError('网络设置包含无效字符')
    if field=='ssid' and len(data)>32:raise ValueError('Wi-Fi 名称过长（最多 32 字节）')
    if field=='pass' and len(data) not in (0,*range(8,64)):raise ValueError('Wi-Fi 密码需为 8～63 字节；无密码网络可留空')
    return bytes([code])+data

def check_config_reply(packet):
    if packet.payload!=bytes(2):raise RuntimeError(f'Device rejected setting (0x{int.from_bytes(packet.payload,"little"):04x})')

def devices():
    return sorted([{'port':p.device,'role':'keyboard' if p.pid==0x4001 else 'receiver','serial':p.serial_number}
        for p in list_ports.comports() if p.vid==0x303a and p.pid in (0x4001,0x4002) and
        (p.serial_number or '').startswith(('LXKBDEV','LXRXDEV'))],key=lambda d:(d['role']!='keyboard',d['port']))

def device_present(candidate):
    return candidate is not None and any(all(d.get(k)==candidate.get(k) for k in ('port','role','serial')) for d in devices())

class Device:
    def __init__(self,port):
        candidate=next((x for x in devices() if x['port']==port),None)
        if not candidate:raise ValueError('设备产品 USB 已断开。键盘切到无线模式后，请用旋钮切回 USB 再配网。')
        self.serial=serial.Serial(port=None,baudrate=115200,timeout=.05,write_timeout=.5)
        self.serial.dtr=False;self.serial.rts=False;self.serial.port=port;self.serial.open()
        self.decoder=StreamDecoder();self.sequence=secrets.randbits(32);self.role=candidate['role'];self.port=port
        try:
            response=self.exchange(HELLO,b'',HELLO)
            if response.payload.decode()!=self.role:raise RuntimeError('Unexpected device identity')
        except Exception:self.close();raise
    def exchange(self,kind,payload,reply_kind,timeout=1.5):
        self.sequence=(self.sequence+1)&0xffffffff
        self.serial.write(encode(Packet(kind,self.sequence,0,payload)))
        until=time.monotonic()+timeout
        while time.monotonic()<until:
            for packet in self.decoder.feed(self.serial.read(256)):
                if packet.sequence==self.sequence and packet.type==reply_kind:return packet
        raise TimeoutError(f'{self.port}: no reply')
    def configure(self,field,value=None):
        check_config_reply(self.exchange(CONFIG,config_payload(field,value),ACK))
    def stats(self,values):
        if any(type(values.get(k)) is not int or not 0<=values[k]<=100 for k in ('cpu','gpu','ram')):
            raise ValueError('Invalid load sample')
        reply=self.exchange(HOST_STATS,bytes(values[k] for k in ('cpu','gpu','ram')),ACK)
        if reply.payload:raise RuntimeError('负载同步响应不匹配')
    def close(self):self.serial.close()
