// SPDX-License-Identifier: AGPL-3.0-only
#include "mesh_wire.h"
#include <limits>

namespace bruce_mesh {
namespace {
struct Field {
    uint32_t number = 0;
    unsigned wire = 0;
    uint64_t value = 0;
    const uint8_t *data = nullptr;
    size_t size = 0;
};

class Reader {
    const uint8_t *data;
    size_t size, pos = 0;
    bool good = true;

    bool varint(uint64_t &v) {
        v = 0;
        for (unsigned i = 0; i < 10; ++i) {
            if (pos == size) return good = false;
            const uint8_t b = data[pos++];
            if (i == 9 && b > 1) return good = false;
            v |= uint64_t(b & 0x7f) << (7 * i);
            if (!(b & 0x80)) return true;
        }
        return good = false;
    }

public:
    Reader(const uint8_t *d, size_t n) : data(d), size(n) {}
    bool ok() const { return good && pos == size; }
    bool next(Field &f) {
        if (!good || pos == size) return false;
        f = Field{};
        uint64_t key = 0;
        if (!varint(key) || (key >> 3) == 0 || (key >> 3) > 0x1fffffff) return good = false;
        f.number = uint32_t(key >> 3);
        f.wire = unsigned(key & 7);
        if (f.wire == 0) return varint(f.value);
        if (f.wire == 1 || f.wire == 5) {
            const size_t n = f.wire == 1 ? 8 : 4;
            if (n > size - pos) return good = false;
            for (size_t i = 0; i < n; ++i) f.value |= uint64_t(data[pos++]) << (8 * i);
            return true;
        }
        if (f.wire == 2) {
            uint64_t n = 0;
            if (!varint(n) || n > size - pos) return good = false;
            f.data = data + pos;
            f.size = size_t(n);
            pos += f.size;
            return true;
        }
        return good = false; // Groups are not used by the proto3 PhoneAPI.
    }
};

void varint(Bytes &out, uint64_t v) {
    while (v >= 128) { out.push_back(uint8_t(v) | 0x80); v >>= 7; }
    out.push_back(uint8_t(v));
}
void integer(Bytes &out, unsigned field, uint32_t v) {
    varint(out, uint64_t(field) << 3);
    varint(out, v);
}
void fixed(Bytes &out, unsigned field, uint32_t v) {
    varint(out, (uint64_t(field) << 3) | 5);
    for (unsigned i = 0; i < 4; ++i) out.push_back(uint8_t(v >> (8 * i)));
}
void bytes(Bytes &out, unsigned field, const uint8_t *data, size_t size) {
    varint(out, (uint64_t(field) << 3) | 2);
    varint(out, size);
    if (size) out.insert(out.end(), data, data + size);
}
void nested(Bytes &out, unsigned field, const Bytes &value) { bytes(out, field, value.data(), value.size()); }

bool decodeData(const Field &container, Event &e) {
    Reader r(container.data, container.size);
    Field f;
    uint32_t port = 0;
    bool havePayload = false;
    while (r.next(f)) {
        if (f.number == 1) { if (f.wire != 0) return false; port = uint32_t(f.value); }
        if (f.number == 2) {
            if (f.wire != 2 || f.size > 233) return false;
            e.text.assign(reinterpret_cast<const char *>(f.data), f.size);
            havePayload = true;
        }
    }
    if (!r.ok()) return false;
    if (port == 1 && havePayload && validUtf8(e.text)) e.kind = Kind::Text;
    else e.text.clear();
    return true;
}

bool decodePacket(const Field &container, Event &e) {
    Reader r(container.data, container.size);
    Field f;
    while (r.next(f)) {
        if (f.number == 1 || f.number == 2 || f.number == 6) {
            if (f.wire != 5) return false;
            if (f.number == 1) e.from = uint32_t(f.value);
            if (f.number == 2) e.to = uint32_t(f.value);
            if (f.number == 6) e.id = uint32_t(f.value);
        } else if (f.number == 3) {
            if (f.wire != 0 || f.value > 7) return false;
            e.channel = uint32_t(f.value);
        } else if (f.number == 4) {
            if (f.wire != 2 || !decodeData(f, e)) return false;
        } else if (f.number == 5) { // encrypted oneof supersedes decoded
            if (f.wire != 2) return false;
            e.kind = Kind::Ignored;
            e.text.clear();
        }
    }
    return r.ok();
}

bool decodeChannel(const Field &container, Event &e) {
    e.kind = Kind::Channel;
    Reader r(container.data, container.size);
    Field f;
    while (r.next(f)) {
        if (f.number == 1 || f.number == 3) {
            if (f.wire != 0) return false;
            if (f.number == 1) { if (f.value > 7) return false; e.channel = uint32_t(f.value); }
            else { if (f.value > 2) return false; e.role = uint32_t(f.value); }
        } else if (f.number == 2) {
            if (f.wire != 2) return false;
            Reader settings(f.data, f.size);
            Field setting;
            while (settings.next(setting)) {
                if (setting.number == 3) {
                    if (setting.wire != 2 || setting.size > 32) return false;
                    e.text.assign(reinterpret_cast<const char *>(setting.data), setting.size);
                    if (!validUtf8(e.text)) return false;
                }
                // In particular, skip PSK without retaining or logging it.
            }
            if (!settings.ok()) return false;
        }
    }
    return r.ok();
}

bool decodeConfig(const Field &container, Event &e) {
    Reader r(container.data, container.size);
    Field f;
    while (r.next(f)) {
        if (f.number != 6) continue;
        if (f.wire != 2) return false;
        e.kind = Kind::LoRa;
        Reader lora(f.data, f.size);
        Field l;
        while (lora.next(l)) {
            if (l.number == 7 || l.number == 8 || l.number == 9) {
                if (l.wire != 0 || l.value > std::numeric_limits<uint32_t>::max()) return false;
                if (l.number == 7) e.region = uint32_t(l.value);
                if (l.number == 8) { if (l.value > 7) return false; e.hops = uint32_t(l.value); }
                if (l.number == 9) e.txEnabled = l.value != 0;
            }
        }
        if (!lora.ok()) return false;
    }
    return r.ok();
}
} // namespace

bool validUtf8(const std::string &s) {
    for (size_t i = 0; i < s.size();) {
        uint8_t c = uint8_t(s[i++]);
        if (c < 0x80) { if (c < 0x20 && c != '\n' && c != '\t') return false; continue; }
        unsigned n;
        uint32_t cp, minimum;
        if (c >= 0xc2 && c <= 0xdf) { n = 1; cp = c & 0x1f; minimum = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { n = 2; cp = c & 0x0f; minimum = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { n = 3; cp = c & 7; minimum = 0x10000; }
        else return false;
        if (n > s.size() - i) return false;
        for (unsigned j = 0; j < n; ++j) {
            uint8_t next = uint8_t(s[i++]);
            if ((next & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (next & 0x3f);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
    }
    return true;
}

Bytes requestConfig(uint32_t nonce) { Bytes out; if (nonce) integer(out, 3, nonce); return out; }
Bytes heartbeat() { Bytes out; nested(out, 7, Bytes{}); return out; }
Bytes disconnectRequest() { Bytes out; integer(out, 4, 1); return out; }
Bytes textMessage(uint32_t from, uint32_t id, uint8_t channel, uint8_t hops, const std::string &text) {
    if (!from || !id || channel > 7 || hops > 7 || text.empty() || text.size() > MAX_TEXT || !validUtf8(text)) return {};
    Bytes data, packet, out;
    integer(data, 1, 1); // TEXT_MESSAGE_APP
    bytes(data, 2, reinterpret_cast<const uint8_t *>(text.data()), text.size());
    fixed(packet, 1, from);
    fixed(packet, 2, BROADCAST);
    integer(packet, 3, channel);
    nested(packet, 4, data);
    fixed(packet, 6, id);
    integer(packet, 9, hops);
    // No want_ack: BLE acceptance must never be presented as mesh delivery.
    nested(out, 1, packet);
    return out;
}

bool decodeFromRadio(const uint8_t *data, size_t size, Event &event) {
    event = Event{};
    if (!data || !size || size > MAX_FRAME) return false;
    Reader r(data, size);
    Event decoded;
    Field f;
    while (r.next(f)) {
        if (f.number == 1) { if (f.wire != 0) return false; continue; }
        // FromRadio payload_variant is a oneof. Last recognized payload wins.
        if (f.number == 2) {
            decoded = Event{};
            if (f.wire != 2 || !decodePacket(f, decoded)) return false;
        } else if (f.number == 3) {
            decoded = Event{};
            if (f.wire != 2) return false;
            Reader info(f.data, f.size);
            Field i;
            while (info.next(i)) {
                if (i.number == 1) {
                    if (i.wire != 0 || i.value > 0xffffffffu) return false;
                    decoded.from = uint32_t(i.value);
                }
            }
            if (!info.ok()) return false;
            decoded.kind = Kind::MyInfo;
        } else if (f.number == 5) {
            decoded = Event{};
            if (f.wire != 2 || !decodeConfig(f, decoded)) return false;
        } else if (f.number == 7 || f.number == 8) {
            decoded = Event{};
            if (f.wire != 0 || f.value > 0xffffffffu) return false;
            decoded.kind = f.number == 7 ? Kind::Complete : Kind::Reboot;
            decoded.nonce = uint32_t(f.value);
        } else if (f.number == 10) {
            decoded = Event{};
            if (f.wire != 2 || !decodeChannel(f, decoded)) return false;
        } else if (f.number == 11) {
            decoded = Event{};
            if (f.wire != 2) return false;
            Reader queue(f.data, f.size);
            Field q;
            while (queue.next(q)) {
                if (q.number == 1 || q.number == 4) {
                    if (q.wire != 0) return false;
                    if (q.number == 1) decoded.result = int32_t(q.value);
                    else decoded.id = uint32_t(q.value);
                }
            }
            if (!queue.ok()) return false;
            decoded.kind = Kind::Queue;
        } else if (f.number >= 4 && f.number <= 19) decoded = Event{};
    }
    if (!r.ok()) return false;
    event = decoded;
    return true;
}
} // namespace bruce_mesh
