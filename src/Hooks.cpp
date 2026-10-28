#include "Core.hpp"

#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
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

// ---------------------------------------------------------------------------
// Speedhack (+ audio)
// ---------------------------------------------------------------------------
class $modify(PUScheduler, CCScheduler) {
    void update(float dt) {
        auto& c = pu::cfg();
        float speed = c.speedhack ? c.speed : 1.f;
        CCScheduler::update(dt * speed);
        applyAudioSpeed(speed);
    }
};

// ---------------------------------------------------------------------------
// Physics: TPS bypass, frame stepper, replay input
// ---------------------------------------------------------------------------
class $modify(PUBaseGameLayer, GJBaseGameLayer) {
    // Runs once per physics step. The replay engine hooks in here so that
    // recording and playback happen at exactly the same point of every step.
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

    // Frame stepper: freeze physics until a step is requested.
    void update(float dt) {
        auto& c = pu::cfg();
        if (c.stepper && isPlayLayer(this)) {
            if (!c.stepRequest) return;
            c.stepRequest = false;
            float spt = 1.f / (c.tpsBypass ? c.tps : 240.f);
            m_extraDelta = 0.f;
            GJBaseGameLayer::update(spt * 1.001f); // exactly one physics step
            return;
        }
        GJBaseGameLayer::update(dt);
    }

    // TPS bypass: same idea as the vanilla function, but with a custom step size.
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

// ---------------------------------------------------------------------------
// PlayLayer: noclip, macro lifecycle, in-level controls
// ---------------------------------------------------------------------------
class $modify(PUPlayLayer, PlayLayer) {
    struct Fields {
        CCLabelBMFont* status = nullptr;
        ButtonSprite* freezeSpr = nullptr;
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
        menu->setContentSize({260.f, 24.f});
        menu->setAnchorPoint({0.f, .5f});
        menu->setPosition({6.f, win.height - 30.f});
        menu->setTouchPriority(-600); // above the game's own tap-to-jump handling
        menu->setLayout(RowLayout::create()->setGap(4.f)->setAxisAlignment(AxisAlignment::Start));

        auto mk = [&](char const* text, char const* tex, SEL_MenuHandler cb) {
            auto spr = ButtonSprite::create(text, "bigFont.fnt", tex, .5f);
            auto btn = CCMenuItemSpriteExtra::create(spr, this, cb);
            menu->addChild(btn);
            return spr;
        };
        mk("Menu", "GJ_button_01.png", menu_selector(PUPlayLayer::onMenu));
        m_fields->freezeSpr = mk("Freeze", "GJ_button_02.png", menu_selector(PUPlayLayer::onFreeze));
        mk("Step", "GJ_button_02.png", menu_selector(PUPlayLayer::onStep));
        menu->updateLayout();
        m_uiLayer->addChild(menu, 100);

        this->schedule(schedule_selector(PUPlayLayer::updateHud));
    }

    void onMenu(CCObject*) { pu::openMenu(); }

    void onFreeze(CCObject*) {
        auto& c = pu::cfg();
        c.stepper = !c.stepper;
        c.stepRequest = false;
    }

    void onStep(CCObject*) {
        auto& c = pu::cfg();
        c.stepper = true; // stepping always freezes first
        c.stepRequest = true;
    }

    void updateHud(float) {
        auto& e = pu::Engine::get();
        if (auto* s = m_fields->status) {
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
                    s->setString(pu::cfg().stepper ? "FROZEN" : "");
                    s->setColor({255, 255, 255});
                    break;
            }
        }
        if (auto* f = m_fields->freezeSpr) {
            f->setString(pu::cfg().stepper ? "Resume" : "Freeze");
        }
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        pu::Engine::get().onReset();
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        // Let the anti-cheat spike through so noclip can't break the level itself.
        if (pu::cfg().noclip && object != m_anticheatSpike) return;
        PlayLayer::destroyPlayer(player, object);
    }

    void levelComplete() {
        auto& c = pu::cfg();
        auto& e = pu::Engine::get();

        // Runs that used any of these tools are flagged as test runs so nothing
        // is submitted or saved to your account by accident.
        bool cheated = c.noclip || c.tpsBypass || c.stepper ||
                       (c.speedhack && c.speed != 1.f) || e.mode != pu::Mode::Idle;
        if (cheated) m_isTestMode = true;

        e.onLevelComplete(this); // stops + saves the recording at the end of the level
        PlayLayer::levelComplete();
    }

    void onQuit() {
        pu::Engine::get().stop(false);
        PlayLayer::onQuit();
    }
};

// ---------------------------------------------------------------------------
// Entry buttons for the menu (touch friendly)
// ---------------------------------------------------------------------------
class $modify(PUPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto label = CCLabelBMFont::create("PU", "bigFont.fnt");
        auto spr = CircleButtonSprite::create(label, CircleBaseColor::Green, CircleBaseSize::Small);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(PUPauseLayer::onPU));

        CCNode* menu = this->getChildByID("right-button-menu");
        if (!menu) {
            auto own = CCMenu::create();
            own->setPosition({40.f, 40.f});
            this->addChild(own);
            menu = own;
        }
        menu->addChild(btn);
        menu->updateLayout();
    }

    void onPU(CCObject*) { pu::openMenu(); }
};

class $modify(PUMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;

        auto label = CCLabelBMFont::create("PU", "bigFont.fnt");
        auto spr = CircleButtonSprite::create(label, CircleBaseColor::Green, CircleBaseSize::MediumAlt);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(PUMenuLayer::onPU));

        CCNode* menu = this->getChildByID("bottom-menu");
        if (!menu) {
            auto own = CCMenu::create();
            own->setPosition({40.f, 40.f});
            this->addChild(own);
            menu = own;
        }
        menu->addChild(btn);
        menu->updateLayout();
        return true;
    }

    void onPU(CCObject*) { pu::openMenu(); }
};
