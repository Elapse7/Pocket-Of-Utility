#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>

using namespace geode::prelude;

static constexpr char const* kFrom = "your friend";

class $modify(BirthdayMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;

        auto win = CCDirector::get()->getWinSize();

        auto title = CCLabelBMFont::create("Happy 19th Birthday, Azure Will!", "goldFont.fnt");
        title->setScale(.6f);
        title->setPosition({win.width / 2.f, win.height - 22.f});
        title->runAction(CCRepeatForever::create(CCSequence::create(
            CCScaleTo::create(.8f, .66f),
            CCScaleTo::create(.8f, .6f),
            nullptr
        )));
        this->addChild(title, 50);

        auto sub = CCLabelBMFont::create("Chaos Airflow victor!", "bigFont.fnt");
        sub->setScale(.4f);
        sub->setPosition({win.width / 2.f, win.height - 42.f});
        this->addChild(sub, 50);

        // the popup needs a moment, the menu scene isn't running yet during init
        this->scheduleOnce(schedule_selector(BirthdayMenuLayer::showGreeting), 1.f);
        return true;
    }

    void showGreeting(float) {
        static bool shown = false;
        if (shown) return;
        shown = true;

        auto msg = std::string("Happy 19th Birthday, Azure Will!\n\n")
            + "Congrats on beating Chaos Airflow too. Enjoy your day!\n\n"
            + "- from " + kFrom;
        FLAlertLayer::create("Happy Birthday!", msg, "Thanks!")->show();
    }
};
