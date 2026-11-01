import contextlib,hashlib,importlib.util,io,json,struct,tempfile,unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

root=Path(__file__).resolve().parents[1]
path=root/'scripts/flash.py'
spec=importlib.util.spec_from_file_location('flash_workflow',path)
f=importlib.util.module_from_spec(spec);spec.loader.exec_module(f)

class Device:
    CHIP_NAME='ESP32-S3';secure_download_mode=False;IS_STUB=True
    def __init__(self,data):
        self.data=bytearray(data);self.writes=0;self.resets=0;self.corrupt=False
        self._port=SimpleNamespace(close=lambda:None,read=lambda n:b'ESP-IDF v5.5.6 2nd stage bootloader\n')
    def read_mac(self):return bytes.fromhex('010203040506')
    def get_flash_encryption_enabled(self):return False
    def get_secure_boot_enabled(self):return False
    def change_baud(self,n):pass
    def flash_set_parameters(self,n):pass
    def uses_usb_otg(self):return False
    def uses_usb_jtag_serial(self):return False
    def read_flash(self,a,n):return bytes(self.data[a:a+n])
    def flash_md5sum(self,a,n):return hashlib.md5(self.data[a:a+n]).hexdigest()
    def hard_reset(self):self.resets+=1

class Tests(unittest.TestCase):
    def setUp(self):
        (root/'artifacts/tests').mkdir(parents=True,exist_ok=True)
        self.temp=tempfile.TemporaryDirectory(dir=root/'artifacts/tests')
        self.base=Path(self.temp.name);self.build=self.base/'receiver/build';self.build.mkdir(parents=True)
        self.meta={'git_revision':'v5.5.6'}
        (self.build/'project_description.json').write_text(json.dumps(self.meta))
        self.table=b'test-partition-layout'+b'\xff'*200
        app=bytearray(b'\0'*1024);app[0]=0xe9;struct.pack_into('<I',app,0x20,0xabcd5432)
        app[0x20+48:0x20+48+11]=b'lx_receiver';app[0x20+112:0x20+118]=b'v5.5.6'
        self.files={0:b'new-boot'+b'\x01'*700,0x8000:self.table,0xd000:b'\xff'*8192,0x10000:bytes(app)}
        names={a:f'{a:x}.bin' for a in self.files}
        for a,data in self.files.items():(self.build/names[a]).write_bytes(data)
        manifest={'flash_files':{hex(a):n for a,n in names.items()},'app':{'offset':'0x10000','file':names[0x10000]},'partition-table':{'offset':'0x8000','file':names[0x8000]},'otadata':{'offset':'0xd000','file':names[0xd000]},'flash_settings':{'flash_size':'4MB'}}
        (self.build/'flasher_args.json').write_text(json.dumps(manifest))
        data=bytearray(b'\xff'*0x400000)
        for a,b in self.files.items():data[a:a+len(b)]=b
        data[0:8]=b'old-boot';data[0x10000+900]=99
        data[0xd000:0xd008]=b'OTAstate';data[0x9000:0x9008]=b'private!'
        self.device=Device(data);self.before=bytes(data)
        self.args=SimpleNamespace(board='receiver',port='FAKE',control_port=None,expect_mac='01:02:03:04:05:06',backup_cache=None)
    def tearDown(self):self.temp.cleanup()
    def execute(self,capacity='4MB',corrupt_read=False):
        def write(esp,opts):
            self.assertFalse(opts.erase_all)
            for a,stream in opts.addr_filename:
                data=stream.read();n=(len(data)+4095)//4096*4096
                esp.data[a:a+n]=b'\xff'*n;esp.data[a:a+len(data)]=data;esp.writes+=1
            if esp.corrupt:esp.data[0x9000]=0
        ticks=iter(range(0,10000,5))
        with contextlib.ExitStack() as s:
            s.enter_context(patch.object(f,'ROOT',self.base))
            s.enter_context(patch('esptool.cmds.detect_chip',return_value=self.device))
            s.enter_context(patch('esptool.cmds.detect_flash_size',return_value=capacity))
            s.enter_context(patch('esptool.cmds.write_flash',side_effect=write))
            s.enter_context(patch.object(f.time,'monotonic',side_effect=lambda:next(ticks)))
            if corrupt_read:s.enter_context(patch.object(self.device,'read_flash',side_effect=lambda a,n: b'\0'*n if n==0x100000 else bytes(self.device.data[a:a+n])))
            s.enter_context(contextlib.redirect_stdout(io.StringIO()))
            f.run(self.args)
        return json.loads(next((self.base/'artifacts/device-backups').glob('*/report.json')).read_text())
    def test_existing_preserves_configuration_and_ota(self):
        r=self.execute();self.assertTrue(r['verified']);self.assertEqual(self.device.writes,2)
        self.assertEqual(self.device.data[0x9000:0xf000],self.before[0x9000:0xf000])
        self.assertTrue(r['bootloader_556_logged'])
    def test_blank_initial_install(self):
        self.device.data[:]=b'\xff'*len(self.device.data)
        r=self.execute();self.assertTrue(r['blank_device']);self.assertTrue(r['verified'])
        for a,b in self.files.items():self.assertEqual(self.device.data[a:a+len(b)],b)
    def test_wrong_layout_no_write(self):
        self.device.data[0x8000]=0
        with self.assertRaisesRegex(RuntimeError,'Partition layout differs'):self.execute()
        self.assertEqual(self.device.writes,0)
    def test_wrong_board_no_write(self):
        self.device.data[0x10000+0x20+48]=ord('X')
        with self.assertRaisesRegex(RuntimeError,'Wrong board'):self.execute()
        self.assertEqual(self.device.writes,0)
    def test_small_flash_no_write(self):
        with self.assertRaisesRegex(RuntimeError,'Insufficient'):self.execute(capacity='2MB')
        self.assertEqual(self.device.writes,0)
    def test_backup_failure_no_write(self):
        with self.assertRaisesRegex(RuntimeError,'Backup chunk mismatch'):self.execute(corrupt_read=True)
        self.assertEqual(self.device.writes,0)
    def test_protected_data_corruption_detected(self):
        self.device.corrupt=True
        with self.assertRaisesRegex(RuntimeError,'Protected Flash changed'):self.execute()
    def test_wrong_mac_no_write(self):
        self.args.expect_mac='00:00:00:00:00:00'
        with self.assertRaisesRegex(RuntimeError,'MAC does not match'):self.execute()
        self.assertEqual(self.device.writes,0)
    def test_native_usb_truncates_large_reads(self):
        read=self.device.read_flash
        def short_read(a,n):
            if n>4096:raise RuntimeError('USB transfer truncated')
            return read(a,n)
        with patch.object(self.device,'uses_usb_otg',return_value=True), patch.object(self.device,'read_flash',side_effect=short_read):
            r=self.execute()
        self.assertTrue(r['verified'])
        self.assertEqual(self.device.data[0x9000:0xf000],self.before[0x9000:0xf000])

if __name__ == '__main__':
    unittest.main(verbosity=2)
