"""Flash generated IDF images with a verified full backup; never erase all.

Use build.py BOARD --flash --port PORT for the normal one-command workflow.
Requires the esptool 4.x environment installed by ESP-IDF 5.5.6 constraints.
Backups contain private device settings: keep artifacts/ out of source releases.
"""
import argparse
import contextlib
import datetime
import hashlib
import json
from pathlib import Path
import struct
import sys
import time
from types import SimpleNamespace

ROOT=Path(__file__).resolve().parents[1]
NAMES={'keyboard':'lx_keyboard','knob':'lx_knob_idf','receiver':'lx_receiver'}

def require(condition,message):
    if not condition:raise RuntimeError(message)

def complement(size,intervals):
    end=0
    for start,stop in sorted(intervals):
        require(start>=end and stop<=size,'Overlapping or out-of-range flash images')
        if start>end:yield end,start-end
        end=stop
    if end<size:yield end,size-end

def reset_control(port,download):
    import serial
    from esptool.reset import ClassicReset,HardReset
    conn=serial.Serial(port=None,baudrate=115200,timeout=.1)
    conn.dtr=False;conn.rts=False;conn.port=port;conn.open()
    try:
        if download:ClassicReset(conn,.1)()
        else:HardReset(conn)()
    finally:conn.close()
    time.sleep(2)

def read_backup(esp,address,length):
    # Bound native USB bursts: some Windows USB paths truncate large stub reads.
    # Each esptool read retains its own digest check; the caller checks the full chunk.
    native=esp.uses_usb_otg() or esp.uses_usb_jtag_serial()
    if not native:return esp.read_flash(address,length)
    return b''.join(esp.read_flash(address+i,min(4096,length-i))
                    for i in range(0,length,4096))

def run(args):
    import esptool
    from esptool.cmds import detect_chip,detect_flash_size,write_flash
    require(esptool.__version__.startswith('4.'),'Use the Python environment installed by ESP-IDF 5.5.6 (esptool 4.x)')
    build=ROOT/'firmware'/args.board/'build'
    metadata=json.loads((build/'project_description.json').read_text(encoding='utf8'))
    require(metadata['git_revision'].lstrip('v')=='5.5.6','Build is not exactly ESP-IDF 5.5.6')
    manifest=json.loads((build/'flasher_args.json').read_text())
    files=sorted((int(a,0),(build/p).resolve()) for a,p in manifest['flash_files'].items())
    for address,path in files:
        require(path.is_relative_to(build.resolve()) and path.is_file(),'Invalid generated flash path')
        require(address%4096==0,'Flash image must start at an erase-sector boundary')
    app_offset=int(manifest['app']['offset'],0)
    app=(build/manifest['app']['file']).read_bytes()
    require(app[0]==0xe9 and app[0x20+112:0x20+144].split(b'\0')[0]==b'v5.5.6','Unexpected application SDK')
    out=ROOT/'artifacts/device-backups'/f'{datetime.datetime.now():%Y%m%d-%H%M%S-%f}-{args.board}'
    out.mkdir(parents=True)
    report={'board':args.board,'port':args.port,'written':False,'verified':False,'images':[]}
    def save():(out/'report.json').write_text(json.dumps(report,indent=2),encoding='utf8')
    save();esp=None;write_started=False
    try:
        if args.control_port:reset_control(args.control_port,True)
        esp=detect_chip(args.port,baud=115200,connect_attempts=3)
        require(esp.CHIP_NAME=='ESP32-S3' and not esp.secure_download_mode,'Unsupported/secured chip')
        mac=':'.join(f'{b:02x}' for b in esp.read_mac())
        if args.expect_mac:require(mac.lower()==args.expect_mac.lower(),'Device MAC does not match')
        require(not esp.get_flash_encryption_enabled() and not esp.get_secure_boot_enabled(),'Secure boot/encrypted devices need a separate signed update procedure')
        if not esp.IS_STUB:esp=esp.run_stub()
        capacity=detect_flash_size(esp)
        require(capacity and capacity.endswith('MB'),'Cannot determine Flash capacity')
        size=int(capacity[:-2])*1024*1024
        required=int(manifest['flash_settings']['flash_size'][:-2])*1024*1024
        require(size>=required,'Insufficient physical Flash capacity')
        esp.change_baud(460800);esp.flash_set_parameters(size)
        table_offset=int(manifest['partition-table']['offset'],0)
        expected=(build/manifest['partition-table']['file']).read_bytes()
        actual=esp.read_flash(table_offset,len(expected))
        blank=actual==b'\xff'*len(actual)
        if blank:
            require(esp.flash_md5sum(0,size)==hashlib.md5(b'\xff'*size).hexdigest(),'Missing partition table on a nonblank device; stop to protect existing data')
        else:
            require(actual==expected,'Partition layout differs; stop to protect existing data')
            descriptor=esp.read_flash(app_offset+0x20,256)
            require(struct.unpack_from('<I',descriptor)[0]==0xabcd5432,'Current application descriptor missing')
            require(descriptor[48:80].split(b'\0')[0].decode()==NAMES[args.board],'Wrong board/application selected')
        report.update(mac=mac,physical_flash=capacity,blank_device=blank,backup_verified=False)
        # A cached chunk is reused only after matching a fresh digest from this device.
        cache=Path(args.backup_cache) if args.backup_cache else ROOT/'artifacts/flash-cache'
        cache=cache/mac.replace(':','');cache.mkdir(parents=True,exist_ok=True)
        backup=out/'flash-before.bin';digest=hashlib.sha256();md5=hashlib.md5()
        with backup.open('wb') as target:
            for address in range(0,size,0x100000):
                length=min(0x100000,size-address);expected_md5=esp.flash_md5sum(address,length)
                cached=cache/f'{address:08x}-{expected_md5}.bin'
                data=cached.read_bytes() if cached.exists() else b''
                if len(data)!=length or hashlib.md5(data).hexdigest()!=expected_md5:
                    data=b'\xff'*length
                    if hashlib.md5(data).hexdigest()!=expected_md5:data=read_backup(esp,address,length)
                    require(len(data)==length and hashlib.md5(data).hexdigest()==expected_md5,'Backup chunk mismatch')
                    cached.write_bytes(data)
                target.write(data);digest.update(data);md5.update(data)
                print(f'Backup {(address+length)*100//size}%',flush=True)
        require(md5.hexdigest()==esp.flash_md5sum(0,size),'Full Flash backup mismatch')
        require(hashlib.sha256(backup.read_bytes()).hexdigest()==digest.hexdigest(),'Backup file verification failed')
        report.update(backup_verified=True,backup_sha256=digest.hexdigest());save()
        updates=[]
        for address,path in files:
            data=path.read_bytes()
            # Existing OTA selection is device state, not firmware. Fresh devices need initial data.
            preserve=not blank and address==int(manifest['otadata']['offset'],0)
            match=esp.flash_md5sum(address,len(data))==hashlib.md5(data).hexdigest()
            report['images'].append({'offset':address,'file':str(path.relative_to(build.resolve())),'sha256':hashlib.sha256(data).hexdigest(),'length':len(data),'preserved':preserve,'already_matched':match})
            if not preserve and not match:updates.append((address,path))
        intervals=[(a,a+((p.stat().st_size+4095)//4096)*4096) for a,p in updates]
        protected=list(complement(size,intervals))
        before=backup.read_bytes()
        report['protected']=[{'offset':a,'length':n,'md5':hashlib.md5(before[a:a+n]).hexdigest()} for a,n in protected]
        save()
        if updates:
            write_started=True;report['write_started']=True;save()
            with contextlib.ExitStack() as stack:
                opts=SimpleNamespace(chip='esp32s3',compress=True,no_compress=False,no_stub=False,force=False,encrypt=False,encrypt_files=None,ignore_flash_encryption_efuse_setting=False,flash_size='keep',flash_mode='keep',flash_freq='keep',erase_all=False,addr_filename=[(a,stack.enter_context(p.open('rb'))) for a,p in updates])
                write_flash(esp,opts)
            report['written']=True;save()
        for entry in report['images']:
            if not entry['preserved']:
                data=(build/entry['file']).read_bytes()
                require(esp.flash_md5sum(entry['offset'],len(data))==hashlib.md5(data).hexdigest(),'Written image verification failed')
        for entry in report['protected']:
            require(esp.flash_md5sum(entry['offset'],entry['length'])==entry['md5'],'Protected Flash changed')
        report.update(verified=True,all_unwritten_sectors_unchanged=True);save()
        esp._port.baudrate=115200
        if args.control_port:
            esp._port.close();esp=None;reset_control(args.control_port,False)
        else:
            esp.hard_reset();esp._port.timeout=.1
            data=bytearray();end=time.monotonic()+12
            while time.monotonic()<end:data.extend(esp._port.read(4096))
            (out/'boot.log').write_bytes(data)
            report['bootloader_556_logged']=b'ESP-IDF v5.5.6 2nd stage bootloader' in data
            report['boot_error']=any(s in data for s in (b'Guru Meditation',b'abort() was called',b'assert failed'))
            save();require(not report['boot_error'],'Boot error: inspect boot.log')
        print('Flash verified. Configuration/storage sectors unchanged. Report:',out,flush=True)
    except Exception as error:
        report['error']=repr(error);save()
        if esp and not write_started:
            try:esp.hard_reset()
            except Exception:pass
        raise
    finally:
        if esp:esp._port.close()

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('board',choices=NAMES)
    parser.add_argument('--port',required=True)
    parser.add_argument('--control-port')
    parser.add_argument('--expect-mac')
    parser.add_argument('--backup-cache')
    run(parser.parse_args())

if __name__=='__main__':main()
