// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

// A deliberately small PhoneAPI subset. Field numbers and wire types follow
// meshtastic/protobufs: mesh.proto, channel.proto and config.proto.
// No channel keys or radio configuration are retained or written by this client.
namespace bruce_mesh {
constexpr size_t MAX_FRAME = 512;
constexpr size_t MAX_TEXT = 180; // UTF-8 bytes, conservative radio payload budget
constexpr uint32_t BROADCAST = 0xffffffffu;
using Bytes = std::vector<uint8_t>;

enum class Kind { Ignored, Text, MyInfo, Complete, Channel, LoRa, Queue, Reboot };
struct Event {
    Kind kind = Kind::Ignored;
    uint32_t from = 0, to = 0, id = 0, nonce = 0;
    uint32_t channel = 0, role = 0, hops = 0, region = 0;
    int32_t result = 0;
    bool txEnabled = false;
    std::string text;
};

Bytes requestConfig(uint32_t nonce);
Bytes heartbeat();
Bytes disconnectRequest();
Bytes textMessage(uint32_t from, uint32_t id, uint8_t channel, uint8_t hops, const std::string &text);
bool decodeFromRadio(const uint8_t *data, size_t size, Event &event);
bool validUtf8(const std::string &text);
} // namespace bruce_mesh
