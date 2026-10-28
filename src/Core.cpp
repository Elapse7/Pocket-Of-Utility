#include "Core.hpp"

#include <cmath>
#include <cstring>

using namespace geode::prelude;

namespace pu {

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------
Settings& cfg() {
    static Settings s;
    return s;
}

void Settings::load() {
    auto* m = Mod::get();
    noclip     = m->getSavedValue<bool>("noclip", false);
    speedhack  = m->getSavedValue<bool>("speedhack", false);
    speed      = m->getSavedValue<float>("speed", 1.f);
    speedAudio = m->getSavedValue<bool>("speed-audio", true);
    tpsBypass  = m->getSavedValue<bool>("tps-bypass", false);
    tps        = m->getSavedValue<float>("tps", 240.f);
    stepper    = false; // always start unfrozen
    accuracy   = m->getSavedValue<bool>("accuracy", true);
}

void Settings::save() const {
    auto* m = Mod::get();
    m->setSavedValue("noclip", noclip);
    m->setSavedValue("speedhack", speedhack);
    m->setSavedValue("speed", speed);
    m->setSavedValue("speed-audio", speedAudio);
    m->setSavedValue("tps-bypass", tpsBypass);
    m->setSavedValue("tps", tps);
    m->setSavedValue("accuracy", accuracy);
}

// ---------------------------------------------------------------------------
// Small binary helpers (varints keep the macro files tiny)
// ---------------------------------------------------------------------------
namespace {

void putVar(std::vector<uint8_t>& o, uint32_t v) {
    while (v >= 0x80) {
        o.push_back(static_cast<uint8_t>(v | 0x80));
        v >>= 7;
    }
    o.push_back(static_cast<uint8_t>(v));
}

bool getVar(std::vector<uint8_t> const& d, size_t& i, uint32_t& out) {
    uint32_t result = 0;
    for (int shift = 0; shift < 35; shift += 7) {
        if (i >= d.size()) return false;
        uint8_t b = d[i++];
        result |= static_cast<uint32_t>(b & 0x7F) << shift;
        if (!(b & 0x80)) {
            out = result;
            return true;
        }
    }
    return false;
}

template <class T>
void put(std::vector<uint8_t>& o, T v) {
    auto* p = reinterpret_cast<uint8_t*>(&v);
    o.insert(o.end(), p, p + sizeof(T));
}

template <class T>
bool get(std::vector<uint8_t> const& d, size_t& i, T& v) {
    if (i + sizeof(T) > d.size()) return false;
    std::memcpy(&v, d.data() + i, sizeof(T));
    i += sizeof(T);
    return true;
}

void fixPlayer(PlayerObject* p, float x, float y, float v) {
    if (!p) return;
    constexpr float kTolerance = 0.05f; // game units; real desyncs are far bigger
    if (std::abs(p->getPositionX() - x) > kTolerance ||
        std::abs(p->getPositionY() - y) > kTolerance) {
        p->setPosition({x, y});
        p->m_yVelocity = v;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------
Engine& Engine::get() {
    static Engine e;
    return e;
}

float Engine::effectiveTps() {
    return cfg().tpsBypass ? cfg().tps : 240.f;
}

bool Engine::startRecording(PlayLayer* pl) {
    // Practice-mode checkpoints rewind the physics, which would corrupt the
    // step counter. Macros are recorded as full runs from the start.
    if (pl->m_isPracticeMode) return false;

    m_clicks.clear();
    m_snaps.clear();
    m_macroTps = effectiveTps();
    mode = Mode::Recording;
    pl->resetLevel(); // start from step 0
    return true;
}

bool Engine::startPlayback(PlayLayer* pl) {
    if (!loadFor(pl)) return false;

    // Play back at the TPS the macro was recorded with.
    auto& c = cfg();
    c.tpsBypass = m_macroTps != 240.f;
    c.tps = m_macroTps;

    mode = Mode::Playing;
    pl->resetLevel();
    return true;
}

void Engine::stop(bool save) {
    if (mode == Mode::Recording && save && !m_clicks.empty()) {
        if (auto* pl = PlayLayer::get()) saveFor(pl);
    }
    mode = Mode::Idle;
}

void Engine::onReset() {
    m_step = 0;
    m_clickCursor = 0;
    m_snapCursor = 0;
    if (mode == Mode::Recording) {
        // Every attempt starts a fresh recording; only a completed run is kept.
        m_clicks.clear();
        m_snaps.clear();
        m_macroTps = effectiveTps();
    }
}

void Engine::onLevelComplete(PlayLayer* pl) {
    if (mode == Mode::Recording) {
        bool ok = !m_clicks.empty() && saveFor(pl);
        Notification::create(
            ok ? fmt::format("Macro saved ({} inputs)", m_clicks.size())
               : std::string("Nothing to save"),
            ok ? NotificationIcon::Success : NotificationIcon::Error, 2.f
        )->show();
    }
    mode = Mode::Idle; // recording / playback both end with the level
}

void Engine::onClick(bool down, int button, bool p1) {
    if (mode != Mode::Recording) return;
    m_clicks.push_back({m_step, static_cast<uint8_t>(button), down, p1});
}

void Engine::pushSnap(GJBaseGameLayer* g) {
    Snap s{};
    s.frame = m_step;
    s.dual = g->m_gameState.m_isDualMode;
    if (g->m_player1) {
        s.x1 = g->m_player1->getPositionX();
        s.y1 = g->m_player1->getPositionY();
        s.v1 = static_cast<float>(g->m_player1->m_yVelocity);
    }
    if (s.dual && g->m_player2) {
        s.x2 = g->m_player2->getPositionX();
        s.y2 = g->m_player2->getPositionY();
        s.v2 = static_cast<float>(g->m_player2->m_yVelocity);
    }
    m_snaps.push_back(s);
}

void Engine::applySnap(GJBaseGameLayer* g, Snap const& s) {
    fixPlayer(g->m_player1, s.x1, s.y1, s.v1);
    if (s.dual) fixPlayer(g->m_player2, s.x2, s.y2, s.v2);
}

// Runs at the very start of every physics step, before the game handles input.
// Recording and playback use the exact same point in the step, which is what
// keeps replays frame-accurate.
void Engine::beforeStep(GJBaseGameLayer* g) {
    if (mode == Mode::Recording) {
        if (cfg().accuracy && m_step % kSnapEvery == 0) pushSnap(g);
    } else if (mode == Mode::Playing) {
        while (m_snapCursor < m_snaps.size() && m_snaps[m_snapCursor].frame < m_step) {
            ++m_snapCursor;
        }
        if (cfg().accuracy && m_snapCursor < m_snaps.size() &&
            m_snaps[m_snapCursor].frame == m_step) {
            applySnap(g, m_snaps[m_snapCursor]);
        }

        while (m_clickCursor < m_clicks.size() && m_clicks[m_clickCursor].frame <= m_step) {
            auto c = m_clicks[m_clickCursor++];
            injecting = true;
            g->handleButton(c.down, c.button, c.p1);
            injecting = false;
        }
    }
}

// ---------------------------------------------------------------------------
// File format  ("PMR1")
//   4 bytes  magic
//   4 bytes  float TPS
//   varint   click count, then per click: varint frame delta + 1 flag byte
//   varint   snapshot count, then per snapshot: varint frame delta, 1 byte
//            dual flag, 3 floats (+3 floats if dual)
// A click costs about 2 bytes, so even very long macros stay small.
// ---------------------------------------------------------------------------
std::vector<uint8_t> Engine::serialize() const {
    std::vector<uint8_t> o;
    o.reserve(16 + m_clicks.size() * 3 + m_snaps.size() * 20);
    o.insert(o.end(), {'P', 'M', 'R', '1'});
    put(o, m_macroTps);

    putVar(o, static_cast<uint32_t>(m_clicks.size()));
    uint32_t prev = 0;
    for (auto const& c : m_clicks) {
        putVar(o, c.frame - prev);
        prev = c.frame;
        o.push_back(static_cast<uint8_t>((c.button & 3) | (c.down ? 4 : 0) | (c.p1 ? 8 : 0)));
    }

    putVar(o, static_cast<uint32_t>(m_snaps.size()));
    prev = 0;
    for (auto const& s : m_snaps) {
        putVar(o, s.frame - prev);
        prev = s.frame;
        o.push_back(s.dual ? 1 : 0);
        put(o, s.x1); put(o, s.y1); put(o, s.v1);
        if (s.dual) { put(o, s.x2); put(o, s.y2); put(o, s.v2); }
    }
    return o;
}

bool Engine::deserialize(std::vector<uint8_t> const& d) {
    if (d.size() < 9 || std::memcmp(d.data(), "PMR1", 4) != 0) return false;
    size_t i = 4;

    float tps = 240.f;
    uint32_t n = 0;
    if (!get(d, i, tps) || !getVar(d, i, n) || n > d.size()) return false;

    std::vector<Click> clicks;
    clicks.reserve(n);
    uint32_t frame = 0;
    for (uint32_t k = 0; k < n; ++k) {
        uint32_t delta = 0;
        if (!getVar(d, i, delta) || i >= d.size()) return false;
        frame += delta;
        uint8_t f = d[i++];
        clicks.push_back({frame, static_cast<uint8_t>(f & 3), (f & 4) != 0, (f & 8) != 0});
    }

    uint32_t m = 0;
    if (!getVar(d, i, m) || m > d.size()) return false;

    std::vector<Snap> snaps;
    snaps.reserve(m);
    frame = 0;
    for (uint32_t k = 0; k < m; ++k) {
        uint32_t delta = 0;
        if (!getVar(d, i, delta) || i >= d.size()) return false;
        frame += delta;
        Snap s{};
        s.frame = frame;
        s.dual = d[i++] != 0;
        if (!get(d, i, s.x1) || !get(d, i, s.y1) || !get(d, i, s.v1)) return false;
        if (s.dual && (!get(d, i, s.x2) || !get(d, i, s.y2) || !get(d, i, s.v2))) return false;
        snaps.push_back(s);
    }

    m_clicks = std::move(clicks);
    m_snaps = std::move(snaps);
    m_macroTps = tps;
    return true;
}

std::filesystem::path Engine::pathFor(PlayLayer* pl) const {
    int id = pl->m_level->m_levelID.value();
    return Mod::get()->getSaveDir() / "macros" / fmt::format("{}.pmr", id);
}

bool Engine::saveFor(PlayLayer* pl) const {
    auto path = pathFor(pl);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    return utils::file::writeBinary(path, serialize()).isOk();
}

bool Engine::loadFor(PlayLayer* pl) {
    auto res = utils::file::readBinary(pathFor(pl));
    if (res.isErr()) return false;
    return deserialize(res.unwrap());
}

} // namespace pu
