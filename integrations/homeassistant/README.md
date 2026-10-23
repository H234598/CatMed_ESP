# Katmed ESP: local read-only Home Assistant adapter

This is an implemented **HA-side candidate**, not an available feature of the
currently installed firmware. `/api/ha/status` is the agreed planned contract from the firmware owner, awaiting
its actual implementation and independent review. The current firmware has no such
endpoint. Do not deploy this candidate expecting the existing device to work.
An isolated Home Assistant Core 2026.10.2 lifecycle fixture passes; no live HA instance or device has been contacted.

## Scope

Copy `custom_components/katmed_esp` to HA's `custom_components` only after the
firmware contract and independent review are complete. Configure a literal device
IPv4 address, its existing web password, and optionally the path of a trusted CA
PEM file **inside HA's filesystem**. The existing web password must already have
been set with the on-display setup code. This integration never performs first
password setup, provisioning, reset, or medication acknowledgment.

TLS certificate/hostname validation is always enabled. The configured CA must trust the device certificate. The current firmware creates a DNS SAN (katmed-…local), not an IP SAN. Configure its independently verified certificate name as the TLS server name when connecting to the literal IPv4. This changes TLS SNI/hostname validation only, not HTTP Host/Origin or DNS routing. There is no `verify_ssl=false`
escape hatch. No credentials or actual deployment addresses are included here.
Passwords are stored in HA config entries like other local integrations; protect
HA's configuration and backups. Session cookies stay in volatile client memory.
Do not enable HTTP request/body debug logging around credentials.

A single shared 30-second coordinator poll supplies alarm, boot notice, severity,
next due and last dose entities. Unknown UTC is `null`, never a guessed date.
`last_dose_known` distinguishes a dose whose clock time is unknown from no known
dose. A transport, schema or identity failure makes coordinator entities
unavailable. Boot notices are not doses or confirmations.

The event entity exposes only an **observed local emergency-menu click** on the
separate large red `Alarm` button in `ALARM_MENU`, captioned
`Nur im Notfall. Drücken kostet!`. This is NOT medication acknowledgment in a red
alarm phase. Firmware owns counting one accepted local click once, excluding
long-press/swipe duplicate events. Medication acknowledgment, boot dismissal,
red rendering, clock changes and remote GETs must not increment this counter.
An emergency click must not change last dose, next due, pending medication or
history. The HA event type is `pressed`, never a dose `confirmed` event.

The first sample, an HA restart and a device boot establish a baseline and emit
no event. An increasing per-boot emergency sequence emits one observed edge;
a missed gap is not replayed as guessed multiple presses. Polling is best effort
and cannot guarantee every emergency click, exactly-once delivery or its exact
time. This integration is not an emergency transport guarantee. No HA service,
button or automation command sends a medication acknowledgment.

## Agreed planned snapshot v1 (endpoint not implemented yet)

Authenticated `GET /api/ha/status`, JSON at most 4096 bytes. It must be a
consistent read-only snapshot; it must not mutate Store or initiate an alarm.
The response contains exactly:

| Field | Meaning |
| --- | --- |
| `schema_version` | integer `1` |
| `device_id` | stable nonsecret identity, exactly 12 lowercase hex characters |
| `boot_id` | decimal string of a nonzero uint64 boot domain, equality only |
| `alarm_active` | boolean; corresponds to severity other than `none` |
| `alarm_severity` | `none`, `blue`, `yellow`, `red`; computed by product rules; `none` for boot notice |
| `boot_notice` | separate boolean; never a dose |
| `next_due` | UTC ISO8601 timestamp or null when unavailable |
| `last_dose` | UTC ISO8601 timestamp or null when unavailable |
| `last_dose_known` | boolean; false requires null `last_dose` |
| `emergency_button_sequence` | integer 0..2^53-1, incremented once per accepted local emergency-menu click within a boot |

No SSID, private group/key, medication history, certificate private material,
firmware digest, IP diagnostics or mutable configuration is requested. The
existing `POST /api/login` password session is reused with its correct Origin,
secure cookie, no redirects, strict bounded body and verified TLS. No new
firmware auth mechanism or MQTT library is proposed. HA performs one renewal
when the status session expires; this never retries a medical write.

## Verification and remaining work

Run only synthetic adapter tests:

```
python3 -B -m unittest discover -s tests/ha_integration -v
```

The tests exercise the actual parser, event cursor and async client using fake
HTTP responses, including fragmented bodies, oversize/redirect rejection,
strict identity/UTC/schema checks, cookie injection rejection and bounded auth
renewal. They do not start HA, contact a host or trigger an automation.

The standard HA config flow, coordinator and entity modules are included.
The actual loader, config-flow manager, seven entity platforms/states,
coordinator failure availability, emergency event, reauthentication/reload and
unload have been exercised against Home Assistant Core **2026.10.2**, Python
3.14.7, in an isolated private test environment. The test blocks socket connect,
DNS, bind and listen, replacing only the device client read with synthetic
snapshots. No HA server is started. Run with a compatible isolated interpreter:

```
python -B tests/ha_integration/core_compatibility.py
```

The fixture pins its expected Core version. The initial registry initialization
error was test setup only. An actual compatibility correction renamed the
reauth identity-mismatch translation key to `unique_id_mismatch`; the guard
already rejected the different device. No other product behavior changed.
The browser frontend, TLS handshake with a real device, actual firmware snapshot
and external automation delivery remain untested. Independent nonauthor review
is still required. No host packages were downgraded or changed, and no firmware
build was run. The exclusive author paths are this
folder and `tests/ha_integration/`; all firmware/shared endpoints belong to the
main owner.

Official sources consulted:

- https://contributors.home-assistant.io/core/integrations/fetching-data/
- https://contributors.home-assistant.io/core/integrations/config-flow/
- https://contributors.home-assistant.io/core/entities/event/
- https://contributors.home-assistant.io/core/integrations/manifest/
- https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32c3/api-reference/protocols/esp_https_server.html

The HA design uses the documented coordinator/config-flow/event entity patterns;
the ESP-IDF reference confirms reuse of the existing HTTPS server rather than a
second transport stack. No HA instance, HA version or broker metadata was found
in the approved local inventory filename search. A real deployment target and
trust configuration remain unknown.
