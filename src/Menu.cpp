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
    CCLabelBMFont* m_mode = nullptr;
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

        // dimmed background + main panel
        this->addChild(CCLayerColor::create({0, 0, 0, 140}, win.width, win.height));

        auto panel = CCScale9Sprite::create("GJ_square01.png", {0.f, 0.f, 80.f, 80.f});
        panel->setContentSize({430.f, 290.f});
        panel->setPosition({cx, cy});
        this->addChild(panel);

        auto addBox = [&](CCSize size, CCPoint pos) {
            auto box = CCScale9Sprite::create("square02_001.png", {0.f, 0.f, 80.f, 80.f});
            box->setContentSize(size);
            box->setPosition(pos);
            box->setOpacity(70);
            this->addChild(box);
        };
        addBox({195.f, 205.f}, {cx - 105.f, cy + 5.f});
        addBox({195.f, 205.f}, {cx + 105.f, cy + 5.f});

        auto title = CCLabelBMFont::create("Alicia Menu", "goldFont.fnt");
        title->setScale(.8f);
        title->setPosition({cx, cy + 126.f});
        this->addChild(title);

        auto menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        menu->setTouchPriority(-501);
        this->addChild(menu);

        // ---- left column: toggles ----
        struct Row { char const* name; int tag; bool on; };
        Row rows[] = {
            {"Noclip",        1, c.noclip},
