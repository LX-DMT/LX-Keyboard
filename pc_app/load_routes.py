"""One confirmed keyboard route per sample. Receiver USB presence is not a route."""
from device import Device,devices
from lx_protocol import CONTROL,CONTROL_REPLY
from load_protocol import identity,confirm,stats_payload,SyncUnavailable,AmbiguousTarget

CHANNELS={'auto':'自动选择','usb':'USB','ble':'蓝牙','wifi':'接收器 Wi-Fi','now':'接收器 ESP-NOW'}

class LoadRoutes:
    def __init__(self,ble=None):
        if ble is None:
            from ble_load import BleLoad
            ble=BleLoad()
        self.ble=ble

    def close(self):self.ble.close()

    def usb_send(self,candidate,values,channel='auto'):
        device=None
        try:
            device=Device(candidate['port'])
            peer=identity(device.exchange(CONTROL,b'',CONTROL_REPLY,timeout=1.25).payload)
            if candidate['role']=='keyboard':
                if peer['mode']!=0 or peer['serial']!=candidate['serial'].upper():
                    raise SyncUnavailable('键盘身份或模式不匹配，暂缓同步')
                route='USB'
            else:
                if peer['mode'] not in (2,3):raise SyncUnavailable('接收器尚未连通键盘，等待连接')
                if channel in ('wifi','now') and peer['mode']!={'wifi':2,'now':3}[channel]:
                    raise SyncUnavailable('等待键盘切到'+CHANNELS[channel]+'模式')
                route='接收器 Wi-Fi' if peer['mode']==2 else '接收器 ESP-NOW'
                # Legacy receiver heartbeats repeat its cached values. Refresh
                # that cache before the end-to-end command so old values cannot
                # overwrite the newly confirmed sample. Its ACK alone is not success.
                device.stats(values)
            # Packet sequence + target MAC + keyboard reply confirms acceptance.
            reply=device.exchange(CONTROL,stats_payload(values,peer),CONTROL_REPLY,timeout=1.25)
            confirm(reply.payload,peer)
            return '键盘已接收 · '+route
        finally:
            if device:
                try:device.close()
                except OSError:pass

    def send(self,values,channel='auto'):
        if channel not in CHANNELS:raise ValueError('Unknown load channel')
        if channel=='ble':
            peer=self.ble.probe()
            if not peer:return '等待蓝牙连接 · 请确认键盘已配对并处于蓝牙模式'
            self.ble.send(values,peer)
            return '键盘已接收 · 蓝牙'
        candidates=devices();keyboards=[d for d in candidates if d['role']=='keyboard']
        receivers=[d for d in candidates if d['role']=='receiver']
        if channel in ('usb','wifi','now'):
            self.ble.disconnect()
            selected=keyboards if channel=='usb' else receivers
            if len(selected)>1:raise AmbiguousTarget('检测到多个所选通道设备，请仅连接要同步的一台')
            if not selected:return '等待连接 · '+CHANNELS[channel]
            return self.usb_send(selected[0],values,channel)
        if len(keyboards)>1:raise AmbiguousTarget('检测到多台键盘 USB，请仅连接要同步的一台')
        if keyboards:
            self.ble.disconnect()
            return self.usb_send(keyboards[0],values)
        peer=self.ble.probe()
        if peer:
            self.ble.send(values,peer)
            return '键盘已接收 · 蓝牙'
        if len(receivers)>1:raise AmbiguousTarget('检测到多个接收器，请仅连接要同步的一个')
        if receivers:return self.usb_send(receivers[0],values)
        return '等待键盘连接 · 支持 USB、蓝牙及接收器'
