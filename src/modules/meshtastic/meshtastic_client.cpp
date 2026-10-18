// SPDX-License-Identifier: AGPL-3.0-only
#include "meshtastic_client.h"
#include <globals.h>

#if defined(BRUCE_MESHTASTIC_BLE) && !defined(LITE_VERSION)
#include "mesh_wire.h"
#include "core/display.h"
#include "core/mykeyboard.h"
#include "core/powerSave.h"
#include "core/radio_mem.h"
#include <NimBLEDevice.h>
#include <atomic>
#include <esp_system.h>

namespace {
using namespace bruce_mesh;
constexpr const char *SERVICE = "6ba1b218-15a8-461f-9fa8-5dcae273eafd";
constexpr const char *TO_RADIO = "f75c76d2-129e-4dad-a1dd-7866124401e7";
constexpr const char *FROM_RADIO = "2c55e69e-4993-11ed-b878-0242ac120002";
constexpr const char *FROM_NUM = "ed9da18c-a800-4f66-a670-aa7547e34453";

uint32_t nonce() {
    uint32_t n;
    do { n = esp_random(); } while (!n);
    return n;
}
String nodeLabel(uint32_t n) {
    char out[10];
    snprintf(out, sizeof(out), "!%08lx", static_cast<unsigned long>(n));
    return String(out);
}
String oneline(const std::string &text) {
    String s(text.c_str());
    s.replace("\n", " ");
    s.replace("\r", " ");
    s.replace("\t", " ");
    return s;
}

class Session {
    class Callbacks : public NimBLEClientCallbacks {
        Session &owner;
    public:
        explicit Callbacks(Session &o) : owner(o) {}
        void onPassKeyEntry(NimBLEConnInfo &info) override {
            // UI input happens BEFORE connecting; never block the NimBLE task.
            NimBLEDevice::injectPassKey(info, owner.pin);
        }
        void onDisconnect(NimBLEClient *, int) override { owner.lost.store(true); }
    } callbacks{*this};
    NimBLEClient *client = nullptr;
    NimBLERemoteCharacteristic *tx = nullptr, *rx = nullptr, *fromNum = nullptr;
    std::atomic<bool> dirty{true}, lost{false};
    uint32_t pin = 0, configNonce = 0, localNode = 0;
    uint32_t lastPoll = 0, lastHeartbeat = 0, configStarted = 0;
    bool complete = false, haveLoRa = false, txEnabled = false, timedOut = false;
    uint8_t hops = 0;
    uint32_t region = 0;
    struct Channel { bool enabled = false; String name; } channels[8];
    std::vector<String> history;
    // RX packets can be replayed by firmware. Keep a small bounded dedup cache.
    struct Seen { uint32_t from = 0, id = 0; } seen[32];
    size_t nextSeen = 0;

    void resetConfig() {
        complete = haveLoRa = txEnabled = timedOut = false;
        localNode = region = 0;
        hops = 0;
        for (auto &c : channels) c = Channel{};
        configNonce = nonce();
        configStarted = millis();
    }
    bool write(const Bytes &frame) {
        return connected() && tx && !frame.empty() && frame.size() <= MAX_FRAME &&
               tx->writeValue(frame.data(), frame.size(), true);
    }
    void receive(const Event &e) {
        switch (e.kind) {
        case Kind::MyInfo: localNode = e.from; break;
        case Kind::Complete:
            if (e.nonce == configNonce) {
                complete = true;
                log("Konfiguration gelesen");
            }
            break;
        case Kind::Channel:
            channels[e.channel].enabled = e.role != 0;
            channels[e.channel].name = oneline(e.text);
            break;
        case Kind::LoRa:
            haveLoRa = true;
            hops = uint8_t(e.hops);
            region = e.region;
            txEnabled = e.txEnabled;
            break;
        case Kind::Text:
            if (e.id) {
                for (const auto &s : seen) if (s.from == e.from && s.id == e.id) return;
                seen[nextSeen] = {e.from, e.id};
                nextSeen = (nextSeen + 1) % 32;
            }
            log("RX " + nodeLabel(e.from) + " K" + String(e.channel) + ": " + oneline(e.text));
            break;
        case Kind::Queue:
            if (e.result != 0) log("Funkwarteschlange Fehler " + String(e.result));
            break;
        case Kind::Reboot:
            resetConfig();
            log("Funkgeraet neu gestartet");
            if (!write(requestConfig(configNonce))) log("Konfigurationsanfrage fehlgeschlagen");
            break;
        default: break;
        }
    }

public:
    uint8_t channel = 0;
    bool redraw = true;
    String state = "Nicht verbunden";
    explicit Session(uint32_t passkey) : pin(passkey) {}
    ~Session() { close(); pin = 0; }
    bool connected() const { return client && client->isConnected() && !lost.load(); }
    bool ready() const { return connected() && complete && localNode && haveLoRa && !timedOut; }
    bool canSend() const { return ready() && region != 0 && txEnabled && channels[channel].enabled; }
    const std::vector<String> &lines() const { return history; }
    void log(const String &line) {
        if (history.size() == 24) history.erase(history.begin());
        history.push_back(line);
        redraw = true;
        // Messages and pairing PINs intentionally never go to Serial or files.
    }
    bool open(const NimBLEAddress &address) {
        close();
        lost.store(false);
        dirty.store(true);
        resetConfig();
        client = NimBLEDevice::createClient();
        if (!client) { state = "Kein BLE-Client frei"; return false; }
        client->setClientCallbacks(&callbacks, false);
        client->setConnectTimeout(12000);
        client->setConnectRetries(0);
        if (!client->connect(address)) { state = "Bluetooth-Verbindung fehlgeschlagen"; close(); return false; }
        if (!client->secureConnection()) {
            state = "Kopplung fehlgeschlagen: PIN pruefen";
            close();
            return false;
        }
        auto *service = client->getService(SERVICE);
        if (!service) { state = "Kein Meshtastic-Dienst"; close(); return false; }
        tx = service->getCharacteristic(TO_RADIO);
        rx = service->getCharacteristic(FROM_RADIO);
        fromNum = service->getCharacteristic(FROM_NUM);
        if (!tx || !rx || !fromNum || !tx->canWrite() || !rx->canRead() || !fromNum->canNotify()) {
            state = "Meshtastic-Schnittstelle unvollstaendig";
            close();
            return false;
        }
        if (!fromNum->subscribe(true, [this](NimBLERemoteCharacteristic *, uint8_t *, size_t, bool) {
                dirty.store(true);
            })) {
            state = "Empfangsbenachrichtigung fehlgeschlagen";
            close();
            return false;
        }
        configStarted = lastHeartbeat = millis();
        if (!write(requestConfig(configNonce))) {
            state = "Konfigurationsanfrage fehlgeschlagen";
            close();
            return false;
        }
        state = "Lese Konfiguration...";
        redraw = true;
        return true;
    }
    void close() {
        complete = false;
        tx = rx = fromNum = nullptr;
        if (client) {
            if (client->isConnected()) client->disconnect();
            NimBLEDevice::deleteClient(client);
            client = nullptr;
        }
    }
    void poll() {
        if (!connected()) {
            if (state != "Verbindung getrennt") { state = "Verbindung getrennt"; redraw = true; }
            return;
        }
        const uint32_t now = millis();
        if (!ready() && !timedOut && now - configStarted > 60000) {
            timedOut = true;
            state = "Konfiguration unvollstaendig";
            log("Neu verbinden zum Wiederholen");
        }
        if (dirty.exchange(false) || now - lastPoll >= 1000) {
            lastPoll = now;
            // Drain a bounded batch; preserve input responsiveness for large node DBs.
            for (unsigned i = 0; i < 8 && connected(); ++i) {
                auto value = rx->readValue();
                if (client->getLastError() != 0) {
                    log("Bluetooth-Lesefehler: neu verbinden");
                    close();
                    state = "Bluetooth-Lesefehler";
                    redraw = true;
                    break;
                }
                if (!value.size()) break;
                Event event;
                if (decodeFromRadio(value.data(), value.size(), event)) receive(event);
                else log("Ungueltiges Datenpaket verworfen");
                if (i == 7) dirty.store(true);
            }
        }
        if (now - lastHeartbeat >= 30000) {
            lastHeartbeat = now;
            if (!write(heartbeat())) log("Bluetooth-Heartbeat fehlgeschlagen");
        }
        if (ready()) {
            String next = "Verbunden " + nodeLabel(localNode) + " K" + String(channel);
            if (!canSend()) next += " / nur Empfang";
            if (state != next) { state = next; redraw = true; }
        }
    }
    void chooseChannel() {
        std::vector<Option> menu;
        for (uint8_t i = 0; i < 8; ++i) {
            if (!channels[i].enabled) continue;
            String name = "K" + String(i) + " " + channels[i].name;
            menu.emplace_back(name, [this, i]() { channel = i; redraw = true; });
        }
        if (menu.empty()) { displayInfo("Noch keine Kanaele gelesen", true); return; }
        menu.emplace_back("Zurueck", []() {});
        loopOptions(menu, MENU_TYPE_SUBMENU, "Vorhandene Kanaele");
    }
    void compose() {
        if (!canSend()) { displayWarning("Konfiguration / Sendefreigabe pruefen", true); return; }
        String typed = keyboard("", int(MAX_TEXT), "Meshtastic Nachricht:");
        if (typed == "\x1b" || typed.isEmpty()) return;
        std::string text(typed.c_str(), typed.length());
        Bytes frame = textMessage(localNode, nonce(), channel, hops, text);
        if (frame.empty()) { displayError("Max. 180 UTF-8 Bytes; Text pruefen", true); return; }
        bool confirmed = false;
        std::vector<Option> confirm;
        confirm.emplace_back("An Kanal " + String(channel) + " senden", [&]() { confirmed = true; });
        confirm.emplace_back("Abbrechen", []() {});
        loopOptions(confirm, MENU_TYPE_SUBMENU, "Kanalnachricht");
        // Never retry a message automatically after ambiguous BLE failure.
        if (confirmed) {
            if (canSend() && write(frame)) {
                log("TX K" + String(channel) + ": " + typed);
                log("An Funkgeraet uebergeben; Zustellung offen");
                dirty.store(true);
            } else log("Senden unklar/fehlgeschlagen; nicht automatisch wiederholt");
        }
        redraw = true;
    }
};

bool readPin(uint32_t &pin) {
    String text = num_keyboard("", 6, "Bluetooth-PIN des T1000-E", true);
    if (text.length() != 6) return false;
    pin = 0;
    for (size_t i = 0; i < 6; ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        pin = pin * 10 + uint32_t(text[i] - '0');
    }
    return true;
}

void drawSession(const Session &session, size_t offset) {
    drawMainBorderWithTitle("Meshtastic BLE (Test)");
    tft.setTextSize(1);
    tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
    tft.setCursor(10, 32);
    tft.println(session.state);
    const auto &lines = session.lines();
    const size_t visible = size_t((tftHeight - 82) / 12 > 0 ? (tftHeight - 82) / 12 : 1);
    size_t end = lines.size() > offset ? lines.size() - offset : 0;
    size_t start = end > visible ? end - visible : 0;
    const size_t width = size_t((tftWidth - 24) / 6);
    int y = 49;
    for (size_t i = start; i < end; ++i) {
        tft.setCursor(10, y);
        String line = lines[i];
        if (line.length() > width) line = line.substring(0, width > 3 ? width - 3 : 0) + "...";
        tft.print(line);
        y += 12;
    }
    printCenterFootnote("Drehen: Verlauf | Klick: Menue");
}

void runSession(const NimBLEAddress &address, uint32_t pin) {
    Session session(pin);
    displayInfo("Verbinde und kopple...");
    if (!session.open(address)) { displayError(session.state, true); return; }
    size_t offset = 0;
    while (!returnToMenu && !check(EscPress)) {
        session.poll();
        if (check(PrevPress)) { if (offset + 1 < session.lines().size()) ++offset; session.redraw = true; }
        if (check(NextPress)) { if (offset) --offset; session.redraw = true; }
        if (check(SelPress)) {
            bool exit = false;
            std::vector<Option> menu;
            menu.emplace_back("Nachricht schreiben", [&]() { session.compose(); });
            menu.emplace_back("Kanal waehlen", [&]() { session.chooseChannel(); });
            menu.emplace_back("Letzte Nachricht ganz", [&]() {
                if (session.lines().empty()) return;
                size_t index = session.lines().size() - 1 - (offset < session.lines().size() ? offset : 0);
                drawMainBorderWithTitle("Nachricht");
                tft.setCursor(10, 40);
                tft.setTextSize(1);
                tft.setTextWrap(true);
                tft.print(session.lines()[index]);
                while (!check(EscPress) && !check(SelPress) && !returnToMenu) {
                    session.poll();
                    delay(20);
                }
                tft.setTextWrap(false);
            });
            menu.emplace_back("Neu verbinden", [&]() {
                displayInfo("Verbinde erneut...");
                if (!session.open(address)) displayError(session.state, true);
            });
            menu.emplace_back("Zurueck zum Verlauf", []() {});
            menu.emplace_back("Trennen und beenden", [&]() { exit = true; });
            loopOptions(menu, MENU_TYPE_SUBMENU, "Meshtastic");
            if (exit) break;
            session.redraw = true;
        }
        if (session.redraw) { drawSession(session, offset); session.redraw = false; }
        delay(20);
    }
}
} // namespace

void meshtasticMenu() {
    // Own the BLE stack only when idle: never tear down another Bruce tool.
    if (NimBLEDevice::isInitialized()) {
        displayWarning("Andere Bluetooth-Funktion zuerst beenden", true);
        return;
    }
    if (!radioHasMemForBle()) { displayError("Nicht genug Bluetooth-Arbeitsspeicher", true); return; }
    if (!NimBLEDevice::init("Bruce Meshtastic")) { displayError("Bluetooth-Start fehlgeschlagen", true); return; }
    struct Cleanup {
        ~Cleanup() { NimBLEDevice::deinit(true); }
    } cleanup;
    NimBLEDevice::setMTU(512);
    NimBLEDevice::setSecurityAuth(false, true, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_ONLY);
    auto *scan = NimBLEDevice::getScan();
    if (!scan) { displayError("Bluetooth-Suche nicht verfuegbar", true); return; }
    scan->setActiveScan(true);
    scan->setMaxResults(32);
    displayInfo("Suche Meshtastic (8 Sekunden)...");
    auto results = scan->getResults(8000, false);
    std::vector<Option> devices;
    for (int i = 0; i < results.getCount(); ++i) {
        auto *device = results.getDevice(i);
        if (!device || !device->isAdvertisingService(NimBLEUUID(SERVICE))) continue;
        NimBLEAddress address = device->getAddress(); // retain public/random address type
        String name = device->getName().empty() ? String(address.toString().c_str()) : String(device->getName().c_str());
        String label = name + " " + String(device->getRSSI()) + "dBm";
        devices.emplace_back(label, [address]() {
            uint32_t pin;
            if (!readPin(pin)) { displayWarning("Kopplung abgebrochen / PIN ungueltig", true); return; }
            runSession(address, pin);
            pin = 0;
        });
    }
    scan->clearResults();
    if (devices.empty()) { displayInfo("Kein Meshtastic-Geraet gefunden", true); return; }
    devices.emplace_back("Zurueck", []() {});
    loopOptions(devices, MENU_TYPE_SUBMENU, "Meshtastic-Geraet");
}
#else
void meshtasticMenu() {}
#endif
