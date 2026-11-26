"""Offline boundary tests for the i18n development tools."""
import hashlib
import http.client
import importlib.util
import io
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'tools' / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


font = load('build_i18n_font')
draft = load('i18n_translate_draft')


class FontCacheTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.cache = Path(self.temp.name) / 'cache' / 'font.otf'
        self.content = b'test font data'
        for name, value in [('CJK', self.cache), ('CJK_SIZE', len(self.content)),
                            ('CJK_SHA256', hashlib.sha256(self.content).hexdigest())]:
            context = patch.object(font, name, value)
            context.start()
            self.addCleanup(context.stop)

    def download(self, content):
        return patch.object(font, 'urlopen', return_value=io.BytesIO(content))

    def test_valid_download_and_existing_cache(self):
        with self.download(self.content):
            font.ensure_cjk()
        self.assertEqual(self.cache.read_bytes(), self.content)
        with patch.object(font, 'urlopen', side_effect=AssertionError('unexpected download')):
            font.ensure_cjk()
        self.assertEqual(list(self.cache.parent.iterdir()), [self.cache])

    def test_invalid_downloads_are_removed(self):
        for content in (self.content[:-1], self.content + b'x', b'x' * len(self.content)):
            with self.subTest(length=len(content)), self.download(content):
                with self.assertRaises(RuntimeError):
                    font.ensure_cjk()
                self.assertFalse(self.cache.exists())
                self.assertEqual(list(self.cache.parent.iterdir()), [])

    def test_interrupted_download_is_removed(self):
        class Interrupted(io.BytesIO):
            def read(self, size=-1):
                raise OSError('interrupted')
        with patch.object(font, 'urlopen', return_value=Interrupted()):
            with self.assertRaises(OSError):
                font.ensure_cjk()
        self.assertEqual(list(self.cache.parent.iterdir()), [])

    def test_invalid_existing_cache_is_preserved(self):
        self.cache.parent.mkdir()
        self.cache.write_bytes(b'bad')
        with patch.object(font, 'urlopen', side_effect=AssertionError('unexpected download')):
            with self.assertRaises(RuntimeError):
                font.ensure_cjk()
        self.assertEqual(self.cache.read_bytes(), b'bad')

    def test_existing_and_dangling_symlinks_are_rejected(self):
        self.cache.parent.mkdir()
        target = Path(self.temp.name) / 'target'
        for present in (False, True):
            if present:
                target.write_bytes(self.content)
            self.cache.symlink_to(target)
            with self.assertRaisesRegex(RuntimeError, 'symlink'):
                font.ensure_cjk()
            self.assertTrue(self.cache.is_symlink())
            self.cache.unlink()


class TranslationTests(unittest.TestCase):
    def run_response(self, payload, batch):
        with patch.object(draft.urllib.request, 'urlopen', return_value=io.BytesIO(payload)) as call, \
                patch.object(draft.time, 'sleep'):
            result = draft.translate(batch, 'en')
        return result, call.call_count

    def test_valid_separator_placeholder_and_line_break(self):
        result = 'hello %s' + draft.SEPARATOR + 'two' + draft.BREAK + 'lines'
        payload = json.dumps([[[result]]]).encode()
        self.assertEqual(self.run_response(payload, ['hallo %s', 'zwei\nZeilen'])[0],
                         ['hello %s', 'two\nlines'])

    def test_invalid_responses_retry_without_exposing_payload(self):
        marker = 'private-response-marker'
        cases = [b'\xff' + marker.encode(), b'not-json ' + marker.encode(), b'{}', b'[[]]',
                 json.dumps([[[42]]]).encode(), b'x' * (draft.MAX_RESPONSE_BYTES + 1),
                 json.dumps([[[marker]]]).encode()]
        for payload in cases:
            with self.subTest(length=len(payload)), \
                    patch.object(draft.urllib.request, 'urlopen', side_effect=lambda *a, payload=payload, **k: io.BytesIO(payload)) as call, \
                    patch.object(draft.time, 'sleep') as sleep:
                with self.assertRaises(ValueError) as error:
                    draft.translate(['source %s', 'other'], 'en')
                self.assertNotIn(marker, str(error.exception))
                self.assertEqual(call.call_count, 4)
                self.assertEqual(sleep.call_count, 3)

    def test_placeholder_mismatch_retries(self):
        payload = json.dumps([[['hello']]]).encode()
        with patch.object(draft.urllib.request, 'urlopen', side_effect=lambda *a, **k: io.BytesIO(payload)) as call, \
                patch.object(draft.time, 'sleep'):
            with self.assertRaisesRegex(ValueError, 'placeholder'):
                draft.translate(['hallo %s'], 'en')
        self.assertEqual(call.call_count, 4)

    def test_incomplete_response_retries_and_does_not_expose_partial_data(self):
        class Interrupted(io.BytesIO):
            def read(self, size=-1):
                raise http.client.IncompleteRead(b'private-partial-response', 10)

        valid = json.dumps([[['hello']]]).encode()
        with patch.object(draft.urllib.request, 'urlopen',
                          side_effect=[Interrupted(), io.BytesIO(valid)]) as call, \
                patch.object(draft.time, 'sleep') as sleep:
            self.assertEqual(draft.translate(['hallo'], 'en'), ['hello'])
            self.assertEqual(call.call_count, 2)
            self.assertEqual(sleep.call_count, 1)

        with patch.object(draft.urllib.request, 'urlopen',
                          side_effect=lambda *a, **k: Interrupted()) as call, \
                patch.object(draft.time, 'sleep') as sleep:
            with self.assertRaisesRegex(ValueError, '^incomplete translation response$') as error:
                draft.translate(['hallo'], 'en')
            self.assertNotIn('private-partial-response', str(error.exception))
            self.assertEqual(call.call_count, 4)
            self.assertEqual(sleep.call_count, 3)


if __name__ == '__main__':
    unittest.main()
