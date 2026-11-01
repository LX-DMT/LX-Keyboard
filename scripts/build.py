"""Build the portable release with an explicitly selected ESP-IDF SDK.

Run from an activated IDF terminal, or supply --idf /path/to/esp-idf.
Add --flash --port PORT to back up and flash the selected board after building.
This helper never changes global environment/settings.
"""
import argparse
import datetime
import os
from pathlib import Path
import re
import subprocess
import sys
import threading

ROOT=Path(__file__).resolve().parents[1]
VERSION=(ROOT/'IDF_VERSION').read_text(encoding='utf8').strip()

def wake_keyboard(port):
    import serial
    conn=serial.Serial(port=None,baudrate=115200,timeout=.2,write_timeout=2)
    conn.dtr=False;conn.rts=False;conn.port=port;conn.open()
    try:
        conn.reset_input_buffer();conn.write(b'power wake\n')
        import time
        end=time.monotonic()+4;response=b''
        while time.monotonic()<end:
            response+=conn.read(4096)
            if b'activity=1' in response:return
        raise RuntimeError('Keyboard did not acknowledge power wake')
    finally:conn.close()

class KnobPower:
    def __init__(self,port):self.port=port;self.stop=threading.Event();self.error=None;self.thread=None
    def __enter__(self):
        if self.port:
            wake_keyboard(self.port)
            def worker():
                while not self.stop.wait(20):
                    try:wake_keyboard(self.port)
                    except Exception as error:
                        self.error=error;print('Keyboard keep-awake failed:',error,flush=True);return
            self.thread=threading.Thread(target=worker,daemon=True);self.thread.start()
        return self
    def check(self):
        if self.error:raise RuntimeError('Knob power cannot be maintained') from self.error
    def __exit__(self,*exc):
        self.stop.set()
        if self.thread:self.thread.join(timeout=6)

def environment(idf):
    version=(idf/'tools/cmake/version.cmake').read_text(encoding='utf8')
    found='.'.join(re.search(r'set\(IDF_VERSION_'+p+r' (\d+)\)',version)[1] for p in ('MAJOR','MINOR','PATCH'))
    if found!=VERSION:raise RuntimeError(f'Requires ESP-IDF {VERSION}, selected {found}')
    env=os.environ.copy();env['IDF_PATH']=str(idf)
    if env.get('IDF_PYTHON_ENV_PATH'):
        env['IDF_PYTHON_ENV_PATH']=str(Path(env['IDF_PYTHON_ENV_PATH']).resolve())
    env.setdefault('IDF_TOOLS_PATH',str(Path.home()/'.espressif'))
    # Limit trust to this SDK for this process; do not edit global Git config.
    count=int(env.get('GIT_CONFIG_COUNT','0'))
    env[f'GIT_CONFIG_KEY_{count}']='safe.directory';env[f'GIT_CONFIG_VALUE_{count}']=idf.as_posix()
    env['GIT_CONFIG_COUNT']=str(count+1)
    exported=subprocess.check_output([sys.executable,str(idf/'tools/idf_tools.py'),'export','--format','key-value'],env=env,text=True,encoding='utf8')
    for line in exported.splitlines():
        if '=' in line:
            key,value=line.split('=',1)
            env[key]=value.replace('%PATH%',env.get('PATH','')).replace('${PATH}',env.get('PATH','')).replace('$PATH',env.get('PATH',''))
    env['IDF_TARGET']='esp32s3';env['PYTHONUTF8']='1'
    env['CCACHE_DIR']=str(ROOT/'artifacts/ccache')
    return env

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('board',choices=('keyboard','knob','receiver','all'))
    parser.add_argument('--idf',default=os.environ.get('IDF_PATH'))
    parser.add_argument('--action',choices=('build','reconfigure','save-defconfig'),default='build')
    parser.add_argument('--flash',action='store_true',help='Build, back up and flash one board, including bootloader')
    parser.add_argument('--port',help='Download port (not the product USB configuration port)')
    parser.add_argument('--control-port',help='Optional external reset adapter for native USB download')
    parser.add_argument('--expect-mac',help='Optional extra device identity check')
    parser.add_argument('--backup-cache',help='Optional verified backup chunk cache directory')
    parser.add_argument('--power-port',help='Knob only: keyboard diagnostic port to keep knob power awake')
    args=parser.parse_args()
    if args.flash and (args.board=='all' or args.action!='build' or not args.port):
        parser.error('--flash requires one board, --action build and --port')
    if args.power_port and (args.board!='knob' or args.power_port==args.port):
        parser.error('--power-port must be the separate keyboard diagnostic port, with board knob')
    if not args.idf:parser.error('Set IDF_PATH in an activated ESP-IDF terminal, or use --idf PATH')
    idf=Path(args.idf).expanduser().resolve();env=environment(idf)
    logs=ROOT/'artifacts/logs';logs.mkdir(parents=True,exist_ok=True)
    boards=('keyboard','knob','receiver') if args.board=='all' else (args.board,)
    with KnobPower(args.power_port) as power:
      for board in boards:
        project=ROOT/'firmware'/board
        path=logs/f'{datetime.datetime.now():%Y%m%d-%H%M%S}-{board}-{args.action}.log'
        command=[sys.executable,str(idf/'tools/idf.py'),'-C',str(project),args.action]
        print(f'{board}: ESP-IDF {VERSION}; log={path}',flush=True)
        with path.open('w',encoding='utf8') as log:
            result=subprocess.run(command,env=env,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT)
        if result.returncode:
            print(path.read_text(encoding='utf8',errors='replace')[-12000:])
            raise SystemExit(result.returncode)
        print(f'{board}: {args.action} passed',flush=True)
        if args.flash:
            power.check()
            command=[sys.executable,str(ROOT/'scripts/flash.py'),board,'--port',args.port]
            for option in ('control_port','expect_mac','backup_cache'):
                if getattr(args,option):command += ['--'+option.replace('_','-'),getattr(args,option)]
            subprocess.run(command,env=env,cwd=ROOT,check=True)
            power.check()

if __name__=='__main__':main()
