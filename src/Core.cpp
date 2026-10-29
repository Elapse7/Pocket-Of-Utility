#include "Core.hpp"

#include <algorithm>
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
// Small binary helpers (varints keep the macro files small)
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

// not called "get": Engine has a static member with that name that would hide it
template <class T>
bool getVal(std::vector<uint8_t> const& d, size_t& i, T& v) {
    if (i + sizeof(T) > d.size()) return false;
    std::memcpy(&v, d.data() + i, sizeof(T));
    i += sizeof(T);
    return true;
}

void putState(std::vector<uint8_t>& o, PState const& s) {
    put(o, s.x); put(o, s.y); put(o, s.v); put(o, s.rot);
}

bool getState(std::vector<uint8_t> const& d, size_t& i, PState& s) {
    return getVal(d, i, s.x) && getVal(d, i, s.y) && getVal(d, i, s.v) && getVal(d, i, s.rot);
}

PState readPlayer(PlayerObject* p) {
    PState s;
    if (p) {
        s.x = p->getPositionX();
        s.y = p->getPositionY();
        s.v = static_cast<float>(p->m_yVelocity);
        s.rot = p->getRotation();
    }
    return s;
}

void writePlayer(PlayerObject* p, PState const& s) {
    if (!p) return;
    p->setPosition({s.x, s.y});
    p->m_yVelocity = s.v;
    p->setRotation(s.rot);
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
    m_clicks.clear();
    m_macroTps = effectiveTps();

    // Recording always starts from the very beginning of the level, so old
    // practice checkpoints have to go. New ones can be placed while recording.
    if (pl->m_isPracticeMode && pl->m_checkpointArray) {
        pl->m_checkpointArray->removeAllObjects();
    }

    mode = Mode::Recording;
    pl->resetLevel();
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
    if (mode == Mode::Recording) {
        // A restart from the beginning throws the recording away.
        m_clicks.clear();
        m_macroTps = effectiveTps();
    }
}

// Called after PlayLayer::resetLevel. If the game respawned the player from a
// practice checkpoint, the step counter was already rewound by
// onCheckpointLoaded, so only a real restart resets everything.
void Engine::endReset() {
    if (!m_loadedCheckpoint) onReset();
}

// Practice mode: dying and respawning from a checkpoint rewinds the recording
// to the step the checkpoint was placed on, and drops every input after it.
void Engine::onCheckpointLoaded(CheckpointObject* cp) {
    auto it = m_cpFrames.find(cp);
    if (it == m_cpFrames.end()) return;

    m_loadedCheckpoint = true;
    m_step = it->second;

    if (mode == Mode::Recording) {
        while (!m_clicks.empty() && m_clicks.back().frame >= m_step) m_clicks.pop_back();
    }

    m_clickCursor = static_cast<size_t>(
        std::lower_bound(
            m_clicks.begin(), m_clicks.end(), m_step,
            [](Click const& c, uint32_t f) { return c.frame < f; }
        ) - m_clicks.begin()
    );
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

    Click c{};
    c.frame = m_step;
    c.button = static_cast<uint8_t>(button);
    c.down = down;
    c.p1 = p1;
    c.hasFix = cfg().accuracy;
    c.dual = m_preDual;
    c.a = m_preA;
    c.b = m_preB;
    m_clicks.push_back(c);
}

// Runs at the very start of every physics step, before the game handles input.
// Recording and playback use the exact same point in the step, which is what
// keeps replays accurate.
void Engine::beforeStep(GJBaseGameLayer* g) {
    if (mode == Mode::Recording) {
        // remember the state the player is in right before any input of this step
        m_preDual = g->m_gameState.m_isDualMode;
        m_preA = readPlayer(g->m_player1);
        m_preB = m_preDual ? readPlayer(g->m_player2) : PState{};
    } else if (mode == Mode::Playing) {
        while (m_clickCursor < m_clicks.size() && m_clicks[m_clickCursor].frame <= m_step) {
            Click const& c = m_clicks[m_clickCursor++];

            // input fix: put the player exactly where it was when the input was recorded
            if (c.hasFix && cfg().accuracy) {
                writePlayer(g->m_player1, c.a);
                if (c.dual) writePlayer(g->m_player2, c.b);
            }

            injecting = true;
            g->handleButton(c.down, c.button, c.p1);
            injecting = false;
        }
    }
}

// ---------------------------------------------------------------------------
// File format  ("ALI1", saved as .ali)
//   4 bytes  magic
//   4 bytes  float TPS
//   varint   click count, then per click:
//              varint frame delta
//              1 flag byte: bits 0-1 button, 2 down, 3 player1, 4 has fix, 5 dual
//              if has fix: 4 floats (player 1), plus 4 floats if dual
// Without input fixes a click costs about 2 bytes; with them about 18.
// ---------------------------------------------------------------------------
std::vector<uint8_t> Engine::serialize() const {
    std::vector<uint8_t> o;
    o.reserve(16 + m_clicks.size() * 20);
    o.insert(o.end(), {'A', 'L', 'I', '1'});
    put(o, m_macroTps);

    putVar(o, static_cast<uint32_t>(m_clicks.size()));
    uint32_t prev = 0;
    for (auto const& c : m_clicks) {
        putVar(o, c.frame - prev);
        prev = c.frame;
        o.push_back(static_cast<uint8_t>(
            (c.button & 3) | (c.down ? 4 : 0) | (c.p1 ? 8 : 0) |
            (c.hasFix ? 16 : 0) | (c.dual ? 32 : 0)
        ));
        if (c.hasFix) {
            putState(o, c.a);
            if (c.dual) putState(o, c.b);
        }
    }
    return o;
}

bool Engine::deserialize(std::vector<uint8_t> const& d) {
    if (d.size() < 9 || std::memcmp(d.data(), "ALI1", 4) != 0) return false;
    size_t i = 4;

    float tps = 240.f;
    uint32_t n = 0;
    if (!getVal(d, i, tps) || !getVar(d, i, n) || n > d.size()) return false;

    std::vector<Click> clicks;
    clicks.reserve(n);
    uint32_t frame = 0;
    for (uint32_t k = 0; k < n; ++k) {
        uint32_t delta = 0;
        if (!getVar(d, i, delta) || i >= d.size()) return false;
        frame += delta;
        uint8_t f = d[i++];

        Click c{};
        c.frame = frame;
        c.button = f & 3;
        c.down = (f & 4) != 0;
        c.p1 = (f & 8) != 0;
        c.hasFix = (f & 16) != 0;
        c.dual = (f & 32) != 0;
        if (c.hasFix) {
            if (!getState(d, i, c.a)) return false;
            if (c.dual && !getState(d, i, c.b)) return false;
        }
        clicks.push_back(c);
    }

    m_clicks = std::move(clicks);
    m_macroTps = tps;
    return true;
}

std::filesystem::path Engine::pathFor(PlayLayer* pl) const {
    int id = pl->m_level->m_levelID.value();
    return Mod::get()->getSaveDir() / "macros" / fmt::format("{}.ali", id);
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
