#include "Core.hpp"

#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <algorithm>
#include <cmath>

using namespace geode::prelude;

$on_mod(Loaded) {
    pu::cfg().load();
}

namespace {

bool isPlayLayer(GJBaseGameLayer* g) {
    auto* pl = PlayLayer::get();
    return pl && static_cast<GJBaseGameLayer*>(pl) == g;
}

// Pitches the whole FMOD mix so music and sounds follow the speedhack.
void applyAudioSpeed(float speed) {
    static float last = 1.f;
    float target = pu::cfg().speedAudio ? std::clamp(speed, 0.1f, 10.f) : 1.f;
    if (std::abs(target - last) < 0.001f) return;

    auto* engine = FMODAudioEngine::sharedEngine();
    if (!engine || !engine->m_system) return;

    FMOD::ChannelGroup* master = nullptr;
    if (engine->m_system->getMasterChannelGroup(&master) == FMOD_OK && master) {
        master->setPitch(target);
        last = target;
    }
}

} // namespace

// Speedhack (+ audio)
class $modify(PUScheduler, CCScheduler) {
    void update(float dt) {
        auto& c = pu::cfg();
        float speed = c.speedhack ? c.speed : 1.f;
        CCScheduler::update(dt * speed);
        applyAudioSpeed(speed);
    }
};

// TPS bypass + replay input
class $modify(PUBaseGameLayer, GJBaseGameLayer) {
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        bool inLevel = isPlayLayer(this);
        if (inLevel) pu::Engine::get().beforeStep(this);
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        if (inLevel) pu::Engine::get().afterStep();
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        auto& e = pu::Engine::get();
        if (isPlayLayer(this)) {
            // While a macro plays, only the macro may press buttons.
            if (e.mode == pu::Mode::Playing && !e.injecting) return;
            if (e.mode == pu::Mode::Recording) e.onClick(down, button, isPlayer1);
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    float getModifiedDelta(float dt) {
        auto& c = pu::cfg();
        if (!c.tpsBypass || !isPlayLayer(this)) {
            return GJBaseGameLayer::getModifiedDelta(dt);
        }
        float timewarp = std::min(1.f, m_gameState.m_timeWarp);
        float spt = timewarp / c.tps;
        float total = dt + m_extraDelta;
        int steps = std::max(1, static_cast<int>(total / spt));
        float used = steps * spt;
        m_extraDelta = total - used;
        return used;
    }
};

// PlayLayer: macro lifecycle + the small menu button
class $modify(PUPlayLayer, PlayLayer) {
    struct Fields {
        CCLabelBMFont* status = nullptr;
    };

    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();

        auto win = CCDirector::get()->getWinSize();

        auto status = CCLabelBMFont::create("", "bigFont.fnt");
        status->setScale(.4f);
        status->setOpacity(200);
        status->setPosition({win.width / 2.f, win.height - 12.f});
        m_uiLayer->addChild(status, 100);
        m_fields->status = status;

        auto menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        menu->setTouchPriority(-600); // above the game's own tap-to-jump handling

        auto label = CCLabelBMFont::create("AL", "bigFont.fnt");
        auto spr = CircleButtonSprite::create(label, CircleBaseColor::Green, CircleBaseSize::Small);
        spr->setScale(.7f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(PUPlayLayer::onMenu));
        btn->setPosition({24.f, win.height - 24.f});
        menu->addChild(btn);
        m_uiLayer->addChild(menu, 100);

        this->schedule(schedule_selector(PUPlayLayer::updateHud));
    }

    void onMenu(CCObject*) { pu::openMenu(); }

    void updateHud(float) {
        auto& e = pu::Engine::get();
        auto* s = m_fields->status;
        if (!s) return;
        switch (e.mode) {
            case pu::Mode::Recording:
                s->setString(fmt::format("REC  {} inputs", e.inputCount()).c_str());
                s->setColor({255, 90, 90});
                break;
            case pu::Mode::Playing:
                s->setString("PLAYBACK");
                s->setColor({90, 255, 140});
                break;
            default:
                s->setString("");
                break;
        }
    }

    // Practice mode support: the engine tracks the step each checkpoint was
    // placed on, and rewinds the recording when you respawn from one.
    void resetLevel() {
        auto& e = pu::Engine::get();
        e.beginReset();
        PlayLayer::resetLevel();
        e.endReset();
    }

    void storeCheckpoint(CheckpointObject* cp) {
        PlayLayer::storeCheckpoint(cp);
        pu::Engine::get().onCheckpointStored(cp);
    }

    void loadFromCheckpoint(CheckpointObject* cp) {
        PlayLayer::loadFromCheckpoint(cp);
        pu::Engine::get().onCheckpointLoaded(cp);
    }

    void levelComplete() {
        auto& c = pu::cfg();
        auto& e = pu::Engine::get();

        // Runs that used these tools are flagged as test runs so nothing
        // is submitted or saved to your account by accident.
        bool cheated = c.tpsBypass || (c.speedhack && c.speed != 1.f) ||
                       e.mode != pu::Mode::Idle;
        if (cheated) m_isTestMode = true;

        e.onLevelComplete(this); // stops + saves the recording at the end of the level
        PlayLayer::levelComplete();
    }

    void onQuit() {
        pu::Engine::get().stop(false);
        PlayLayer::onQuit();
    }
};
