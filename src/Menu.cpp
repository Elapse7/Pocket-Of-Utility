#include "Core.hpp"

#include <algorithm>
#include <cmath>

using namespace geode::prelude;

namespace {

// Sliders are logarithmic so the useful range gets most of the bar.
float speedFromSlider(float v) { return std::round(std::pow(10.f, 2.f * v - 1.f) * 100.f) / 100.f; } // 0.1x .. 10x
float sliderFromSpeed(float s) { return (std::log10(std::max(s, 0.1f)) + 1.f) / 2.f; }
float tpsFromSlider(float v)   { return std::round(60.f * std::pow(100.f, v) / 10.f) * 10.f; }       // 60 .. 6000
float sliderFromTps(float t)   { return std::log(std::max(t, 60.f) / 60.f) / std::log(100.f); }

void notify(std::string const& text, bool good) {
    Notification::create(text, good ? NotificationIcon::Success : NotificationIcon::Error, 1.5f)->show();
}

class UtilMenu : public CCLayer {
public:
    CCLabelBMFont* m_speedLabel = nullptr;
    CCLabelBMFont* m_tpsLabel = nullptr;
    CCLabelBMFont* m_info = nullptr;

    static UtilMenu* create() {
        auto ret = new UtilMenu();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    bool init() override {
        if (!CCLayer::init()) return false;

        auto& c = pu::cfg();
        auto win = CCDirector::get()->getWinSize();
        float cx = win.width / 2.f;
        float cy = win.height / 2.f;

        this->setID("pu-menu");
        this->setTouchEnabled(true);
        this->setKeypadEnabled(true);

        // dimmed background + panel
        this->addChild(CCLayerColor::create({0, 0, 0, 140}, win.width, win.height));

        auto panel = CCScale9Sprite::create("GJ_square01.png", {0.f, 0.f, 80.f, 80.f});
        panel->setContentSize({340.f, 280.f});
        panel->setPosition({cx, cy});
        this->addChild(panel);

        auto box = CCScale9Sprite::create("square02_001.png", {0.f, 0.f, 80.f, 80.f});
        box->setContentSize({316.f, 218.f});
        box->setPosition({cx, cy - 6.f});
        box->setOpacity(70);
        this->addChild(box);

        auto title = CCLabelBMFont::create("Alicia Menu", "goldFont.fnt");
        title->setScale(.8f);
        title->setPosition({cx, cy + 118.f});
        this->addChild(title);

        auto menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        menu->setTouchPriority(-501);
        this->addChild(menu);

        // ---- toggles ----
        struct Row { char const* name; int tag; bool on; float y; };
        Row rows[] = {
            {"Speedhack",   1, c.speedhack,  cy + 86.f},
            {"Speed audio", 2, c.speedAudio, cy + 60.f},
            {"TPS bypass",  3, c.tpsBypass,  cy - 6.f},
        };
        for (auto& r : rows) {
            auto label = CCLabelBMFont::create(r.name, "bigFont.fnt");
            label->setScale(.45f);
            label->setAnchorPoint({0.f, .5f});
            label->setPosition({cx - 150.f, r.y});
            this->addChild(label);

            auto toggler = CCMenuItemToggler::createWithStandardSprites(
                this, menu_selector(UtilMenu::onToggle), .65f
            );
            toggler->setTag(r.tag);
            toggler->toggle(r.on);
            toggler->setPosition({cx + 130.f, r.y});
            menu->addChild(toggler);
        }

        // ---- sliders ----
        m_speedLabel = CCLabelBMFont::create("", "bigFont.fnt");
        m_speedLabel->setScale(.4f);
        m_speedLabel->setPosition({cx, cy + 36.f});
        this->addChild(m_speedLabel);

        auto speedSlider = Slider::create(this, menu_selector(UtilMenu::onSpeed), .85f);
        speedSlider->setPosition({cx, cy + 20.f});
        speedSlider->setValue(sliderFromSpeed(c.speed));
        this->addChild(speedSlider);
        speedSlider->m_touchLogic->setTouchPriority(-501);

        m_tpsLabel = CCLabelBMFont::create("", "bigFont.fnt");
        m_tpsLabel->setScale(.4f);
        m_tpsLabel->setPosition({cx, cy - 30.f});
        this->addChild(m_tpsLabel);

        auto tpsSlider = Slider::create(this, menu_selector(UtilMenu::onTps), .85f);
        tpsSlider->setPosition({cx, cy - 46.f});
        tpsSlider->setValue(sliderFromTps(c.tps));
        this->addChild(tpsSlider);
        tpsSlider->m_touchLogic->setTouchPriority(-501);

        // ---- replay buttons ----
        auto addButton = [&](char const* text, char const* tex, float x, float yy, SEL_MenuHandler cb) {
            auto spr = ButtonSprite::create(text, "bigFont.fnt", tex, .5f);
            auto btn = CCMenuItemSpriteExtra::create(spr, this, cb);
            btn->setPosition({x, yy});
            menu->addChild(btn);
        };
        addButton("Rec",  "GJ_button_06.png", cx - 80.f, cy - 76.f, menu_selector(UtilMenu::onRecord));
        addButton("Play", "GJ_button_01.png", cx,        cy - 76.f, menu_selector(UtilMenu::onPlay));
        addButton("Stop", "GJ_button_02.png", cx + 80.f, cy - 76.f, menu_selector(UtilMenu::onStop));

        m_info = CCLabelBMFont::create("", "chatFont.fnt");
        m_info->setScale(.8f);
        m_info->setPosition({cx, cy - 98.f});
        this->addChild(m_info);

        addButton("Close", "GJ_button_01.png", cx, cy - 122.f, menu_selector(UtilMenu::onClose));

        this->refresh();
        this->schedule(schedule_selector(UtilMenu::tick), .5f);
        return true;
    }

    void refresh() {
        auto& c = pu::cfg();
        m_speedLabel->setString(fmt::format("Speed  {:.2f}x", c.speed).c_str());
        m_tpsLabel->setString(fmt::format("TPS  {}", static_cast<int>(c.tps)).c_str());
        tick(0.f);
    }

    void tick(float) {
        auto& e = pu::Engine::get();
        char const* mode = "Idle";
        if (e.mode == pu::Mode::Recording) mode = "Recording";
        if (e.mode == pu::Mode::Playing) mode = "Playing";

        if (e.inputCount() > 0) {
            m_info->setString(
                fmt::format("{}  -  {} inputs  -  {:.1f} KB", mode, e.inputCount(), e.sizeInBytes() / 1024.0).c_str()
            );
        } else {
            m_info->setString(fmt::format("{}  -  no macro in memory", mode).c_str());
        }
    }

    // ---- callbacks ----
    void onToggle(CCObject* sender) {
        auto* t = static_cast<CCMenuItemToggler*>(sender);
        bool on = !t->isToggled(); // the callback fires before the state flips
        auto& c = pu::cfg();
        switch (t->getTag()) {
            case 1: c.speedhack = on; break;
            case 2: c.speedAudio = on; break;
            case 3: c.tpsBypass = on; break;
        }
    }

    void onSpeed(CCObject* sender) {
        pu::cfg().speed = speedFromSlider(static_cast<SliderThumb*>(sender)->getValue());
        this->refresh();
    }

    void onTps(CCObject* sender) {
        pu::cfg().tps = tpsFromSlider(static_cast<SliderThumb*>(sender)->getValue());
        this->refresh();
    }

    void onRecord(CCObject*) {
        auto* pl = PlayLayer::get();
        if (!pl) return notify("Open a level first", false);
        if (!pu::Engine::get().startRecording(pl)) return notify("Could not start recording", false);
        notify("Recording from the start (practice checkpoints work). It saves when you finish the level", true);
        this->onClose(nullptr);
    }

    void onPlay(CCObject*) {
        auto* pl = PlayLayer::get();
        if (!pl) return notify("Open a level first", false);
        if (!pu::Engine::get().startPlayback(pl)) return notify("No macro saved for this level", false);
        notify("Playing macro", true);
        this->onClose(nullptr);
    }

    void onStop(CCObject*) {
        pu::Engine::get().stop(false);
        notify("Stopped", true);
        this->tick(0.f);
    }

    void onClose(CCObject*) {
        pu::cfg().save();
        this->setKeypadEnabled(false);
        this->removeFromParentAndCleanup(true);
    }

    void keyBackClicked() override { this->onClose(nullptr); }

    // Swallow every touch that isn't on the menu so the game underneath stays untouched.
    void registerWithTouchDispatcher() override {
        CCTouchDispatcher::get()->addTargetedDelegate(this, -500, true);
    }
    bool ccTouchBegan(CCTouch*, CCEvent*) override { return true; }
};

} // namespace

namespace pu {

void openMenu() {
    auto* scene = CCDirector::get()->getRunningScene();
    if (!scene || scene->getChildByID("pu-menu")) return;
    if (auto* layer = UtilMenu::create()) scene->addChild(layer, 9999);
}

} // namespace pu
