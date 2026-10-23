"""Actual Core lifecycle fixture. Run in isolated HA2026.10.2 environment only.

No HA server is started. Device HTTP is replaced at the client boundary;
outbound connect/DNS operations are forbidden. Existing adapter tests remain separate.
"""
import asyncio
from dataclasses import replace
from datetime import datetime, timezone
from importlib.metadata import version
from pathlib import Path
import shutil
import socket
import sys
import tempfile
import unittest
from unittest.mock import AsyncMock, patch

from homeassistant import config_entries, loader
from homeassistant.core import HomeAssistant
from homeassistant.helpers import area_registry as ar, device_registry as dr, entity_registry as er

SOURCE = Path(__file__).resolve().parents[2] / 'integrations/homeassistant/custom_components'

class ActualCoreCompatibility(unittest.IsolatedAsyncioTestCase):
    async def test_actual_configflow_entities_events_failure_reauth_and_unload(self):
        self.assertEqual(version('homeassistant'), '2026.10.2')
        with tempfile.TemporaryDirectory(prefix='katmed-ha-core-') as directory:
            shutil.copytree(SOURCE, Path(directory) / 'custom_components')
            sys.path.insert(0, directory)
            hass = HomeAssistant(directory)
            hass.config.skip_pip = True
            loader.async_setup(hass)
            hass.config_entries = config_entries.ConfigEntries(hass, {})
            dr.async_setup(hass)
            await ar.async_load(hass)
            await dr.async_load(hass)
            await er.async_load(hass)
            from custom_components.katmed_esp.client import KatmedClient, CannotConnect, InvalidAuth
            from custom_components.katmed_esp.protocol import Snapshot

            snapshot = Snapshot.parse({
                'schema_version': 1, 'device_id': '000000000001', 'boot_id': '1',
                'alarm_active': False, 'alarm_severity': 'none', 'boot_notice': True,
                'next_due': None, 'last_dose': None, 'last_dose_known': False,
                'emergency_button_sequence': 0,
            })
            data = {'host': '192.0.2.1', 'password': 'synthetic-only',
                    'tls_server_name': 'katmed-synthetic.local'}
            def no_network(*args, **kwargs):
                raise AssertionError('Live connection or DNS forbidden in Core fixture')

            try:
                with patch.object(socket.socket, 'connect', no_network), \
                     patch.object(socket.socket, 'connect_ex', no_network), \
                     patch.object(socket.socket, 'bind', no_network), \
                     patch.object(socket.socket, 'listen', no_network), \
                     patch.object(socket, 'create_connection', no_network), \
                     patch.object(socket, 'getaddrinfo', no_network), \
                     patch.object(KatmedClient, 'read', AsyncMock(return_value=snapshot)) as read:
                    # Real loader, flow manager, validation, entry setup and platforms.
                    form = await hass.config_entries.flow.async_init('katmed_esp', context={'source': 'user'})
                    self.assertEqual(form['type'], 'form')
                    self.assertEqual(form['step_id'], 'user')
                    read.side_effect = InvalidAuth
                    rejected = await hass.config_entries.flow.async_configure(form['flow_id'], data)
                    self.assertEqual(rejected['errors'], {'base': 'invalid_auth'})
                    read.side_effect = None
                    created = await hass.config_entries.flow.async_configure(form['flow_id'], data)
                    self.assertEqual(created['type'], 'create_entry')
                    entry = created['result']
                    await hass.async_block_till_done()
                    self.assertEqual(entry.state, config_entries.ConfigEntryState.LOADED)
                    self.assertEqual(len(hass.states.async_all()), 7)
                    coordinator, session = hass.data['katmed_esp'][entry.entry_id]
                    states = {state.entity_id: state for state in hass.states.async_all()}
                    event_id = next(key for key in states if key.startswith('event.'))
                    alarm_id = next(key for key in states if key.endswith('_alarm') and key.startswith('binary_sensor.'))
                    last_id = next(key for key in states if key.endswith('_last_dose') and key.startswith('sensor.'))
                    self.assertEqual(hass.states.get(event_id).state, 'unknown')
                    self.assertEqual(hass.states.get(last_id).state, 'unknown')
                    self.assertEqual(hass.states.get(alarm_id).state, 'off')

                    duplicate = await hass.config_entries.flow.async_init('katmed_esp', context={'source': 'user'})
                    duplicate = await hass.config_entries.flow.async_configure(duplicate['flow_id'], data)
                    self.assertEqual(duplicate['type'], 'abort')
                    self.assertEqual(duplicate['reason'], 'already_configured')

                    # No emergency from medical ACK, rendering, or clock-only state.
                    medical = replace(snapshot, boot_notice=False, last_dose_known=True,
                                      last_dose=datetime(2026, 10, 23, tzinfo=timezone.utc))
                    read.return_value = medical
                    await coordinator.async_refresh(); await hass.async_block_till_done()
                    self.assertEqual(hass.states.get(event_id).state, 'unknown')
                    read.return_value = replace(medical, alarm_active=True, alarm_severity='red')
                    await coordinator.async_refresh(); await hass.async_block_till_done()
                    self.assertEqual(hass.states.get(event_id).state, 'unknown')
                    emergency = replace(medical, emergency_button_sequence=1)
                    read.return_value = emergency
                    await coordinator.async_refresh(); await hass.async_block_till_done()
                    first_event = hass.states.get(event_id)
                    self.assertEqual(first_event.attributes['event_type'], 'pressed')
                    await coordinator.async_refresh(); await hass.async_block_till_done()
                    self.assertEqual(hass.states.get(event_id).state, first_event.state)
                    read.return_value = replace(emergency, boot_id='2', emergency_button_sequence=0)
                    await coordinator.async_refresh(); await hass.async_block_till_done()
                    self.assertEqual(hass.states.get(event_id).state, first_event.state)

                    # Real coordinator/platform availability propagation, not stale green.
                    read.side_effect = CannotConnect
                    await coordinator.async_refresh(); await hass.async_block_till_done()
                    self.assertTrue(all(s.state == 'unavailable' for s in hass.states.async_all()))
                    read.side_effect = None
                    read.return_value = replace(emergency, boot_id='2', emergency_button_sequence=0)
                    await coordinator.async_refresh(); await hass.async_block_till_done()
                    self.assertNotEqual(hass.states.get(alarm_id).state, 'unavailable')

                    read.return_value = replace(emergency, device_id='000000000002')
                    await coordinator.async_refresh(); await hass.async_block_till_done()
                    self.assertTrue(all(s.state == 'unavailable' for s in hass.states.async_all()))
                    wrong = await hass.config_entries.flow.async_init('katmed_esp',
                        context={'source': 'reauth', 'entry_id': entry.entry_id}, data=dict(entry.data))
                    wrong = await hass.config_entries.flow.async_configure(wrong['flow_id'], {'password': 'not-saved'})
                    self.assertEqual(wrong['type'], 'abort')
                    self.assertEqual(wrong['reason'], 'unique_id_mismatch')
                    self.assertEqual(entry.data['password'], 'synthetic-only')
                    read.return_value = replace(emergency, boot_id='2', emergency_button_sequence=0)

                    # Real reauth flow updates the same entry and reloads; no duplicate.
                    reauth = await hass.config_entries.flow.async_init('katmed_esp',
                        context={'source': 'reauth', 'entry_id': entry.entry_id}, data=dict(entry.data))
                    self.assertEqual(reauth['step_id'], 'reauth_confirm')
                    read.side_effect = InvalidAuth
                    denied = await hass.config_entries.flow.async_configure(reauth['flow_id'], {'password': 'synthetic-new'})
                    self.assertEqual(denied['errors'], {'base': 'invalid_auth'})
                    read.side_effect = None
                    result = await hass.config_entries.flow.async_configure(reauth['flow_id'], {'password': 'synthetic-new'})
                    self.assertEqual(result['type'], 'abort')
                    self.assertEqual(result['reason'], 'reauth_successful')
                    await hass.async_block_till_done()
                    self.assertEqual(len(hass.config_entries.async_entries('katmed_esp')), 1)
                    self.assertEqual(entry.data['password'], 'synthetic-new')
                    self.assertTrue(session.closed)
                    _, current_session = hass.data['katmed_esp'][entry.entry_id]
                    self.assertTrue(await hass.config_entries.async_unload(entry.entry_id))
                    await hass.async_block_till_done()
                    self.assertTrue(current_session.closed)
                    self.assertNotIn(entry.entry_id, hass.data['katmed_esp'])
                    self.assertFalse(any(s.state != 'unavailable' for s in hass.states.async_all()))
            finally:
                await hass.async_stop(force=True)
                sys.path.remove(directory)

if __name__ == '__main__':
    unittest.main(verbosity=2)
