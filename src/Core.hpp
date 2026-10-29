#pragma once

#include <Geode/Geode.hpp>
#include <cstdint>
#include <filesystem>
#include <unordered_map>
#include <vector>

namespace pu {

// ---------------------------------------------------------------------------
// Settings (saved between sessions)
// ---------------------------------------------------------------------------
struct Settings {
    bool noclip = false;
    bool speedhack = false;
    float speed = 1.f;        // 0.1x .. 10x
    bool speedAudio = true;   // pitch the audio together with the speedhack
    bool tpsBypass = false;
    float tps = 240.f;        // 60 .. 6000
    bool stepper = false;     // frame stepper (game frozen until "Step")
    bool accuracy = true;     // "Input fixes": store/restore player state on every input

    bool stepRequest = false; // runtime only

    void load();
    void save() const;
};

Settings& cfg();

// ---------------------------------------------------------------------------
// Replay engine (input based, with input fixes, practice-mode aware)
// ---------------------------------------------------------------------------
enum class Mode { Idle, Recording, Playing };

struct PState {
    float x = 0.f, y = 0.f, v = 0.f, rot = 0.f; // position, y velocity, rotation
};

struct Click {
    uint32_t frame;   // physics step the input was handled on
    uint8_t button;   // 1 = jump, 2 = left, 3 = right
    bool down;
    bool p1;
    bool hasFix;      // input fix stored for this click
    bool dual;        // player 2 state stored too
    PState a;         // player 1 state right before the input
    PState b;         // player 2 state right before the input
};

class Engine {
public:
    static Engine& get();

    Mode mode = Mode::Idle;
    bool injecting = false;   // true while the engine itself fires an input

    bool startRecording(PlayLayer* pl);
    bool startPlayback(PlayLayer* pl);
    void stop(bool save);
    void onLevelComplete(PlayLayer* pl);

    // reset + practice mode handling
    void beginReset() { m_loadedCheckpoint = false; }
    void endReset();
    void onCheckpointStored(CheckpointObject* cp) { m_cpFrames[cp] = m_step; }
    void onCheckpointLoaded(CheckpointObject* cp);

    // called by the hooks
    void beforeStep(GJBaseGameLayer* g);
    void afterStep() { ++m_step; }
    void onClick(bool down, int button, bool p1);

    size_t inputCount() const { return m_clicks.size(); }
    size_t sizeInBytes() const { return serialize().size(); }

    std::vector<uint8_t> serialize() const;
    bool deserialize(std::vector<uint8_t> const& data);
    std::filesystem::path pathFor(PlayLayer* pl) const;
    bool saveFor(PlayLayer* pl) const;
    bool loadFor(PlayLayer* pl);

private:
    void onReset();
    static float effectiveTps();

    std::vector<Click> m_clicks;
    std::unordered_map<CheckpointObject*, uint32_t> m_cpFrames;
    size_t m_clickCursor = 0;
    uint32_t m_step = 0;
    float m_macroTps = 240.f;
    bool m_loadedCheckpoint = false;

    // player state captured at the start of the current step (while recording)
    bool m_preDual = false;
    PState m_preA, m_preB;
};

// menu
void openMenu();

} // namespace pu
