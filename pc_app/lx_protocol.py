"""LX v2 wire format, separate from the unchanged knob LX v1 protocol."""
from dataclasses import dataclass
import struct
import binascii
HEADER=struct.Struct('<2sBBHII')
MAX_PAYLOAD=234
KEYBOARD,ACK,KEEPALIVE,LED_STATE,MOUSE,CONSUMER,HELLO,RELEASE,HOST_STATS,TEXT,CONFIG=range(1,12)
CONTROL,CONTROL_REPLY=12,13
SETUP,SETUP_REPLY=14,15
ACTIVATION,ACTIVATION_REPLY=16,17
CUSTOM_KEYS,CUSTOM_KEYS_REPLY=18,19
LIGHTING,LIGHTING_REPLY=20,21
@dataclass(frozen=True)
class Packet:
    type:int
    sequence:int
    session:int
    payload:bytes=b''

def encode(packet:Packet)->bytes:
    if len(packet.payload)>MAX_PAYLOAD: raise ValueError('payload too long')
    body=HEADER.pack(b'LX',2,packet.type,len(packet.payload),packet.sequence,packet.session)+packet.payload
    return body+struct.pack('<H',binascii.crc_hqx(body,0xffff))

def decode(data:bytes)->Packet:
    if not 16<=len(data)<=250: raise ValueError('packet length')
    magic,version,kind,length,sequence,session=HEADER.unpack_from(data)
    if magic!=b'LX' or version!=2 or len(data)!=16+length: raise ValueError('packet header')
    if binascii.crc_hqx(data[:-2],0xffff)!=struct.unpack_from('<H',data,len(data)-2)[0]: raise ValueError('packet CRC')
    return Packet(kind,sequence,session,data[14:-2])

class StreamDecoder:
    """Handles serial/TCP splits with bounded buffering; only valid frames escape."""
    def __init__(self): self.buffer=bytearray()
    def feed(self,data:bytes):
        packets=[]
        for byte in data:
            self.buffer.append(byte)
            while len(self.buffer)>=16:
                length=int.from_bytes(self.buffer[4:6],'little')
                if self.buffer[:3]!=b'LX\x02' or length>MAX_PAYLOAD:
                    del self.buffer[0];continue
                if len(self.buffer)<16+length: break
                try: packet=decode(bytes(self.buffer[:16+length]))
                except ValueError: del self.buffer[0];continue
                packets.append(packet);del self.buffer[:16+length]
        return packets
