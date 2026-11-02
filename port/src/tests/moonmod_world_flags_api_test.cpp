#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <memory>

#include "moonmod/chronicle_adapter.hpp"
#include "savedata.hpp"

namespace dc::game {}
using namespace dc::game;

TEST(MoonModWorldFlagsApi, ReadsAndWritesNativeStoryAndMapFlagsWithBounds) {
    CSaveData *previous_save = SaveData;
    struct Restore {
        CSaveData *save;
        ~Restore() { SaveData = save; }
    } restore{previous_save};

    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);
    const auto *story_get = moonmod::FindApiOperation("world.story_flag_get");
    const auto *story_set = moonmod::FindApiOperation("world.story_flag_set");
    const auto *value_get = moonmod::FindApiOperation("world.story_value_get");
    const auto *value_set = moonmod::FindApiOperation("world.story_value_set");
    const auto *map_get = moonmod::FindApiOperation("world.map_flag_get");
    const auto *map_set = moonmod::FindApiOperation("world.map_flag_set");
    ASSERT_NE(story_get, nullptr);
    ASSERT_NE(story_set, nullptr);
    ASSERT_NE(value_get, nullptr);
    ASSERT_NE(value_set, nullptr);
    ASSERT_NE(map_get, nullptr);
    ASSERT_NE(map_set, nullptr);
    for (const auto path : {"world.story_flag_get", "world.story_flag_set", "world.story_value_get",
                            "world.story_value_set", "world.map_flag_get", "world.map_flag_set"}) {
        EXPECT_TRUE(std::ranges::any_of(config.api_capabilities, [path](const moonmod::ApiCapability &capability) {
            return capability.path == path && capability.supported;
        }));
    }

    SaveData = nullptr;
    EXPECT_EQ(config.api_handler("test.world-flags", *story_get, moonmod::Json::array({{{"id", 10}}})).status,
              moonmod::ApiCallStatus::Failed);
    EXPECT_EQ(config.api_handler("test.world-flags", *value_set,
                                 moonmod::Json::array({{{"id", 1}, {"value", 5}}})).status,
              moonmod::ApiCallStatus::Failed);
    EXPECT_EQ(config.api_handler("test.world-flags", *map_set,
                                 moonmod::Json::array({{{"map_id", 1}, {"flag_id", 1}, {"value", true}}})).status,
              moonmod::ApiCallStatus::Failed);

    auto save = std::make_unique<CSaveData>();
    save->Initialize();
    SaveData = save.get();
    EXPECT_FALSE(save->GetGameFlag(8191));
    const auto story_written = config.api_handler(
        "test.world-flags", *story_set, moonmod::Json::array({{{"id", 8191}, {"value", true}}}));
    ASSERT_EQ(story_written.status, moonmod::ApiCallStatus::Ok) << story_written.message;
    EXPECT_FALSE(story_written.value.at("previous").get<bool>());
    EXPECT_TRUE(story_written.value.at("value").get<bool>());
    EXPECT_TRUE(story_written.value.at("changed").get<bool>());
    EXPECT_TRUE(save->GetGameFlag(8191));
    const auto story_repeated = config.api_handler(
        "test.world-flags", *story_set, moonmod::Json::array({{{"id", 8191}, {"value", true}}}));
    ASSERT_EQ(story_repeated.status, moonmod::ApiCallStatus::Ok);
    EXPECT_FALSE(story_repeated.value.at("changed").get<bool>());
    EXPECT_TRUE(config.api_handler("test.world-flags", *story_get, moonmod::Json::array({{{"id", 8191}}}))
                    .value.at("value").get<bool>());

    const auto value_written = config.api_handler(
        "test.world-flags", *value_set,
        moonmod::Json::array({{{"id", 31}, {"value", std::numeric_limits<int>::min()}}}));
    ASSERT_EQ(value_written.status, moonmod::ApiCallStatus::Ok) << value_written.message;
    EXPECT_EQ(value_written.value.at("value"), std::numeric_limits<int>::min());
    EXPECT_EQ(save->GetGameIntFlag(31), std::numeric_limits<int>::min());
    const auto value_updated = config.api_handler(
        "test.world-flags", *value_set,
        moonmod::Json::array({{{"id", 31}, {"value", std::numeric_limits<int>::max()}}}));
    ASSERT_EQ(value_updated.status, moonmod::ApiCallStatus::Ok) << value_updated.message;
    EXPECT_EQ(value_updated.value.at("previous"), std::numeric_limits<int>::min());
    EXPECT_EQ(config.api_handler("test.world-flags", *value_get, moonmod::Json::array({{{"id", 31}}}))
                  .value.at("value"),
              std::numeric_limits<int>::max());

    const auto map_written = config.api_handler(
        "test.world-flags", *map_set,
        moonmod::Json::array({{{"map_id", 79}, {"flag_id", 511}, {"value", true}}}));
    ASSERT_EQ(map_written.status, moonmod::ApiCallStatus::Ok) << map_written.message;
    EXPECT_TRUE(save->GetMapFlag(79, 511));
    EXPECT_TRUE(config.api_handler("test.world-flags", *map_get,
                                   moonmod::Json::array({{{"map_id", 79}, {"flag_id", 511}}}))
                    .value.at("value").get<bool>());
    const auto map_cleared = config.api_handler(
        "test.world-flags", *map_set,
        moonmod::Json::array({{{"map_id", 79}, {"flag_id", 511}, {"value", false}}}));
    ASSERT_EQ(map_cleared.status, moonmod::ApiCallStatus::Ok);
    EXPECT_TRUE(map_cleared.value.at("previous").get<bool>());
    EXPECT_FALSE(map_cleared.value.at("value").get<bool>());

    EXPECT_EQ(config.api_handler("test.world-flags", *story_get, moonmod::Json::array({{{"id", 8192}}})).status,
              moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(config.api_handler("test.world-flags", *story_set,
                                 moonmod::Json::array({{{"id", 1}, {"value", 1}}})).status,
              moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(config.api_handler("test.world-flags", *value_get, moonmod::Json::array({{{"id", 32}}})).status,
              moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(config.api_handler("test.world-flags", *value_set,
                                 moonmod::Json::array({{{"id", 0}, {"value", 1.5}}})).status,
              moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(config.api_handler("test.world-flags", *map_get,
                                 moonmod::Json::array({{{"map_id", 80}, {"flag_id", 0}}})).status,
              moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(config.api_handler("test.world-flags", *map_set,
                                 moonmod::Json::array({{{"map_id", 0}, {"flag_id", 512}, {"value", false}}})).status,
              moonmod::ApiCallStatus::InvalidArgument);
}
