import importlib.util
import io
import json
from pathlib import Path
import tempfile
import types
import unittest
from unittest.mock import patch

D=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('candidate_host',D/'tools/katmed_usb_setup.py');tool=importlib.util.module_from_spec(spec);spec.loader.exec_module(tool)
SECRET='SYNTHETIC_PRIVATE_VALUE_NOT_FOR_OUTPUT'
class Port:
    def __init__(self,clock,mode='cap',payload=None):self.clock=clock;self.mode=mode;self.payload=payload;self.reads=0;self.bytes=0;self.purges=0;self.closed=False;self.writes=[]
    def read(self,size):
        self.reads+=1;self.clock[0]+=1 if self.mode=='under'else .01
        if self.mode=='error':raise OSError(SECRET)
        value=self.payload if self.payload is not None else (b'x' if self.mode=='under'else b'x'*size)
        if isinstance(value,bytes):self.bytes+=len(value)
        return value
    def reset_input_buffer(self):
        self.purges+=1
        if self.mode=='purge_error':raise OSError(SECRET)
    def write(self,data):self.writes.append(data);return len(data)
    def flush(self):pass
    def read_until(self,*args):return b''
    def close(self):self.closed=True

class Tests(unittest.TestCase):
    def drain(self,p,clock,**kwargs):
        sleeps=[]
        def sleep(seconds):sleeps.append(seconds);clock[0]+=seconds
        with patch.object(tool.time,'sleep',sleep):tool._drain_startup(p,clock=lambda:clock[0],**kwargs)
        return sleeps
    def test_under_cap_unchanged(self):
        c=[0.];p=Port(c,'under');self.assertEqual(self.drain(p,c),[]);self.assertEqual((p.reads,p.bytes,p.purges),(5,5,1))
    def test_exact_cap_waits_original_deadline_no_more_reads(self):
        c=[0.];p=Port(c);s=self.drain(p,c);self.assertEqual((p.reads,p.bytes,p.purges),(32,8192,1));self.assertEqual(len(s),1);self.assertAlmostEqual(c[0],5.);self.assertAlmostEqual(s[0],4.68)
    def test_no_extra_sleep_after_deadline(self):
        c=[0.];p=Port(c,'under');self.drain(p,c);self.assertEqual(c[0],5.)
    def test_native_read_error_stays_error(self):
        c=[0.];p=Port(c,'error')
        with self.assertRaises(tool.SetupError):self.drain(p,c)
        self.assertEqual(p.purges,0)
    def test_wrong_read_type_stays_error(self):
        c=[0.];p=Port(c,payload=SECRET)
        with self.assertRaises(tool.SetupError):self.drain(p,c)
        self.assertEqual(p.purges,0)
    def test_oversize_read_stays_error(self):
        c=[0.];p=Port(c,payload=b'x'*257)
        with self.assertRaises(tool.SetupError):self.drain(p,c)
        self.assertEqual(p.purges,0)
    def test_purge_error_stays_error(self):
        c=[0.];p=Port(c,'purge_error')
        with self.assertRaises(tool.SetupError):self.drain(p,c)
        self.assertEqual((p.bytes,p.purges),(8192,1))
    def test_optional_witness_cap_still_rejected(self):
        c=[0.];p=Port(c)
        with self.assertRaises(tool.SetupError):self.drain(p,c,phase_observer=lambda _:None)
        self.assertEqual((p.bytes,p.purges),(8192,0));self.assertLess(c[0],5.)
    def test_incomplete_witness_still_rejected(self):
        c=[0.];p=Port(c,'under',b'I (1) katmed_usb_phase: schema=1');rejected=[]
        with self.assertRaises(tool.SetupError):self.drain(p,c,phase_observer=lambda _:None,rejection_observer=rejected.append)
        self.assertIn('drain_incomplete',rejected);self.assertEqual(p.purges,0)
    def test_malformed_witness_still_rejected(self):
        c=[0.];p=Port(c,payload=b'I (1) katmed_usb_phase: broken\n')
        with self.assertRaises(tool.SetupError):self.drain(p,c,phase_observer=lambda _:None)
        self.assertEqual(p.purges,0)
    def test_valid_witness_unchanged(self):
        fields={k:0 for k in tool.USB_PHASE_KEYS};fields.update(schema=1,phase_seq=1,boot_domain=1)
        line=b'I (1) katmed_usb_phase: '+b' '.join(f'{k}={v}'.encode()for k,v in fields.items())+b'\n';c=[0.];p=Port(c,'under',line);seen=[];self.drain(p,c,phase_observer=seen.append);self.assertEqual(len(seen),5);self.assertEqual(p.purges,1)
    def test_unchanged_16_status_line_limit(self):
        class Lines:
            def __init__(self):self.n=0
            def read_until(self,*args):self.n+=1;return b'UNRELATED\n'
        p=Lines()
        with self.assertRaises(tool.SetupError):tool._read_protocol_line(p)
        self.assertEqual(p.n,16)
    def test_missing_STATUS_blocks_ARM_commit_and_closes(self):
        directory=D/'.local';directory.mkdir(mode=0o700,exist_ok=True)
        with tempfile.NamedTemporaryFile(mode='w',dir=directory,delete=False)as f:
            json.dump({'group':'22'*16,'secret':'11'*32,'ssid':'SYNTHETIC_NETWORK','password':'SYNTHETIC_PASSWORD','time_source':'synthetic-time'},f);path=Path(f.name)
        c=[0.];p=Port(c);args=types.SimpleNamespace(config=str(path),port='SYNTHETIC',expect_device='001122334455',expect_firmware='1111111',expect_build='222222222',expect_catalog='33'*32)
        captured=io.StringIO()
        try:
            with patch.object(tool.time,'sleep',lambda seconds:c.__setitem__(0,c[0]+seconds)),__import__('contextlib').redirect_stdout(captured),__import__('contextlib').redirect_stderr(captured):
                with self.assertRaises(tool.SetupError):tool.run_bound_provision(args,opener=lambda _:p,startup=lambda port:tool._drain_startup(port,clock=lambda:c[0]),nonce_factory=lambda:'44'*16)
        finally:path.unlink()
        self.assertTrue(p.closed);self.assertEqual(p.writes,[b'KM1 STATUS\n']);self.assertEqual(captured.getvalue(),'');self.assertNotIn(SECRET,captured.getvalue())
if __name__=='__main__':unittest.main()
