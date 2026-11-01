#include <Geode/Geode.hpp>
#include <Geode/modify/FMODAudioEngine.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/ScrollLayer.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/ui/Notification.hpp>
#include <Geode/ui/BasedButtonSprite.hpp>
#include <Geode/utils/async.hpp>
#include <Geode/utils/file.hpp>

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

using namespace geode::prelude;

// ---------------------------------------------------------------------------
// Storage: original sound name (e.g. "explode_11.ogg") -> user's custom file
// ---------------------------------------------------------------------------
namespace {
    struct SoundStore {
        std::shared_mutex mutex;
        // original name -> absolute path of the custom file (used by the hooks)
        std::unordered_map<std::string, std::string> active;
        // original name -> file name inside <save dir>/sounds (persisted)
        std::unordered_map<std::string, std::string> stored;
        // when true the hooks play the original sound (not used for now, kept for previews)
        bool bypass = false;
    };

    SoundStore& store() {
        static SoundStore s;
        return s;
    }

    auto soundsDir() {
        return Mod::get()->getSaveDir() / "sounds";
    }

    auto indexPath() {
        return Mod::get()->getSaveDir() / "replacements.txt";
    }

    std::string lowerStr(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return s;
    }

    // ".mp3", ".ogg", ... (empty if there is none)
    std::string extOf(std::string const& path) {
        auto slash = path.find_last_of("/\\");
        auto dot = path.find_last_of('.');
        if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "";
        return path.substr(dot);
    }

    std::string sanitize(std::string const& name) {
        std::string out;
        out.reserve(name.size());
        for (unsigned char c : name) {
            out += (std::isalnum(c) || c == '.' || c == '_' || c == '-') ? static_cast<char>(c) : '_';
        }
        return out;
    }

    // Must be called with the unique lock held (or before anything else uses the store)
    void saveIndexLocked(SoundStore& s) {
        std::string data;
        for (auto const& [orig, file] : s.stored) {
            data += orig;
            data += '\t';
            data += file;
            data += '\n';
        }
        (void) geode::utils::file::createDirectoryAll(Mod::get()->getSaveDir());
        (void) geode::utils::file::writeString(indexPath(), data);
    }

    void loadIndex() {
        auto& s = store();
        std::unique_lock lock(s.mutex);
        auto res = geode::utils::file::readString(indexPath());
        if (res.isErr()) return; // first launch, nothing saved yet

        std::string const data = res.unwrap();
        size_t pos = 0;
        while (pos < data.size()) {
            auto end = data.find('\n', pos);
            if (end == std::string::npos) end = data.size();
            std::string line = data.substr(pos, end - pos);
            pos = end + 1;

            if (!line.empty() && line.back() == '\r') line.pop_back();
            auto tab = line.find('\t');
            if (tab == std::string::npos || tab == 0 || tab + 1 >= line.size()) continue;

            std::string orig = line.substr(0, tab);
            std::string file = line.substr(tab + 1);
            auto full = soundsDir() / file;
            std::error_code existsEc;
            if (!std::filesystem::exists(full, existsEc)) continue; // file was deleted by hand

            s.stored[orig] = file;
            s.active[orig] = full.string();
        }
    }

    void ensureLoaded() {
        static std::once_flag flag;
        std::call_once(flag, loadIndex);
    }

    // Used by the hooks: returns the custom file path if this sound is replaced
    std::string redirect(std::string const& path) {
        ensureLoaded();
        auto& s = store();
        std::shared_lock lock(s.mutex);
        if (s.bypass) return path;
        auto it = s.active.find(path);
        return it == s.active.end() ? path : it->second;
    }

    bool isReplaced(std::string const& name) {
        ensureLoaded();
        auto& s = store();
        std::shared_lock lock(s.mutex);
        return s.active.contains(name);
    }

    void removeStoredFileLocked(SoundStore& s, std::string const& name) {
        auto it = s.stored.find(name);
        if (it == s.stored.end()) return;
        std::error_code removeEc;
        std::filesystem::remove(soundsDir() / it->second, removeEc);
        s.stored.erase(it);
        s.active.erase(name);
    }

    // Returns an empty string on success, otherwise an error message
    std::string setReplacement(std::string const& name, std::filesystem::path const& source) {
        ensureLoaded();

        auto bytes = geode::utils::file::readBinary(source);
        if (bytes.isErr()) {
            return fmt::format("Can't read the file: {}", bytes.unwrapErr());
        }
        if (bytes.unwrap().empty()) {
            return "The file is empty";
        }

        (void) geode::utils::file::createDirectoryAll(soundsDir());

        std::string fileName = sanitize(name) + extOf(source.string());
        auto dest = soundsDir() / fileName;

        auto& s = store();
        std::unique_lock lock(s.mutex);

        // drop the previous file (it may have a different extension)
        removeStoredFileLocked(s, name);

        auto written = geode::utils::file::writeBinary(dest, bytes.unwrap());
        if (written.isErr()) {
            saveIndexLocked(s);
            return fmt::format("Can't save the file: {}", written.unwrapErr());
        }

        s.stored[name] = fileName;
        s.active[name] = dest.string();
        saveIndexLocked(s);
        return "";
    }

    void resetReplacement(std::string const& name) {
        ensureLoaded();
        auto& s = store();
        std::unique_lock lock(s.mutex);
        removeStoredFileLocked(s, name);
        saveIndexLocked(s);
    }

    bool isAudioName(std::string const& name) {
        auto ext = lowerStr(extOf(name));
        return ext == ".ogg" || ext == ".mp3" || ext == ".wav";
    }

    // All sounds that exist in the game's Resources folder.
    // Falls back to a list of well-known sounds when the folder can't be listed
    // (for example on Android, where resources live inside the APK).
    std::vector<std::string> listGameSounds() {
        std::vector<std::string> out;

        try {
            std::filesystem::path dir(dirs::getResourcesDir().string());
            std::error_code ec;
            for (auto const& entry : std::filesystem::directory_iterator(dir, ec)) {
                if (ec) break;
                if (!entry.is_regular_file(ec)) continue;
                auto name = entry.path().filename().string();
                if (isAudioName(name)) out.push_back(std::move(name));
            }
        } catch (...) {
            out.clear();
        }

        if (out.empty()) {
            out = {
                "achievement_01.ogg", "buyItem01.ogg", "buyItem03.ogg", "chest07.ogg",
                "chest08.ogg", "door001.ogg", "endStart_02.ogg", "explode_11.ogg",
                "highscoreGet02.ogg", "magicExplosion.ogg", "playSound_01.ogg",
                "quitSound_01.ogg", "reward01.ogg", "secretKey.ogg", "menuLoop.mp3",
            };
        }

        std::sort(out.begin(), out.end(), [](std::string const& a, std::string const& b) {
            return lowerStr(a) < lowerStr(b);
        });
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }
}

// ---------------------------------------------------------------------------
// Hooks: redirect the requested sound to the custom file
// ---------------------------------------------------------------------------
class $modify(CSAudioEngine, FMODAudioEngine) {
    int playEffect(gd::string path, float speed, float unknown, float volume) {
        return FMODAudioEngine::playEffect(redirect(std::string(path)), speed, unknown, volume);
    }

    int playEffectAdvanced(
        gd::string path, float speed, float unknown, float volume, float pitch,
        bool fastFourierTransform, bool reverb, int startMillis, int endMillis,
        int fadeIn, int fadeOut, bool loopEnabled, int effectID, bool overrideSound,
        bool noPreload, int channelID, int uniqueID, float minInterval, int sfxGroup
    ) {
        return FMODAudioEngine::playEffectAdvanced(
            redirect(std::string(path)), speed, unknown, volume, pitch, fastFourierTransform, reverb,
            startMillis, endMillis, fadeIn, fadeOut, loopEnabled, effectID, overrideSound,
            noPreload, channelID, uniqueID, minInterval, sfxGroup
        );
    }

    void playMusic(gd::string path, bool shouldLoop, float fadeInTime, int channel) {
        FMODAudioEngine::playMusic(redirect(std::string(path)), shouldLoop, fadeInTime, channel);
    }
};

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------
class CustomSoundsPopup : public Popup {
protected:
    static constexpr float LIST_WIDTH = 330.f;
    static constexpr float ROW_HEIGHT = 30.f;

    std::vector<std::string> m_sounds;
    ScrollLayer* m_scroll = nullptr;
    TextInput* m_search = nullptr;

    bool init() {
        if (!Popup::init(380.f, 280.f)) return false;

        this->setTitle("Custom sounds");

        ensureLoaded();
        m_sounds = listGameSounds();

        m_search = TextInput::create(300.f, "Search sound...");
        m_search->setCallback([this](std::string const&) {
            this->rebuildList();
        });
        m_mainLayer->addChildAtPosition(m_search, Anchor::Top, ccp(0.f, -45.f));

        auto bg = CCScale9Sprite::create("square02b_001.png", { 0.f, 0.f, 80.f, 80.f });
        bg->setColor({ 0, 0, 0 });
        bg->setOpacity(90);
        bg->setScale(.5f);
        bg->setContentSize({ (LIST_WIDTH + 10.f) * 2.f, 180.f * 2.f });
        m_mainLayer->addChildAtPosition(bg, Anchor::Center, ccp(0.f, -25.f));

        m_scroll = ScrollLayer::create({ LIST_WIDTH, 170.f });
        m_scroll->m_contentLayer->setLayout(ScrollLayer::createDefaultListLayout());
        m_mainLayer->addChildAtPosition(m_scroll, Anchor::Center, ccp(-LIST_WIDTH / 2.f, -110.f));

        this->rebuildList();
        return true;
    }

    void rebuildList() {
        auto filter = lowerStr(m_search->getString());

        m_scroll->m_contentLayer->removeAllChildren();
        for (auto const& name : m_sounds) {
            if (!filter.empty() && lowerStr(name).find(filter) == std::string::npos) continue;
            m_scroll->m_contentLayer->addChild(this->makeRow(name));
        }
        m_scroll->m_contentLayer->updateLayout();
        m_scroll->scrollToTop();
    }

    CCNode* makeRow(std::string const& name) {
        bool replaced = isReplaced(name);

        auto row = CCNode::create();
        row->setContentSize({ LIST_WIDTH, ROW_HEIGHT });

        auto bg = CCScale9Sprite::create("square02b_001.png", { 0.f, 0.f, 80.f, 80.f });
        bg->setColor(replaced ? ccColor3B { 40, 120, 40 } : ccColor3B { 0, 0, 0 });
        bg->setOpacity(replaced ? 160 : 70);
        bg->setScale(.5f);
        bg->setContentSize({ LIST_WIDTH * 2.f, ROW_HEIGHT * 2.f });
        bg->setPosition({ LIST_WIDTH / 2.f, ROW_HEIGHT / 2.f });
        row->addChild(bg);

        auto label = CCLabelBMFont::create(name.c_str(), "bigFont.fnt");
        label->limitLabelWidth(175.f, .5f, .15f);
        label->setAnchorPoint({ 0.f, .5f });
        label->setPosition({ 8.f, ROW_HEIGHT / 2.f });
        row->addChild(label);

        auto menu = CCMenu::create();
        menu->setContentSize({ LIST_WIDTH, ROW_HEIGHT });
        menu->setPosition({ 0.f, 0.f });
        menu->ignoreAnchorPointForPosition(false);
        menu->setAnchorPoint({ 0.f, 0.f });
        row->addChild(menu);

        // preview
        auto playSpr = CCSprite::createWithSpriteFrameName("GJ_playMusicBtn_001.png");
        playSpr->setScale(.45f);
        auto playBtn = CCMenuItemExt::createSpriteExtra(playSpr, [name](CCMenuItemSpriteExtra*) {
            auto engine = FMODAudioEngine::sharedEngine();
            if (lowerStr(extOf(name)) == ".mp3") {
                engine->playMusic(name, false, 0.f, 0);
            } else {
                engine->playEffect(name, 1.f, 0.f, 1.f);
            }
        });
        playBtn->setPosition({ 205.f, ROW_HEIGHT / 2.f });
        menu->addChild(playBtn);

        // replace
        auto replaceSpr = ButtonSprite::create("Replace", "bigFont.fnt", "GJ_button_01.png", .6f);
        replaceSpr->setScale(.6f);
        auto replaceBtn = CCMenuItemExt::createSpriteExtra(replaceSpr, [this, name](CCMenuItemSpriteExtra*) {
            this->pickFileFor(name);
        });
        replaceBtn->setPosition({ 262.f, ROW_HEIGHT / 2.f });
        menu->addChild(replaceBtn);

        // reset (only when replaced)
        if (replaced) {
            auto resetSpr = CCSprite::createWithSpriteFrameName("GJ_deleteIcon_001.png");
            resetSpr->setScale(.5f);
            auto resetBtn = CCMenuItemExt::createSpriteExtra(resetSpr, [this, name](CCMenuItemSpriteExtra*) {
                resetReplacement(name);
                Notification::create("Original sound restored", NotificationIcon::Success)->show();
                this->rebuildList();
            });
            resetBtn->setPosition({ 311.f, ROW_HEIGHT / 2.f });
            menu->addChild(resetBtn);
        }

        return row;
    }

    void pickFileFor(std::string name) {
        namespace file = geode::utils::file;

        geode::async::spawn(
            file::pick(file::PickMode::OpenFile, file::FilePickOptions {
                .filters = { file::FilePickOptions::Filter {
                    .description = "Audio files",
                    .files = { "*.mp3", "*.ogg", "*.wav" },
                }},
            }),
            [self = Ref<CustomSoundsPopup>(this), name](file::PickResult result) {
                if (result.isErr()) {
                    Notification::create("Can't open the file picker", NotificationIcon::Error)->show();
                    return;
                }
                auto picked = std::move(result).unwrap();
                if (!picked.has_value()) return; // cancelled

                auto err = setReplacement(name, *picked);
                if (!err.empty()) {
                    Notification::create(err, NotificationIcon::Error)->show();
                    return;
                }
                Notification::create("Sound replaced", NotificationIcon::Success)->show();
                self->rebuildList();
            }
        );
    }

public:
    static CustomSoundsPopup* create() {
        auto ret = new CustomSoundsPopup();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

// Button on the main menu that opens the popup
class $modify(CSMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;

        auto menu = this->getChildByID("bottom-menu");
        if (!menu) return true;

        auto spr = CircleButtonSprite::createWithSpriteFrameName("GJ_playMusicBtn_001.png");
        auto btn = CCMenuItemExt::createSpriteExtra(spr, [](CCMenuItemSpriteExtra*) {
            CustomSoundsPopup::create()->show();
        });
        btn->setID("custom-sounds-button"_spr);
        menu->addChild(btn);
        menu->updateLayout();
        return true;
    }
};
