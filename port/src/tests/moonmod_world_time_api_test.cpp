#include <gtest/gtest.h>

#include <algorithm>
#include <memory>

#include "editloop.hpp"
#include "main.hpp"
#include "moonmod/chronicle_adapter.hpp"
#include "savedata.hpp"

namespace dc::game {}
using namespace dc::game;


TEST(MoonModWorldTimeApi, ReadsAndSetsNormalizedNativeTimeAndRejectsInvalidInput) {
    CSaveData *previous_save = SaveData;
    const s32 previous_mode = mode;
    const float previous_time = NowTime;
    struct Restore {
        CSaveData *save;
        s32 game_mode;
        float editor_time;
        ~Restore() {
            SaveData = save;
            mode = game_mode;
            NowTime = editor_time;
        }
    } restore{previous_save, previous_mode, previous_time};

    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);
    const moonmod::ApiOperationSpec *get_operation = moonmod::FindApiOperation("world.get");
    const moonmod::ApiOperationSpec *set_operation = moonmod::FindApiOperation("world.set_time");
    ASSERT_NE(get_operation, nullptr);
    ASSERT_NE(set_operation, nullptr);
    EXPECT_TRUE(std::ranges::any_of(config.api_capabilities, [](const moonmod::ApiCapability &capability) {
        return capability.path == "world.set_time" && capability.supported;
    }));

    SaveData = nullptr;
    EXPECT_EQ(config.api_handler("test.moonmod.world-time", *set_operation,
                                 moonmod::Json::array({moonmod::Json{{"fraction", 0.5}}})).status,
              moonmod::ApiCallStatus::Failed);

    auto save = std::make_unique<CSaveData>();
    save->Initialize();
    save->GetDngStatus()->cur_georama = 2;
    save->SetNowTime(3.0f);
    SaveData = save.get();
    mode = GAME_MODE_EDIT;
    NowTime = 3.0f;

    const moonmod::ApiCallResult before = config.api_handler(
        "test.moonmod.world-time", *get_operation, moonmod::Json::array());
    ASSERT_EQ(before.status, moonmod::ApiCallStatus::Ok) << before.message;
    EXPECT_EQ(before.value.at("time").at("native_cycle_hours"), 12);
    EXPECT_NEAR(before.value.at("time").at("fraction").get<double>(), 0.25, 0.00001);
    EXPECT_NEAR(before.value.at("time").at("native_time").get<double>(), 3.0, 0.00001);

    const moonmod::ApiCallResult set = config.api_handler(
        "test.moonmod.world-time", *set_operation,
        moonmod::Json::array({moonmod::Json{{"fraction", 0.75}}}));
    ASSERT_EQ(set.status, moonmod::ApiCallStatus::Ok) << set.message;
    EXPECT_NEAR(SaveData->GetNowTime(), 9.0, 0.00001);
    EXPECT_NEAR(NowTime, 9.0, 0.00001);
    EXPECT_NEAR(set.value.at("time").at("fraction").get<double>(), 0.75, 0.00001);

    const moonmod::ApiCallResult invalid = config.api_handler(
        "test.moonmod.world-time", *set_operation,
        moonmod::Json::array({moonmod::Json{{"fraction", 1.0}}}));
    EXPECT_EQ(invalid.status, moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_NEAR(SaveData->GetNowTime(), 9.0, 0.00001);

    const moonmod::ApiCallResult extra_field = config.api_handler(
        "test.moonmod.world-time", *set_operation,
        moonmod::Json::array({moonmod::Json{{"fraction", 0.5}, {"day", 4}}}));
    EXPECT_EQ(extra_field.status, moonmod::ApiCallStatus::InvalidArgument);
}
