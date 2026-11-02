#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "audio/mixer.hpp"
#include "moonmod/audio_api.hpp"
#include "moonmod/runtime.hpp"

namespace dc::game {}
using namespace dc::game;

namespace {

namespace fs = std::filesystem;

struct TempRoot {
    fs::path path = fs::temp_directory_path() /
                    ("moonmod-audio-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ~TempRoot() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

void Write(const fs::path &path, std::string_view contents) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    ASSERT_TRUE(output.good()) << path.string();
}

template <typename T>
void WriteLittle(std::ofstream &output, T value) {
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        output.put(static_cast<char>((static_cast<std::uint64_t>(value) >> (index * 8)) & 0xff));
    }
}

void WriteProbeWav(const fs::path &path, int sample_count = 4800) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write("RIFF", 4);
    WriteLittle<std::uint32_t>(output, 36 + sample_count * 4);
    output.write("WAVEfmt ", 8);
    WriteLittle<std::uint32_t>(output, 16);
    WriteLittle<std::uint16_t>(output, 1);
    WriteLittle<std::uint16_t>(output, 2);
    WriteLittle<std::uint32_t>(output, 48000);
    WriteLittle<std::uint32_t>(output, 48000 * 4);
    WriteLittle<std::uint16_t>(output, 4);
    WriteLittle<std::uint16_t>(output, 16);
    output.write("data", 4);
    WriteLittle<std::uint32_t>(output, sample_count * 4);
    for (int frame = 0; frame < sample_count; ++frame) {
        const auto sample = static_cast<std::int16_t>(std::sin(frame * 0.07) * 12000.0);
        WriteLittle<std::uint16_t>(output, static_cast<std::uint16_t>(sample));
        WriteLittle<std::uint16_t>(output, static_cast<std::uint16_t>(sample));
    }
    ASSERT_TRUE(output.good()) << path.string();
}

const moonmod::ApiOperationSpec &Operation(std::string_view path) {
    const moonmod::ApiOperationSpec *operation = moonmod::FindApiOperation(path);
    if (operation == nullptr) throw std::runtime_error("missing audio API operation");
    return *operation;
}

void Render(audio::Mixer &mixer, std::vector<float> &samples, int frames = 1024) {
    samples.resize(static_cast<std::size_t>(frames) * 2);
    mixer.Render(samples.data(), frames);
}

bool HasSignal(const std::vector<float> &samples) {
    return std::ranges::any_of(samples, [](float sample) { return std::abs(sample) > 0.0001f; });
}

} // namespace

TEST(MoonModAudio, PackageSoundMixesStopsAndIsCleanedUpByOwner) {
    TempRoot root;
    const fs::path package = root.path / "mods" / "test.audio";
    WriteProbeWav(package / "resources" / "audio" / "tone.wav");

    audio::Mixer mixer;
    moonmod::AudioApi api(mixer);
    const moonmod::Json definition = {{"id", "tone"}, {"path", "resources/audio/tone.wav"}};
    const auto registered = api.Call("test.audio", package, Operation("audio.register"), moonmod::Json::array({definition}));
    ASSERT_EQ(registered.status, moonmod::ApiCallStatus::Ok) << registered.message;
    EXPECT_EQ(registered.value.at("qualified_id"), "test.audio:tone");

    const auto played = api.Call("test.audio", package, Operation("audio.play"),
                                 moonmod::Json::array({moonmod::Json{{"id", "tone"}, {"loop", true}, {"volume", 0.5}, {"pan", -0.25}}}));
    ASSERT_EQ(played.status, moonmod::ApiCallStatus::Ok) << played.message;
    const std::string handle = played.value.at("handle").get<std::string>();
    std::vector<float> rendered;
    Render(mixer, rendered);
    EXPECT_TRUE(HasSignal(rendered));

    const auto foreign_stop = api.Call("other.mod", package, Operation("audio.stop"),
                                       moonmod::Json::array({moonmod::Json{{"handle", handle}}}));
    EXPECT_EQ(foreign_stop.status, moonmod::ApiCallStatus::PermissionDenied);

    const auto stopped = api.Call("test.audio", package, Operation("audio.stop"),
                                  moonmod::Json::array({moonmod::Json{{"handle", handle}}}));
    ASSERT_EQ(stopped.status, moonmod::ApiCallStatus::Ok) << stopped.message;
    Render(mixer, rendered);
    EXPECT_FALSE(HasSignal(rendered));

    const auto invalid_path = api.Call("test.audio", package, Operation("audio.register"),
                                       moonmod::Json::array({moonmod::Json{{"id", "escape"}, {"path", "resources/audio/../../outside.wav"}}}));
    EXPECT_EQ(invalid_path.status, moonmod::ApiCallStatus::InvalidArgument);

    const auto looping = api.Call("test.audio", package, Operation("audio.play"),
                                  moonmod::Json::array({moonmod::Json{{"id", "tone"}, {"loop", true}}}));
    ASSERT_EQ(looping.status, moonmod::ApiCallStatus::Ok) << looping.message;
    Render(mixer, rendered);
    EXPECT_TRUE(HasSignal(rendered));
    api.UnloadOwner("test.audio");
    Render(mixer, rendered);
    EXPECT_FALSE(HasSignal(rendered));
    const auto after_unload = api.Call("test.audio", package, Operation("audio.play"),
                                       moonmod::Json::array({moonmod::Json{{"id", "tone"}}}));
    EXPECT_EQ(after_unload.status, moonmod::ApiCallStatus::InvalidArgument);
}

TEST(MoonModAudio, LuaPackageCallsAudioApiAndHeadlessMixerAdvancesIt) {
    moonmod::SharedRuntime().Stop();
    TempRoot root;
    const fs::path package = root.path / "mods" / "test.audio-lua";
    Write(package / "mod.json",
          R"json({"schema_version":1,"id":"test.audio-lua","name":"Audio Lua Probe","version":"1.0.0","framework_api":1,"games":["chronicle","chronicle-two"],"entry":"main.lua","permissions":["content.register","game.write"]})json");
    WriteProbeWav(package / "resources" / "audio" / "tone.wav");
    Write(package / "main.lua", R"lua(
assert(game.audio.register({id = "tone", path = "resources/audio/tone.wav"}))
local first = assert(game.audio.play({id = "tone", loop = true, volume = 0.5}))
game.events.subscribe("tests.audio.stop", function()
  assert(game.audio.stop({handle = first.handle}))
  local second = assert(game.audio.play({id = "tone", loop = true}))
  assert(second.handle ~= first.handle)
end)
)lua");
    Write(root.path / "save" / "moonmod" / "profile.json",
          R"json({"format_version":1,"enabled":{"test.audio-lua":true},"settings":{}})json");

    audio::Mixer mixer;
    moonmod::AudioApi audio_api(mixer);
    moonmod::RuntimeConfig config;
    config.game_id = "chronicle-two";
    config.game_name = "Chronicle Two";
    config.mod_directories = {root.path / "mods"};
    config.save_root = root.path / "save";
    for (const char *path : {"audio.register", "audio.play", "audio.stop"}) {
        config.api_capabilities.push_back({path, true, {}});
    }
    config.package_api_handler = [&audio_api](std::string_view mod_id, const fs::path &package_root,
                                              const moonmod::ApiOperationSpec &operation,
                                              const moonmod::Json &arguments) {
        return audio_api.Call(mod_id, package_root, operation, arguments);
    };
    config.api_owner_unloaded = [&audio_api](std::string_view mod_id) { audio_api.UnloadOwner(mod_id); };
    std::vector<std::string> errors;
    config.log = [&errors](std::string_view level, std::string_view, std::string_view message) {
        if (level == "error") errors.emplace_back(message);
    };
    std::string error;
    moonmod::Runtime runtime;
    ASSERT_TRUE(runtime.Start(std::move(config), error)) << error;
    ASSERT_EQ(runtime.Mods().size(), 1u);
    ASSERT_EQ(runtime.Mods()[0].state, moonmod::ModState::Loaded) << runtime.Mods()[0].status_message;

    std::vector<float> rendered;
    Render(mixer, rendered);
    EXPECT_TRUE(HasSignal(rendered));
    ASSERT_TRUE(runtime.PublishToMod("test.audio-lua", "tests.audio.stop", moonmod::Json::object()));
    EXPECT_TRUE(errors.empty()) << (errors.empty() ? "" : errors.front());
    Render(mixer, rendered);
    EXPECT_TRUE(HasSignal(rendered));
    runtime.Stop();
    Render(mixer, rendered);
    EXPECT_FALSE(HasSignal(rendered));
}

TEST(MoonModAudio, PackageWithoutAudioPermissionsCannotRegisterOrPlay) {
    moonmod::SharedRuntime().Stop();
    TempRoot root;
    const fs::path package = root.path / "mods" / "test.audio-denied";
    Write(package / "mod.json",
          R"json({"schema_version":1,"id":"test.audio-denied","name":"Audio Permission Probe","version":"1.0.0","framework_api":1,"games":["chronicle","chronicle-two"],"entry":"main.lua","permissions":[]})json");
    WriteProbeWav(package / "resources" / "audio" / "tone.wav");
    Write(package / "main.lua", R"lua(
local registered, register_error = game.audio.register({id = "tone", path = "resources/audio/tone.wav"})
assert(registered == nil and register_error.code == "permission_denied")
local played, play_error = game.audio.play({id = "tone"})
assert(played == nil and play_error.code == "permission_denied")
)lua");
    Write(root.path / "save" / "moonmod" / "profile.json",
          R"json({"format_version":1,"enabled":{"test.audio-denied":true},"settings":{}})json");

    audio::Mixer mixer;
    moonmod::AudioApi audio_api(mixer);
    moonmod::RuntimeConfig config;
    config.game_id = "chronicle";
    config.game_name = "Chronicle";
    config.mod_directories = {root.path / "mods"};
    config.save_root = root.path / "save";
    for (const char *path : {"audio.register", "audio.play", "audio.stop"}) {
        config.api_capabilities.push_back({path, true, {}});
    }
    config.package_api_handler = [&audio_api](std::string_view mod_id, const fs::path &package_root,
                                              const moonmod::ApiOperationSpec &operation,
                                              const moonmod::Json &arguments) {
        return audio_api.Call(mod_id, package_root, operation, arguments);
    };
    config.api_owner_unloaded = [&audio_api](std::string_view mod_id) { audio_api.UnloadOwner(mod_id); };
    std::string error;
    moonmod::Runtime runtime;
    ASSERT_TRUE(runtime.Start(std::move(config), error)) << error;
    ASSERT_EQ(runtime.Mods().size(), 1u);
    ASSERT_EQ(runtime.Mods()[0].state, moonmod::ModState::Loaded) << runtime.Mods()[0].status_message;
    runtime.Stop();
}

