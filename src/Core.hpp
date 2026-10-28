#pragma once

#include <Geode/Geode.hpp>
#include <cstdint>
#include <filesystem>
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
    bool accuracy = true;     // replay: re-sync player position every second

    bool stepRequest = false; // runtime only

    void load();
    void save() const;
};

Settings& cfg();

// ---------------------------------------------------------------------------
// Replay engine
// ---------------------------------------------------------------------------
enum class Mode { Idle, Recording, Playing };

struct Click {
    uint32_t frame;   // physics step the input was handled on
    uint8_t button;   // 1 = jump, 2 = left, 3 = right
    bool down;
    bool p1;
};

// Every kSnapEvery steps the engine stores the player state. While playing a
// macro back, any drift larger than a tiny tolerance gets corrected.
struct Snap {
    uint32_t frame;
    bool dual;
    float x1, y1, v1;
    float x2, y2, v2;
};

class Engine {
public:
    static Engine& get();

    Mode mode = Mode::Idle;
    bool injecting = false;   // true while the engine itself fires an input

    bool startRecording(PlayLayer* pl);
    bool startPlayback(PlayLayer* pl);
    void stop(bool save);
    void onReset();
    void onLevelComplete(PlayLayer* pl);

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
    static constexpr uint32_t kSnapEvery = 240;

    void pushSnap(GJBaseGameLayer* g);
    void applySnap(GJBaseGameLayer* g, Snap const& s);
    static float effectiveTps();

    std::vector<Click> m_clicks;
    std::vector<Snap> m_snaps;
    size_t m_clickCursor = 0;
    size_t m_snapCursor = 0;
    uint32_t m_step = 0;
    float m_macroTps = 240.f;
};

// menu
void openMenu();

} // namespace pu
