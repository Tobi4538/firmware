# Meshtastic BLE client for T-Embed — experimental preparation

Based on `Tobi4538/firmware`, `firmware-dev` at
`f4a415bca2893a038b59599eb2a9d34fc418bdc5` (2026-09-27).
Existing board, sleep and Wi-Fi modifications are preserved.

## Intended connection

T-Embed (Bruce UI) → Bluetooth LE → T1000-E (Meshtastic) → LoRa mesh.
The T-Embed needs no LoRa add-on for this mode. Bruce's existing raw LoRa
chat is separate and is not a Meshtastic client. The T-Embed is a terminal
for the connected node, not an additional independently reachable mesh node.
Both devices must remain in Bluetooth range.

## Build and entry point

The feature is opt-in; regular Bruce build environments do not expose it.

```
pio run -e lilygo-t-embed-cc1101-meshtastic
pio run -e lilygo-t-embed-meshtastic
```

Choose the environment matching the physical board before flashing. The branch
contains GitHub Actions builds for both variants and portable protocol tests.
A successful compile is not a successful hardware test. Do not install a binary
until its build result, board variant and existing firmware backup are checked.

Open **Bluetooth → Meshtastic BLE**. End any other Bruce Bluetooth tool first.
Power the T1000-E, enable its Bluetooth, and disconnect the phone app if needed.
Select the advertised device, then enter its actual six-digit Bluetooth PIN.
Set region/modem/channels on the T1000-E using the official app beforehand.
This client reads these settings; it does not change them.

## Implemented scope

- Service UUID discovery, preserving BLE public/random address type.
- Pairing via NimBLE 2.5; PIN collected on the UI task before connecting.
- Config synchronization with a fresh nonzero nonce and a 60-second timeout.
- Bounded FIFO drain, FromNum notifications, fallback polling, heartbeats.
- Local node ID, enabled channels, existing hop limit and transmit setting.
- Text reception with node IDs and bounded RAM-only history; full-line view.
- Channel broadcasts up to 180 UTF-8 bytes with explicit send confirmation.
- BLE handoff is labelled as such; it is never represented as mesh delivery.
- No automatic resend after an ambiguous write; reconnect is a menu action.
- UTF-8 validation, bounded parsing, unknown-field skipping and RX deduplication.
- No retained channel keys, message files, PIN logs or configuration writes.
- All owned clients and BLE resources are released when leaving the tool.

This first version does not implement direct messages, node-name lists, position
maps, delivery acknowledgements, automatic reconnect, background reception after
leaving the menu, or a persistent conversation database. During the keyboard
and modal menus the radio retains incoming messages in its own finite queue;
the client drains that queue after returning. Messages may be lost if it fills.
Only supported text messages are displayed; telemetry/positions are skipped.
UTF-8 validity does not imply all glyphs are available in the Bruce display font.

## Checks without hardware

```
g++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -g -Isrc \
  tests/meshtastic/test_mesh_wire.cpp src/modules/meshtastic/mesh_wire.cpp \
  -o /tmp/mesh-wire-test
/tmp/mesh-wire-test
```

Tests use independent golden protobuf wire vectors for sends, incoming text,
config/nonce, channels (including skipped PSK), LoRa config and queue errors;
they cover truncation, malformed wire types/lengths, UTF-8, payload bounds,
unknown fields, and 20,000 deterministic malformed frames under sanitizers.

## Hardware acceptance checklist (pending T1000-E arrival)

1. Confirm exact T-Embed variant and current Bruce version; retain a known-good
   firmware and configuration backup before any installation.
2. Verify idle scan, no-device outcome, cancellation and normal menu return.
3. Pair with the correct PIN; verify a wrong PIN fails cleanly without reboot.
4. Confirm local ID, enabled channels, TX setting and config completion match
   the official app. No transmission before synchronization completes.
5. Receive a known text from another node; compare sender, channel and content.
6. Explicitly send a short test to the chosen channel and check receipt on a
   second node. BLE write success alone is insufficient evidence of delivery.
7. Test a long message, umlauts, bursts, duplicate packets and unknown payloads.
8. Switch off or move the T1000-E out of range; check disconnect and manual
   reconnect, without replaying outgoing messages.
9. Exit and reopen the tool; exercise other Bruce Bluetooth features afterward.
10. Test sleep/wake and watch for watchdog resets and memory pressure, especially
    with the existing T-Embed sleep changes. Scope remains foreground-only.

## Protocol references

- https://github.com/meshtastic/protobufs/blob/master/meshtastic/mesh.proto
- https://github.com/meshtastic/protobufs/blob/master/meshtastic/channel.proto
- https://github.com/meshtastic/protobufs/blob/master/meshtastic/config.proto
- https://github.com/meshtastic/python/blob/master/meshtastic/ble_interface.py
- https://github.com/h2zero/NimBLE-Arduino/tree/2.5.0

Reviewed on 2026-10-18. The lightweight codec implements only the fields needed
by this client, not a general replacement for the official generated protobufs.
The reviewed upstream protobuf revision was
`672149074ff98e6e54f77a0a1548d8c3917fba7c`.

## Validation status of this preparation

The local preparation was statically reviewed and its patch application checked.
There is no C++/ESP32 compiler in that environment. C++ protocol tests and full
PlatformIO compilation are therefore delegated to the included GitHub Actions
workflow; consult the checks for the exact commit before using any build.
No device has been flashed and hardware acceptance remains pending.
