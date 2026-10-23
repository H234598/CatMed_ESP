"""Synthetic bounded adapter checks: no HA instance, socket, or credentials."""
import asyncio
import importlib.util
import json
from pathlib import Path
import sys
import types
import unittest

ROOT = Path(__file__).resolve().parents[2]
PACKAGE = ROOT / 'integrations/homeassistant/custom_components/katmed_esp'
pkg = types.ModuleType('katmed_test_adapter')
pkg.__path__ = [str(PACKAGE)]
sys.modules[pkg.__name__] = pkg

def load(name):
    spec = importlib.util.spec_from_file_location(f'{pkg.__name__}.{name}', PACKAGE / f'{name}.py')
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module

protocol = load('protocol')
client = load('client')

def sample(**changes):
    return dict(schema_version=1, device_id='000000000001', boot_id='1',
                alarm_active=True, alarm_severity='blue', boot_notice=False,
                next_due=None, last_dose=None, last_dose_known=False,
                emergency_button_sequence=0, **changes) if not changes else {**sample(), **changes}

class ProtocolTests(unittest.TestCase):
    def test_unknown_dates_and_boot_are_not_dose(self):
        value = protocol.Snapshot.parse(sample(boot_notice=True, alarm_active=False, alarm_severity="none"))
        self.assertIsNone(value.next_due)
        self.assertIsNone(value.last_dose)
        self.assertFalse(value.last_dose_known)

    def test_complete_strict_shape(self):
        for bad in (sample(schema_version=True), sample(alarm_active=1),
                    sample(emergency_button_sequence=True), sample(emergency_button_sequence=-1),
                    sample(alarm_severity='orange'), sample(alarm_active=False),
                    sample(last_dose='2026-10-23T01:00:00Z'),
                    sample(next_due='2026-10-23T01:00:00'),
                    sample(next_due='2026-10-23T01:00:00+02:00'),
                    sample(device_id='bad\nidentity'), sample(device_id='ABCDEF000001'),
                    sample(boot_id='0'), sample(boot_id='18446744073709551616'),
                    sample(boot_notice=True), {**sample(), 'secret': 'not allowed'}):
            with self.subTest(case=list(bad.keys())):
                with self.assertRaises(protocol.InvalidSnapshot):
                    protocol.Snapshot.parse(bad)
        short = sample(); short.pop('boot_id')
        with self.assertRaises(protocol.InvalidSnapshot):
            protocol.Snapshot.parse(short)

    def test_known_dose_can_have_unknown_utc(self):
        self.assertTrue(protocol.Snapshot.parse(sample(last_dose_known=True)).last_dose_known)
        value = protocol.Snapshot.parse(sample(last_dose_known=True, last_dose='2026-10-23T01:00:00Z'))
        self.assertEqual(value.last_dose.utcoffset().total_seconds(), 0)

    def test_event_baseline_duplicate_restart_gap_and_regression(self):
        cursor = protocol.EventCursor()
        accept = lambda **kw: cursor.accept(protocol.Snapshot.parse(sample(**kw)))
        self.assertFalse(accept(emergency_button_sequence=4))
        self.assertFalse(accept(emergency_button_sequence=4))
        self.assertTrue(accept(emergency_button_sequence=7))  # one observed edge, no invented 3 events
        self.assertFalse(accept(boot_id='2', emergency_button_sequence=0))
        self.assertTrue(accept(boot_id='2', emergency_button_sequence=1))
        with self.assertRaises(protocol.InvalidSnapshot):
            accept(boot_id='2', emergency_button_sequence=0)
        with self.assertRaises(protocol.InvalidSnapshot):
            accept(device_id='000000000002')

    def test_old_ack_counter_is_not_a_supported_alias(self):
        old = sample()
        old["red_button_sequence"] = old.pop("emergency_button_sequence")
        for payload in (old, {**sample(), "red_button_sequence": 1}):
            with self.assertRaises(protocol.InvalidSnapshot):
                protocol.Snapshot.parse(payload)

    def test_medication_boot_clock_or_color_changes_are_not_emergency(self):
        cursor = protocol.EventCursor()
        self.assertFalse(cursor.accept(protocol.Snapshot.parse(sample())))
        for changes in (
            {"alarm_severity": "red"},
            {"last_dose_known": True, "last_dose": "2026-10-23T01:00:00Z", "alarm_active": False, "alarm_severity": "none"},
            {"next_due": "2026-10-23T09:00:00Z"},
            {"boot_notice": True, "alarm_active": False, "alarm_severity": "none"},
        ):
            self.assertFalse(cursor.accept(protocol.Snapshot.parse(sample(**changes))))
        # The independent emergency counter can advance without a medication alarm.
        self.assertTrue(cursor.accept(protocol.Snapshot.parse(sample(
            emergency_button_sequence=1, alarm_active=False, alarm_severity="none"))))

    def test_no_hostname_url_or_redirect_target_input(self):
        self.assertEqual(protocol.endpoint('192.0.2.1'), 'https://192.0.2.1')
        for host in ('http://192.0.2.1', '192.0.2.1/path', 'user@host', 'host.local', '::1'):
            with self.assertRaises(ValueError): protocol.endpoint(host)

class Content:
    def __init__(self, raw): self.raw = raw
    async def read(self, limit):
        # Deliberately fragmented, including otherwise perfectly valid JSON.
        count = min(limit, 17)
        chunk, self.raw = self.raw[:count], self.raw[count:]
        return chunk

class Response:
    def __init__(self, data=None, status=200, cookie=None, raw=None, content_type='application/json'):
        self.status = status; self.content_type = content_type
        self.content = Content(raw if raw is not None else json.dumps(data).encode())
        self.cookies = {} if cookie is None else {'katmed_session': types.SimpleNamespace(value=cookie)}
    async def __aenter__(self): return self
    async def __aexit__(self, *args): pass

class Session:
    def __init__(self, responses): self.responses = iter(responses); self.calls = []
    def request(self, method, url, **kwargs):
        self.calls.append((method, url, kwargs))
        return next(self.responses)

LOGIN = {'authenticated': True, 'setup_required': False}

class ClientTests(unittest.IsolatedAsyncioTestCase):
    async def test_login_status_and_tls_no_medical_call(self):
        session = Session([Response(LOGIN, cookie='a'*64), Response(sample())])
        context = object()
        value = await client.KatmedClient(session, '192.0.2.1', 'synthetic-password', context, 'katmed-synthetic.local').read()
        self.assertFalse(value.boot_notice)
        self.assertEqual([(m, u.rsplit('/', 1)[-1]) for m,u,_ in session.calls], [('POST','login'), ('GET','status')])
        for _, _, kw in session.calls:
            self.assertIs(kw['ssl'], context)
            self.assertEqual(kw['server_hostname'], 'katmed-synthetic.local')
            self.assertFalse(kw['allow_redirects'])
            self.assertEqual(kw['headers']['Origin'], 'https://192.0.2.1')
        self.assertEqual(session.calls[1][2]['headers']['Cookie'], 'katmed_session='+'a'*64)

    async def test_one_expired_session_renewal(self):
        session = Session([Response(LOGIN,cookie='a'*64),Response(status=401),Response(LOGIN,cookie='b'*64),Response(sample())])
        await client.KatmedClient(session,'192.0.2.1','synthetic',object()).read()
        self.assertEqual(len(session.calls), 4)

    async def test_unauthorized_stops_after_single_renewal(self):
        session = Session([Response(LOGIN,cookie='a'*64),Response(status=401),Response(LOGIN,cookie='b'*64),Response(status=403)])
        with self.assertRaises(client.InvalidAuth):
            await client.KatmedClient(session,'192.0.2.1','synthetic',object()).read()
        self.assertEqual(len(session.calls),4)

    async def test_reject_oversize_redirect_wrong_type_and_invalid_schema(self):
        for response in (Response(raw=b'x'*4097),Response(status=302),Response(sample(),content_type='text/html'),Response({'bad':'reply'}), Response(raw=b'{"schema_version":1,"schema_version":1}')):
            session=Session([Response(LOGIN,cookie='a'*64),response])
            with self.assertRaises(client.CannotConnect):
                await client.KatmedClient(session,'192.0.2.1','synthetic',object()).read()

    async def test_login_does_not_accept_missing_or_injected_cookie(self):
        for cookie in (None,'a'*63,'a'*65,'a'*63+'\n'):
            session=Session([Response(LOGIN,cookie=cookie)])
            with self.assertRaises(client.InvalidAuth):
                await client.KatmedClient(session,'192.0.2.1','synthetic',object()).read()

    async def test_invalid_tls_names_rejected_without_request(self):
        for name in ('https://host', 'host/path', 'host\nheader', '*.local', '-bad.local'):
            session = Session([])
            with self.assertRaises(ValueError):
                client.KatmedClient(session,'192.0.2.1','synthetic',object(), name)
            self.assertFalse(session.calls)

    async def test_login_requires_authenticated_not_first_setup(self):
        session=Session([Response({'authenticated':False,'setup_required':True},cookie='a'*64)])
        with self.assertRaises(client.InvalidAuth):
            await client.KatmedClient(session,'192.0.2.1','synthetic',object()).read()

class EventEntityTests(unittest.TestCase):
    def test_actual_event_handler_emits_pressed_only_and_never_on_failure(self):
        # Exercise the real handler with minimal HA API doubles; not an HA runtime test.
        from unittest.mock import patch
        coordinator_api = types.ModuleType('homeassistant.helpers.update_coordinator')
        class CoordinatorEntity:
            def __init__(self, coordinator): self.coordinator = coordinator
            def async_write_ha_state(self): pass
        coordinator_api.CoordinatorEntity = CoordinatorEntity
        entity_api = types.ModuleType('homeassistant.helpers.entity')
        entity_api.DeviceInfo = dict
        event_api = types.ModuleType('homeassistant.components.event')
        class EventEntity:
            def _trigger_event(self, kind): self.observed.append(kind)
        event_api.EventEntity = EventEntity
        event_api.EventDeviceClass = types.SimpleNamespace(BUTTON='button')
        core_api = types.ModuleType('homeassistant.core')
        core_api.callback = lambda fn: fn
        modules = {'homeassistant.helpers.update_coordinator': coordinator_api,
                   'homeassistant.helpers.entity': entity_api,
                   'homeassistant.components.event': event_api, 'homeassistant.core': core_api}
        with patch.dict(sys.modules, modules):
            load('const'); load('entity'); events = load('event')
            coordinator = types.SimpleNamespace(device_id='000000000001', event_generation=0,
                                                last_update_success=True)
            entity = events.KatmedEmergencyButton(coordinator); entity.observed = []
            entity._handle_coordinator_update()
            self.assertEqual(entity.observed, [])
            coordinator.event_generation = 1
            entity._handle_coordinator_update(); entity._handle_coordinator_update()
            self.assertEqual(entity.observed, ['pressed'])
            self.assertEqual(entity._attr_event_types, ['pressed'])
            coordinator.event_generation = 2; coordinator.last_update_success = False
            entity._handle_coordinator_update()
            self.assertEqual(entity.observed, ['pressed'])

if __name__ == '__main__': unittest.main()
