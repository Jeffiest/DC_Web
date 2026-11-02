#include <gtest/gtest.h>

#include <ranges>

#include "camerafollow.hpp"
#include "main.hpp"
#include "dun/gameloop.hpp"
#include "moonmod/chronicle_adapter.hpp"

namespace dc::game {}
using namespace dc::game;


TEST(MoonModRenderApi, ReadsActiveDungeonCameraPoseAndRejectsUnavailableContext) {
    const s32 previous_mode = mode;
    CCameraFollow *const previous_camera = NowCamera__3;
    CCameraFollow camera(20.0f, 5.0f, 0.5f, 8.0f);
    struct Restore {
        s32 mode;
        CCameraFollow *camera;
        ~Restore() {
            ::mode = mode;
            NowCamera__3 = camera;
        }
    } restore{previous_mode, previous_camera};

    camera.pos[0] = 11.0f;
    camera.pos[1] = 22.0f;
    camera.pos[2] = 33.0f;
    camera.ref[0] = -4.0f;
    camera.ref[1] = 5.0f;
    camera.ref[2] = 6.0f;
    camera.angle_h = 0.25f;
    camera.angle_v = -0.5f;
    camera.roll = 0.125f;
    NowCamera__3 = &camera;
    mode = GAME_MODE_DUNGEON;

    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);
    const moonmod::ApiOperationSpec *operation = moonmod::FindApiOperation("render.camera_get");
    ASSERT_NE(operation, nullptr);
    EXPECT_TRUE(std::ranges::any_of(config.api_capabilities, [](const moonmod::ApiCapability &capability) {
        return capability.path == "render.camera_get" && capability.supported;
    }));

    const moonmod::ApiCallResult result = config.api_handler(
        "test.moonmod.render-probe", *operation, moonmod::Json::array());
    ASSERT_EQ(result.status, moonmod::ApiCallStatus::Ok) << result.message;
    EXPECT_EQ(result.value.value("scene", ""), "dungeon");
    EXPECT_NEAR(result.value.at("eye").at("x").get<float>(), 11.0f, 0.00001f);
    EXPECT_NEAR(result.value.at("eye").at("y").get<float>(), 22.0f, 0.00001f);
    EXPECT_NEAR(result.value.at("eye").at("z").get<float>(), 33.0f, 0.00001f);
    EXPECT_NEAR(result.value.at("target").at("x").get<float>(), -4.0f, 0.00001f);
    EXPECT_NEAR(result.value.at("heading").get<float>(), 0.25f, 0.00001f);
    EXPECT_NEAR(result.value.at("pitch").get<float>(), -0.5f, 0.00001f);
    EXPECT_NEAR(result.value.at("roll").get<float>(), 0.125f, 0.00001f);

    const moonmod::ApiOperationSpec *set_operation = moonmod::FindApiOperation("render.camera_set");
    ASSERT_NE(set_operation, nullptr);
    EXPECT_TRUE(std::ranges::any_of(config.api_capabilities, [](const moonmod::ApiCapability &capability) {
        return capability.path == "render.camera_set" && capability.supported;
    }));
    const moonmod::ApiCallResult updated = config.api_handler(
        "test.moonmod.render-probe", *set_operation,
        moonmod::Json::array({moonmod::Json{{"distance", 85.0}, {"height", 25.0}, {"angle", 2.25}}}));
    ASSERT_EQ(updated.status, moonmod::ApiCallStatus::Ok) << updated.message;
    EXPECT_NEAR(camera.GetDistance(), 85.0f, 0.00001f);
    EXPECT_NEAR(camera.GetHeight(), 25.0f, 0.00001f);
    EXPECT_NEAR(camera.next_angle, 2.25f, 0.00001f);
    const moonmod::ApiCallResult invalid = config.api_handler(
        "test.moonmod.render-probe", *set_operation,
        moonmod::Json::array({moonmod::Json{{"distance", 500.0}}}));
    EXPECT_EQ(invalid.status, moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_NEAR(camera.GetDistance(), 85.0f, 0.00001f);

    camera.FollowOff();
    const moonmod::ApiCallResult no_follow = config.api_handler(
        "test.moonmod.render-probe", *set_operation,
        moonmod::Json::array({moonmod::Json{{"angle", 0.5}}}));
    EXPECT_EQ(no_follow.status, moonmod::ApiCallStatus::Failed);

    mode = GAME_MODE_EDIT;
    const moonmod::ApiCallResult unavailable = config.api_handler(
        "test.moonmod.render-probe", *operation, moonmod::Json::array());
    EXPECT_EQ(unavailable.status, moonmod::ApiCallStatus::Failed);
}
