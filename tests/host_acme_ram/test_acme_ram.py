#!/usr/bin/env python3
import json
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import katmed_acme_ram


PHASES = (
    "baseline", "initialization", "order", "challenge",
    "certificate", "error", "recovery", "retry",
)


def metric(uptime: int, minimum: int, largest: int, main: int, lvgl: int) -> str:
    return (
        "I (1) katmed_metrics: "
        f"uptime_ms={uptime} pack_hash_us=0 pack_valid=0 "
        f"variant_hash_us=0 variant_valid=0 jpeg_decode_us=0 jpeg_valid=0 "
        f"blink_cycle_us=0 blink_valid=0 blink_pixels=0 blink_segments=0 "
        f"blink_cycles=0 blink_changes=0 pack_status=0 variant_status=0 jpeg_status=0 "
        f"internal_min_free={minimum} "
        f"internal_largest_block={largest} main_stack_hwm_bytes={main} "
        f"lvgl_stack_hwm_bytes={lvgl} lvgl_stack_valid=1\n"
    )


def phase(sequence: int, name: str) -> str:
    return f"I (1) katmed_acme_phase: schema=1 phase_seq={sequence} phase={name}\n"


class AcmeRamBenchCheck(unittest.TestCase):
    def test_full_order_and_repeated_recovery_are_qualified(self):
        lines = []
        sequence = 1
        for repetition in range(2):
            for name in PHASES:
                lines.append(phase(sequence, name))
                lines.append(metric(
                    sequence * 1000,
                    64000 - repetition * 3000 - sequence * 100,
                    22000 - repetition * 500 - sequence * 50,
                    12000 - repetition * 100,
                    9000 - repetition * 75,
                ))
                sequence += 1
        summary = katmed_acme_ram.evaluate("".join(lines).encode("ascii"))
        self.assertEqual("qualified_host_synthetic", summary["qualification"])
        self.assertEqual(list(PHASES), summary["required_phases"])
        self.assertEqual(2, summary["repetitions"])
        self.assertEqual(64000 - 3000 - 1600, summary["internal_min_free_min"])
        self.assertEqual(22000 - 500 - 800, summary["internal_largest_block_min"])
        self.assertEqual(11900, summary["main_stack_hwm_min"])
        self.assertEqual(8925, summary["lvgl_stack_hwm_min"])

    def test_missing_phase_and_malformed_metric_fail_closed(self):
        text = phase(1, "baseline") + metric(1000, 1, 1, 1, 1)
        with self.assertRaises(katmed_acme_ram.RamBenchError):
            katmed_acme_ram.evaluate(text.encode("ascii"))
        malformed = phase(1, "baseline") + "I (1) katmed_metrics: uptime_ms=x\n"
        with self.assertRaises(katmed_acme_ram.RamBenchError):
            katmed_acme_ram.evaluate(malformed.encode("ascii"))

    def test_cli_writes_only_bounded_summary(self):
        text = phase(1, "baseline") + metric(1000, 100, 80, 60, 40)
        text += phase(2, "initialization") + metric(2000, 90, 70, 50, 30)
        with tempfile.TemporaryDirectory() as temporary:
            source = pathlib.Path(temporary) / "fixture.log"
            output = pathlib.Path(temporary) / "summary.json"
            source.write_text(text)
            result = subprocess.run(
                [sys.executable, str(ROOT / "tools/katmed_acme_ram.py"),
                 "--input", str(source), "--output", str(output)],
                text=True, capture_output=True, check=False,
            )
            self.assertNotEqual(0, result.returncode)
            self.assertFalse(output.exists())


def resource_record(cycle: int, seq: int, name: str, *, variant: str = "client",
                    status: str = "running", free: int = 64000) -> str:
    return (
        f"I (1) katmed_acme_resource: schema=2 variant={variant} cycle={cycle} "
        f"seq={seq} phase={name} status={status} uptime_ms={cycle * 100000 + seq} "
        f"internal_free={free} internal_min={free - 1000} "
        f"internal_largest={free - 2000} dma_free={free - 10000} "
        f"dma_min={free - 11000} dma_largest={free - 12000} monitor_valid=1\n"
    )


def with_measurements(raw: bytes) -> bytes:
    lines=[line for line in raw.decode().splitlines() if 'katmed_acme_resource:' in line]
    extra=[]
    for cycle in (1,2,3):
        samples=[]
        for line in lines:
            m=katmed_acme_ram._RESOURCE_RE.fullmatch(line.encode())
            if m and int(m[2])==cycle: samples.append((int(m[3]),m[4].decode(),int(m[6])*1000,m[1].decode()))
        if not samples: continue
        variant=samples[0][3]
        first,last=samples[0][2],samples[-1][2]
        cpu_times=[('before',first-12000002,first-2),('active',first-1,last+1),('after',last+2,last+12000002)]
        for name,start,end in cpu_times:
            extra.append(f'I (1) katmed_acme_cpu: schema=1 variant={variant} cycle={cycle} core=0 cores=1 window={name} start_us={start} end_us={end} idle_start={start//2} idle_end={end//2} idle_delta={end//2-start//2} elapsed_us={end-start} counter_bits=64 timer_hz=1000000 tickless=0 idle_priority_shared=1 valid=1')
        if variant=='baseline': continue
        active={};attempts={}
        def emit(op,event,seq,stamp,status):
            if event=='begin':
                attempts[op]=attempts.get(op,0)+1
                active[op]=(seq,stamp)
            startseq,start=active[op]
            extra.append(f'I (1) katmed_acme_window: schema=1 variant={variant} cycle={cycle} op={op} attempt={attempts[op]} event={event} status={status} start_seq={startseq} end_seq={seq} start_us={start} end_us={stamp} observations={1 if event=="begin" else 5} max_gap_us={0 if event=="begin" else stamp-start} period_us=100000 internal_min=50000 dma_min=40000 internal_block_observed=30000 dma_block_observed=20000 valid=1')
            if event=='end':del active[op]
        previous=None
        for seq,phase,stamp,_ in samples:
            for interval in ('key','connection'):
                if interval in active and phase != interval:emit(interval,'end',seq,stamp+500,'observed')
            if phase in ('key','connection') and phase not in active:emit(phase,'begin',seq,stamp,'running')
            if phase=='renewal' and 'renewal' not in active and 'renewal' not in attempts:emit('renewal','begin',seq,stamp,'running')
            if phase=='challenge' and 'challenge' not in active:
                if 'issuance' in active and previous:
                    emit('issuance','end',previous[0],previous[2]+500,'error')
                emit('challenge','begin',seq,stamp,'running')
            if phase=='issuance':
                if 'challenge' in active:emit('challenge','end',seq,stamp+500,'ok')
                if 'issuance' not in active:emit('issuance','begin',seq,stamp,'running')
            if phase=='certificate_download' and 'download' not in active and 'download' not in attempts:emit('download','begin',seq,stamp,'running')
            if phase=='certificate_switch':
                if 'download' in active and previous:emit('download','end',previous[0],previous[2]+500,'ok')
                for op in ('issuance','renewal'):
                    if op in active:emit(op,'end',seq,stamp+500,'ok')
            previous=(seq,phase,stamp)
    return ('\n'.join(lines+extra)+'\n').encode()


def resource_capture(variant: str = "client") -> bytes:
    lines = []
    for cycle in range(1, 4):
        names = (["idle", "key", "connection", "challenge", "issuance",
                  "certificate_download", "certificate_switch", "cleanup", "released"] if cycle == 1 else
                 ["idle", "renewal", "connection", "challenge", "issuance",
                  "certificate_download", "certificate_switch", "cleanup", "released"])
        if variant == "baseline":
            names = ["idle", "idle", "cleanup", "released"]
        for seq, name in enumerate(names, 1):
            lines.append(resource_record(
                cycle, seq, name, variant=variant,
                status="ok" if name == "released" else "running",
                free=64000 if name in ("idle", "cleanup", "released") else 60000,
            ))
    return with_measurements("".join(lines).encode("ascii"))


class AcmeResourceCaptureCheck(unittest.TestCase):
    def test_compiled_producer_field_order_matches_file_consumer(self):
        source = (ROOT / "firmware/components/katmed_acme/katmed_acme.cpp").read_text()
        start = source.index('"schema=2 variant=%s')
        end = source.index('monitor_valid=%u"', start) + len('monitor_valid=%u"')
        record_format = "".join(re.findall(r'"([^"]*)"', source[start:end]))
        self.assertEqual(
            ["schema", "variant", "cycle", "seq", "phase", "status", "uptime_ms",
             "internal_free", "internal_min", "internal_largest", "dma_free",
             "dma_min", "dma_largest", "monitor_valid"],
            re.findall(r"([a-z_]+)=", record_format),
        )

    def test_three_synthetic_cycles_are_only_source_qualified(self):
        summary = katmed_acme_ram.evaluate_resource(resource_capture(), "client")
        self.assertEqual("source_capture_valid", summary["qualification"])
        self.assertFalse(summary["hardware_pass"])
        self.assertEqual(3, summary["cycles"])
        self.assertEqual([], summary["missing_phases"])
        self.assertIn("certificate_switch remains separate", summary["certificate_file_boundary"])
        self.assertIn("synchronous deletion", summary["cleanup_boundary"])
        self.assertIn("snapshot", summary["largest_block_boundary"])
        baseline = katmed_acme_ram.evaluate_resource(resource_capture("baseline"), "baseline")
        self.assertEqual("source_capture_valid", baseline["qualification"])

    def test_missing_phase_is_reported_and_cli_cannot_pass(self):
        raw = resource_capture().replace(
            resource_record(2, 4, "challenge", free=60000).encode("ascii"), b""
        )
        # A capture with a missing numeric sequence is rejected before qualification.
        with self.assertRaises(katmed_acme_ram.RamBenchError):
            katmed_acme_ram.evaluate_resource(raw, "client")
        text = resource_capture().decode("ascii")
        text = with_measurements(text.replace("phase=challenge", "phase=connection", 1).encode()).decode()
        summary = katmed_acme_ram.evaluate_resource(text.encode("ascii"), "client")
        self.assertEqual("incomplete", summary["qualification"])
        self.assertIn("challenge", summary["missing_by_cycle"]["1"])
        with tempfile.TemporaryDirectory() as temporary:
            source = pathlib.Path(temporary) / "capture.log"
            output = pathlib.Path(temporary) / "summary.json"
            source.write_text(text)
            result = subprocess.run(
                [sys.executable, str(ROOT / "tools/katmed_acme_ram.py"),
                 "--mode", "resource", "--variant", "client", "--input", str(source),
                 "--output", str(output)], capture_output=True, text=True, check=False,
            )
            self.assertNotEqual(0, result.returncode)
            self.assertEqual("incomplete", json.loads(output.read_text())["qualification"])

    def test_error_abort_cleanup_loss_and_missing_cycle_fail_closed(self):
        raw = resource_capture()
        for damaged in (
            raw.replace(b"phase=released status=ok", b"phase=released status=error", 1),
            raw.replace(resource_record(1, 9, "released", status="ok", free=64000).encode("ascii"),
                        resource_record(1, 9, "released", status="ok", free=59000).encode("ascii"), 1),
            raw.split(b"cycle=3")[0],
        ):
            with self.assertRaises(katmed_acme_ram.RamBenchError):
                katmed_acme_ram.evaluate_resource(damaged, "client")

    def test_malformed_incomplete_and_wrong_variant_never_qualify(self):
        raw = resource_capture()
        for damaged in (
            raw.replace(b"monitor_valid=1", b"monitor_valid=0", 1),
            raw.replace(b"internal_free=64000", b"internal_free=x", 1),
            raw.replace(b"variant=client", b"variant=baseline", 1),
            raw.replace(b"seq=2", b"seq=4", 1),
            b"",
        ):
            with self.assertRaises(katmed_acme_ram.RamBenchError):
                katmed_acme_ram.evaluate_resource(damaged, "client")

    def test_peer_phase_order_and_cross_cycle_loss_fail_closed(self):
        lines = resource_capture().decode("ascii").splitlines(keepends=True)
        lines[4] = lines[4].replace("phase=issuance", "phase=certificate_switch")
        lines[5] = lines[5].replace("phase=certificate_switch", "phase=issuance")
        with self.assertRaises(katmed_acme_ram.RamBenchError):
            katmed_acme_ram.evaluate_resource("".join(lines).encode("ascii"), "client")

        raw = resource_capture()
        for cycle in (2, 3):
            for seq, name in ((1, "idle"), (7, "cleanup")):
                old = resource_record(cycle, seq, name, status="ok" if seq == 7 else "running")
                new = resource_record(cycle, seq, name,
                                      status="ok" if seq == 7 else "running",
                                      free=64000 - (cycle - 1) * 1000)
                raw = raw.replace(old.encode("ascii"), new.encode("ascii"), 1)
        with self.assertRaises(katmed_acme_ram.RamBenchError):
            katmed_acme_ram.evaluate_resource(raw, "client")

    def test_positive_heap_cannot_have_zero_largest_block(self):
        raw = resource_capture()
        for damaged in (
            raw.replace(b"internal_largest=62000", b"internal_largest=0", 1),
            raw.replace(b"dma_largest=52000", b"dma_largest=0", 1),
        ):
            with self.assertRaises(katmed_acme_ram.RamBenchError):
                katmed_acme_ram.evaluate_resource(damaged, "client")


class AcmeSourceContractCheck(unittest.TestCase):
    def test_http01_route_registration_delivery_failure_and_cleanup(self):
        with tempfile.TemporaryDirectory() as temporary:
            binary = pathlib.Path(temporary) / "route_test"
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
                "-I", str(ROOT / "firmware/components/katmed_web/include"),
                str(ROOT / "firmware/components/katmed_web/katmed_web_acme_route.c"),
                str(ROOT / "tests/host_acme_ram/test_acme_route.c"),
                "-o", str(binary),
            ], check=True, capture_output=True, text=True)
            subprocess.run([str(binary)], check=True, capture_output=True, text=True)
        web = (ROOT / "firmware/components/katmed_web/katmed_web.c").read_text()
        self.assertIn("plain.max_uri_handlers = 2;", web)
        self.assertIn("plain.uri_match_fn = katmed_web_plain_uri_match;", web)
        self.assertIn('if (!web.https || !request_host(req, host))', web)

    def test_atomic_close_failures_preserve_old_certificate(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            for name, content in {
                "esp_log.h": "#define ESP_LOGE(tag, ...) ((void)(tag))\n",
                "freertos/FreeRTOS.h": "",
                "freertos/portmacro.h": (
                    "typedef int portMUX_TYPE;\n"
                    "#define portMUX_INITIALIZER_UNLOCKED 0\n"
                    "#define portENTER_CRITICAL(x) ((void)(x))\n"
                    "#define portEXIT_CRITICAL(x) ((void)(x))\n"
                ),
                "katmed_source_export.h": (
                    "#include <stdbool.h>\n"
                    "bool katmed_source_writer_enter(void);\n"
                    "void katmed_source_writer_leave(void);\n"
                ),
            }.items():
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content)
            obj = root / "atomic.o"
            binary = root / "atomic_test"
            result = subprocess.run([
                "cc", "-D_GNU_SOURCE", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-I", str(root), "-Dfsync=fixture_fsync", "-Drename=fixture_rename",
                "-c", str(ROOT / "firmware/components/katmed_acme/katmed_acme_atomic.c"),
                "-o", str(obj),
            ], capture_output=True, text=True)
            self.assertEqual(0, result.returncode, result.stderr)
            subprocess.run([
                "cc", "-D_GNU_SOURCE", "-std=c11", "-Wall", "-Wextra", "-Werror",
                str(obj), str(ROOT / "tests/host_acme_ram/test_acme_atomic.c"),
                "-o", str(binary),
            ], check=True, capture_output=True, text=True)
            subprocess.run([str(binary), str(root / "certificate.pem")],
                           check=True, capture_output=True, text=True)

    def test_resolved_client_patch_is_idempotent_and_propagates_failure(self):
        sys.path.insert(0, str(ROOT / "tools"))
        import prepare_acme_component as patch
        source = ROOT / "firmware/managed_components/dannybackx__acmeclient"
        with tempfile.TemporaryDirectory() as temporary:
            target = pathlib.Path(temporary)
            for name in (*patch.EXPECTED, "idf_component.yml", "include/Acme.h"):
                (target / name).parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source / name, target / name)
            patch.apply(target)
            self.assertFalse(patch.apply(target))
            text = (target / "Acme.cpp").read_text()
            self.assertIn("!katmed_acme_http_host_allowed(req))", text)
            self.assertIn("if (!ws_registered) {", text)
            self.assertIn("free((void *)wsconf.uri);", text)
            self.assertIn("int committed = ok ? katmed_acme_atomic_close(f) : katmed_acme_atomic_abort(f);", text)
            self.assertIn("if (committed != 0)", text)
            self.assertIn("if (ok) ReadCertificate();", text)


if __name__ == "__main__":
    unittest.main(verbosity=2)
