"""Identity-checked telemetry, with no mode/configuration commands."""
class SyncUnavailable(Exception):pass
class AmbiguousTarget(SyncUnavailable):pass

def identity(payload):
    if len(payload)<2:raise SyncUnavailable('键盘确认不完整')
    error=int.from_bytes(payload[:2],'little')
    if error:raise SyncUnavailable('键盘通道未就绪，请检查连接或更新键盘固件')
    if len(payload)!=10 or payload[2]>3 or payload[3]!=0 or not any(payload[4:]):
        raise SyncUnavailable('键盘身份无效')
    return {'mode':payload[2],'mac':payload[4:],'serial':'LXKBDEV'+payload[4:].hex().upper()}

def stats_payload(values,peer):
    if any(type(values.get(k)) is not int or not 0<=values[k]<=100 for k in ('cpu','gpu','ram')):
        raise ValueError('Invalid load sample')
    return bytes([33,values['cpu'],values['gpu'],values['ram']])+peer['mac']

def confirm(payload,peer):
    reply=identity(payload)
    if reply!=peer:raise SyncUnavailable('键盘身份或模式已变化，暂缓同步')
    return reply
