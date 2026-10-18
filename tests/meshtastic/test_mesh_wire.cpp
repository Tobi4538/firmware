// SPDX-License-Identifier: AGPL-3.0-only
#include "modules/meshtastic/mesh_wire.h"
#include <cassert>
#include <iostream>
#include <random>

using namespace bruce_mesh;
bool decode(const Bytes &data, Event &e) { return decodeFromRadio(data.data(), data.size(), e); }

int main() {
    assert(requestConfig(150) == Bytes({0x18, 0x96, 0x01}));
    assert(requestConfig(0).empty());
    assert(heartbeat() == Bytes({0x3a, 0x00}));
    // Golden ToRadio.packet encoded independently using the official field table:
    // from=fixed32(1), to=fixed32(2), channel=3, decoded=4, id=fixed32(6), hop_limit=9.
    const Bytes expected = {
        0x0a, 0x1b, 0x0d, 0x78, 0x56, 0x34, 0x12,
        0x15, 0xff, 0xff, 0xff, 0xff, 0x18, 0x02,
        0x22, 0x06, 0x08, 0x01, 0x12, 0x02, 'H', 'i',
        0x35, 0x04, 0x03, 0x02, 0x01, 0x48, 0x03
    };
    assert(textMessage(0x12345678, 0x01020304, 2, 3, "Hi") == expected);
    assert(textMessage(1, 1, 8, 3, "Hi").empty());
    assert(textMessage(1, 1, 0, 8, "Hi").empty());
    assert(textMessage(1, 0, 0, 3, "Hi").empty());
    assert(textMessage(1, 1, 0, 3, "").empty());
    assert(!textMessage(1, 1, 0, 3, std::string(MAX_TEXT, 'a')).empty());
    assert(textMessage(1, 1, 0, 3, std::string(MAX_TEXT + 1, 'a')).empty());
    assert(validUtf8("Gr\xc3\xbc\xc3\x9f" "e \xf0\x9f\x90\x88"));
    for (auto s : {std::string("\xc0\xaf"), std::string("\xed\xa0\x80"),
                   std::string("\xf4\x90\x80\x80"), std::string("\xc3"), std::string("a\0b", 3)}) {
        assert(!validUtf8(s));
        assert(textMessage(1, 1, 0, 3, s).empty());
    }
    Event e;
    Bytes incoming = expected;
    incoming[0] = 0x12; // Same MeshPacket, FromRadio.packet is field 2.
    assert(decode(incoming, e));
    assert(e.kind == Kind::Text && e.text == "Hi" && e.from == 0x12345678);
    assert(e.to == BROADCAST && e.channel == 2 && e.id == 0x01020304);
    assert(decode(Bytes{0x1a, 0x06, 0x08, 0xf8, 0xac, 0xd1, 0x91, 0x01}, e));
    assert(e.kind == Kind::MyInfo && e.from == 0x12345678);
    assert(decode(Bytes{0x38, 0x96, 0x01}, e) && e.kind == Kind::Complete && e.nonce == 150);
    // Channel 2, name "Chat", role SECONDARY. PSK bytes are skipped.
    assert(decode(Bytes{0x52, 0x10, 0x08, 0x02, 0x12, 0x0a,
                        0x12, 0x02, 0xaa, 0xbb, 0x1a, 0x04, 'C', 'h', 'a', 't', 0x18, 0x02}, e));
    assert(e.kind == Kind::Channel && e.channel == 2 && e.role == 2 && e.text == "Chat");
    // FromRadio.config -> Config.lora -> EU_868, hop_limit 3, tx_enabled true.
    assert(decode(Bytes{0x2a, 0x08, 0x32, 0x06, 0x38, 0x03, 0x40, 0x03, 0x48, 0x01}, e));
    assert(e.kind == Kind::LoRa && e.region == 3 && e.hops == 3 && e.txEnabled);
    assert(decode(Bytes{0x5a, 0x04, 0x08, 0x01, 0x20, 0x07}, e));
    assert(e.kind == Kind::Queue && e.result == 1 && e.id == 7);
    // Unknown fields of every supported wire type must be skipped safely.
    Bytes unknown{0xa0, 0x06, 0x01, 0xa9, 0x06, 0,0,0,0,0,0,0,0,
                  0xb2, 0x06, 0x02, 1,2, 0xbd, 0x06, 0,0,0,0, 0x38, 0x01};
    assert(decode(unknown, e) && e.kind == Kind::Complete && e.nonce == 1);
    // Every truncated nested message is rejected, with no partially decoded event.
    for (size_t i = 0; i < incoming.size(); ++i) {
        Bytes truncated(incoming.begin(), incoming.begin() + i);
        assert(!decode(truncated, e));
        assert(e.kind == Kind::Ignored && e.text.empty());
    }
    for (const Bytes &bad : {Bytes{0}, Bytes{0x0f}, Bytes{0x12, 0xff, 0xff, 0xff, 0xff, 0x7f},
                           Bytes{0x38,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x02},
                           Bytes{0x12,2,0x08,1}, Bytes{0x52,2,0x08,8}, Bytes(MAX_FRAME + 1, 0)}) {
        assert(!decode(bad, e));
        assert(e.kind == Kind::Ignored);
    }
    assert(decode(Bytes{0x40,0x01}, e) && e.kind == Kind::Reboot);
    // No out-of-bounds reads on arbitrary BLE bytes (run under ASan/UBSan in CI).
    std::mt19937 rng(0x1000e);
    for (unsigned trial = 0; trial < 20000; ++trial) {
        Bytes data(rng() % (MAX_FRAME + 2));
        for (auto &b : data) b = uint8_t(rng());
        decode(data, e);
    }
    std::cout << "Meshtastic protocol golden vectors, bounds, UTF-8 and 20000 malformed frames: PASS\n";
}
