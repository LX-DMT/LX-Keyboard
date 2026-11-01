"""Windows load sampling. A missing GPU measurement stays None, never a fake reading."""
import ctypes as C
from ctypes import wintypes as W
import re

def wire_values(values):
    """Legacy knob has no N/A code; keep measured CPU/RAM with GPU=0 placeholder."""
    if not values or values.get('cpu') is None or values.get('ram') is None:return None
    return {'cpu':values['cpu'],'gpu':0 if values.get('gpu') is None else values['gpu'],'ram':values['ram']}

kernel=C.WinDLL('kernel32',use_last_error=True)
class MEMORY(C.Structure):
    _fields_=[('length',W.DWORD),('load',W.DWORD)]+[(name,C.c_ulonglong) for name in
        ('physical','available','page_file','available_page_file','virtual','available_virtual','extended')]
class FILETIME(C.Structure):
    _fields_=[('low',W.DWORD),('high',W.DWORD)]
    def number(self):return (self.high<<32)|self.low
kernel.GetSystemTimes.argtypes=[C.POINTER(FILETIME)]*3
kernel.GlobalMemoryStatusEx.argtypes=[C.POINTER(MEMORY)]

class VALUE(C.Structure):
    _fields_=[('status',W.DWORD),('value',C.c_double)]
class ITEM(C.Structure):
    _fields_=[('name',W.LPWSTR),('value',VALUE)]

class Metrics:
    def __init__(self):
        self.previous=None;self.query=W.HANDLE();self.counter=W.HANDLE();self.gpu_error=None
        self.pdh=C.WinDLL('pdh')
        self.pdh.PdhOpenQueryW.argtypes=[W.LPCWSTR,C.c_size_t,C.POINTER(W.HANDLE)]
        self.pdh.PdhAddEnglishCounterW.argtypes=[W.HANDLE,W.LPCWSTR,C.c_size_t,C.POINTER(W.HANDLE)]
        self.pdh.PdhCollectQueryData.argtypes=[W.HANDLE]
        self.pdh.PdhGetFormattedCounterArrayW.argtypes=[W.HANDLE,W.DWORD,C.POINTER(W.DWORD),C.POINTER(W.DWORD),C.c_void_p]
        self.pdh.PdhGetFormattedCounterArrayW.restype=W.DWORD
        self.pdh.PdhCloseQuery.argtypes=[W.HANDLE]
        rc=self.pdh.PdhOpenQueryW(None,0,C.byref(self.query))
        if not rc:rc=self.pdh.PdhAddEnglishCounterW(self.query,r'\GPU Engine(*)\Utilization Percentage',0,C.byref(self.counter))
        if rc:self.gpu_error=f'GPU counter unavailable (0x{rc&0xffffffff:08x})'
        self.sample()
    def sample(self):
        idle,kern,user=FILETIME(),FILETIME(),FILETIME()
        if not kernel.GetSystemTimes(C.byref(idle),C.byref(kern),C.byref(user)):raise C.WinError(C.get_last_error())
        now=(idle.number(),kern.number()+user.number());cpu=None
        if self.previous:
            total=now[1]-self.previous[1]
            if total>0:cpu=max(0,min(100,round(100*(1-(now[0]-self.previous[0])/total))))
        self.previous=now
        memory=MEMORY();memory.length=C.sizeof(memory)
        if not kernel.GlobalMemoryStatusEx(C.byref(memory)):raise C.WinError(C.get_last_error())
        gpu=None
        if self.counter.value:
            rc=self.pdh.PdhCollectQueryData(self.query)
            if not rc:
                size=W.DWORD();count=W.DWORD()
                rc=self.pdh.PdhGetFormattedCounterArrayW(self.counter,0x200,C.byref(size),C.byref(count),None)
                if rc==0x800007d2 and size.value:
                    buffer=C.create_string_buffer(size.value)
                    rc=self.pdh.PdhGetFormattedCounterArrayW(self.counter,0x200,C.byref(size),C.byref(count),buffer)
                    if not rc:
                        items=C.cast(buffer,C.POINTER(ITEM));engines={}
                        for i in range(count.value):
                            item=items[i]
                            if item.value.status not in (0,1):continue
                            # Sum process usage per physical engine; use busiest
                            # engine (Task Manager semantics), not sum of engines.
                            key=re.sub(r'^pid_\d+_','',item.name)
                            engines[key]=engines.get(key,0)+item.value.value
                        if engines:gpu=max(0,min(100,round(max(engines.values()))));self.gpu_error=None
            if gpu is None:self.gpu_error='GPU counter has no current sample'
        return {'cpu':cpu,'gpu':gpu,'ram':memory.load,'gpu_note':self.gpu_error}
    def close(self):
        if self.query.value:self.pdh.PdhCloseQuery(self.query);self.query=W.HANDLE()
