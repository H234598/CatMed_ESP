import importlib.util
from pathlib import Path
import unittest
ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location("usb",ROOT/"tools/katmed_usb_setup.py")
usb=importlib.util.module_from_spec(spec);spec.loader.exec_module(usb)
class NativeMetrics(unittest.TestCase):
 def test_native_diagnostic_form_is_bounded_and_distinguishes_missing_from_failure(self):
  line=b"I (10) katmed_metrics: uptime_ms=10 jpeg_detail_v=1 jpeg_seq=2 jpeg_stage=1 jpeg_result=5 jpeg_mcus=0 touch_init=4 touch_probe=1 touch_seq=3 touch_transfer=2 touch_sample=4"
  self.assertIsNotNone(usb._metric_line(line))
  for bad in (line.replace(b"jpeg_stage=1",b"jpeg_stage=5"),line.replace(b"jpeg_mcus=0",b"jpeg_mcus=1281"),line.replace(b"touch_sample=4",b"touch_sample=5"),line.replace(b"jpeg_detail_v=1",b"jpeg_detail_v=0"),line+b" private=value"):
   self.assertIsNone(usb._metric_line(bad))
  empty=b"I (10) katmed_metrics: uptime_ms=10 jpeg_detail_v=0 jpeg_seq=0 jpeg_stage=0 jpeg_result=0 jpeg_mcus=0 touch_init=0 touch_probe=0 touch_seq=0 touch_transfer=0 touch_sample=0"
  self.assertIsNotNone(usb._metric_line(empty))
if __name__=="__main__":unittest.main()
