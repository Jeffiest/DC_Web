#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "moonmod/manager_ui.hpp"
#include "moonmod/manifest.hpp"
#include "moonmod/native_plugin_hash.hpp"
#include "moonmod/runtime.hpp"



#ifndef MOONMOD_TEST_NATIVE_PLUGIN_PATH
#error MOONMOD_TEST_NATIVE_PLUGIN_PATH must name the C fixture library
#endif
#ifndef MOONMOD_TEST_GAME_ID
#error MOONMOD_TEST_GAME_ID must select the tested game adapter
#endif

namespace {

namespace fs = std::filesystem;

struct TempRoot {
    fs::path path = fs::temp_directory_path() /
                    ("moonmod-native-runtime-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ~TempRoot() {
        moonmod::SharedRuntime().Stop();
        moonmod::CloseManager();
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

void Write(const fs::path &path, std::string_view content) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << content;
    ASSERT_TRUE(output.good()) << path.string();
}

bool HasLog(const std::vector<std::string> &logs, std::string_view wanted) {
    return std::ranges::find(logs, wanted) != logs.end();
}

int CountLog(const std::vector<std::string> &logs, std::string_view wanted) {
    return static_cast<int>(std::ranges::count(logs, wanted));
}

} // namespace

TEST(MoonModNativePluginHash, FingerprintsLibraryContentsWithSha256) {
    TempRoot root;
    const fs::path file = root.path / "digest-input.bin";
    Write(file, "abc");
    EXPECT_EQ(moonmod::NativePluginFileSha256(file),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const fs::path empty = root.path / "empty.bin";
    Write(empty, "");
    EXPECT_TRUE(moonmod::NativePluginFileSha256(empty).empty());
}

TEST(MoonModNativePluginRuntime, RequiresPackageContainedLibraryAndExplicitPermission) {
    TempRoot root;
    const fs::path escape_package = root.path / "escape";
    Write(escape_package / "main.lua", "return {}\n");
    const moonmod::Json escape_manifest = {
        {"schema_version", 1}, {"id", "test.native-escape"}, {"name", "Native Escape"},
        {"version", "1.0.0"}, {"framework_api", 1}, {"games", moonmod::Json::array({MOONMOD_TEST_GAME_ID})},
        {"entry", "main.lua"}, {"permissions", moonmod::Json::array({"native.extension"})},
        {"native_extension", {{"library", "../outside.dll"}}}
    };
    Write(escape_package / "mod.json", escape_manifest.dump());
    std::string error;
    EXPECT_FALSE(moonmod::ReadManifest(escape_package, error).has_value());
    EXPECT_NE(error.find("safe relative"), std::string::npos);

    const fs::path permission_package = root.path / "missing-permission";
    Write(permission_package / "main.lua", "return {}\n");
    const moonmod::Json permission_manifest = {
        {"schema_version", 1}, {"id", "test.native-permission"}, {"name", "Native Permission"},
        {"version", "1.0.0"}, {"framework_api", 1}, {"games", moonmod::Json::array({MOONMOD_TEST_GAME_ID})},
        {"entry", "main.lua"}, {"permissions", moonmod::Json::array({"game.read"})},
        {"native_extension", {{"library", "native/probe.dll"}}}
    };
    Write(permission_package / "mod.json", permission_manifest.dump());
    error.clear();
    EXPECT_FALSE(moonmod::ReadManifest(permission_package, error).has_value());
    EXPECT_NE(error.find("native.extension manifest permission"), std::string::npos);
}

TEST(MoonModNativePluginRuntime, RequiresLocalTrustAndCleansServicesAcrossReload) {
    using namespace moonmod;
    SharedRuntime().Stop();
    CloseManager();
    TempRoot root;
    const fs::path package = root.path / "mods" / "test.native-probe";
    const fs::path library = package / "native" / fs::path(MOONMOD_TEST_NATIVE_PLUGIN_PATH).filename();
    fs::create_directories(library.parent_path());
    std::error_code copy_error;
    fs::copy_file(fs::path(MOONMOD_TEST_NATIVE_PLUGIN_PATH), library, fs::copy_options::overwrite_existing, copy_error);
    ASSERT_FALSE(copy_error) << copy_error.message();
    const fs::path library_name = fs::path("native") / library.filename();
    const Json manifest = {
        {"schema_version", 1}, {"id", "test.native-probe"}, {"name", "Native Probe"},
        {"version", "1.0.0"}, {"framework_api", 1}, {"games", Json::array({MOONMOD_TEST_GAME_ID})},
        {"entry", "main.lua"},
        {"permissions", Json::array({"game.read", "game.write", "content.register", "native.extension"})},
        {"native_extension", {{"library", library_name.generic_string()}}}
    };
    Write(package / "mod.json", manifest.dump());
    Write(package / "main.lua",
          "assert(game.scripts.register_function({id = 'native-visible', description = 'Native caller probe'}, "
          "function(arguments) return {value = arguments.value * 2} end))\nreturn {}\n");
    Write(root.path / "save" / "moonmod" / "profile.json",
          R"json({"format_version":1,"enabled":{"test.native-probe":true},"settings":{}})json");

    std::vector<std::string> logs;
    RuntimeConfig config;
    config.game_id = MOONMOD_TEST_GAME_ID;
    config.game_name = "Native Plugin Test";
    config.mod_directories = {root.path / "mods"};
    config.save_root = root.path / "save";
    config.api_capabilities.push_back({"economy.get", true, {}});
    config.api_handler = [](std::string_view, const ApiOperationSpec &operation, const Json &arguments) {
        if (operation.path == "economy.get" && arguments.is_array() && arguments.empty()) {
            return ApiCallResult{ApiCallStatus::Ok, Json{{"gilda", 42}}, {}};
        }
        return ApiCallResult{ApiCallStatus::InvalidArgument, nullptr, "unexpected native test call"};
    };
    config.log = [&](std::string_view, std::string_view, std::string_view message) { logs.emplace_back(message); };
    std::string error;
    ASSERT_TRUE(SharedRuntime().Start(config, error)) << error;
    ASSERT_EQ(SharedRuntime().Mods().size(), 1u);
    EXPECT_EQ(SharedRuntime().Mods()[0].state, ModState::Loaded);
    EXPECT_FALSE(SharedRuntime().Mods()[0].native_trusted);
    EXPECT_NE(SharedRuntime().Mods()[0].native_status.find("disabled"), std::string::npos);
    EXPECT_FALSE(HasLog(logs, "native fixture loaded"));
    SharedRuntime().Pump();
    EXPECT_EQ(CountLog(logs, "native fixture tick callback"), 0);

    OpenManager();
    ASSERT_TRUE(HandleManagerInput(ManagerInput{.next_focus = true}));
    EXPECT_TRUE(ManagerState().native_trust_focus);
    ASSERT_TRUE(HandleManagerInput(ManagerInput{.confirm = true}));
    EXPECT_TRUE(ManagerState().native_trust_confirmation);
    ASSERT_TRUE(HandleManagerInput(ManagerInput{.confirm = true}));
    EXPECT_FALSE(ManagerState().native_trust_confirmation);
    ASSERT_TRUE(SharedRuntime().Mods()[0].native_trusted);
    EXPECT_NE(SharedRuntime().Mods()[0].native_status.find("running"), std::string::npos);
    EXPECT_TRUE(HasLog(logs, "native fixture loaded"));
    EXPECT_TRUE(HasLog(logs, "native fixture core Lua API calls passed"));
    SharedRuntime().Pump();
    EXPECT_EQ(CountLog(logs, "native fixture tick callback"), 1);
    CloseManager();

    SharedRuntime().Stop();
    logs.clear();
    ASSERT_TRUE(SharedRuntime().Start(config, error)) << error;
    ASSERT_TRUE(SharedRuntime().Mods()[0].native_trusted);
    EXPECT_TRUE(HasLog(logs, "native fixture loaded"));
    EXPECT_TRUE(HasLog(logs, "native fixture core Lua API calls passed"));
    SharedRuntime().Pump();
    EXPECT_EQ(CountLog(logs, "native fixture tick callback"), 1);
    SharedRuntime().Stop();
    EXPECT_TRUE(HasLog(logs, "native fixture unloaded"));

    std::error_code time_error;
    const auto timestamp = fs::last_write_time(library, time_error);
    ASSERT_FALSE(time_error) << time_error.message();
    fs::last_write_time(library, timestamp + std::chrono::seconds(2), time_error);
    ASSERT_FALSE(time_error) << time_error.message();
    logs.clear();
    ASSERT_TRUE(SharedRuntime().Start(config, error)) << error;
    ASSERT_FALSE(SharedRuntime().Mods()[0].native_trusted);
    EXPECT_FALSE(HasLog(logs, "native fixture loaded"));

    OpenManager();
    ASSERT_TRUE(HandleManagerInput(ManagerInput{.next_focus = true}));
    ASSERT_TRUE(ManagerState().native_trust_focus);
    ASSERT_TRUE(HandleManagerInput(ManagerInput{.confirm = true}));
    ASSERT_TRUE(ManagerState().native_trust_confirmation);
    ASSERT_TRUE(HandleManagerInput(ManagerInput{.confirm = true}));
    ASSERT_TRUE(SharedRuntime().Mods()[0].native_trusted);
    EXPECT_TRUE(HasLog(logs, "native fixture loaded"));
    EXPECT_TRUE(HasLog(logs, "native fixture core Lua API calls passed"));
    ASSERT_TRUE(HandleManagerInput(ManagerInput{.confirm = true}));
    ASSERT_TRUE(ManagerState().native_trust_confirmation);
    ASSERT_TRUE(HandleManagerInput(ManagerInput{.confirm = true}));
    EXPECT_FALSE(SharedRuntime().Mods()[0].native_trusted);
    EXPECT_TRUE(HasLog(logs, "native fixture unloaded"));
    const int callbacks_before = CountLog(logs, "native fixture tick callback");
    SharedRuntime().Pump();
    EXPECT_EQ(CountLog(logs, "native fixture tick callback"), callbacks_before);
}
