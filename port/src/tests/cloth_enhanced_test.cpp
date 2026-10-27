#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "cloth.hpp"
#include "frame.hpp"
#include "platform/config.hpp"
#include "platform/paths.hpp"
#include "platform_fixture.hpp"

namespace dc::test {

using namespace dc::game;
using namespace dc::platform;

namespace {

// A body with one leg, a joint and its end, and a cloth hanging in front of it from a frame of its own.
struct Body {
    CFrame root;
    CFrame chain;
    CFrame hip;
    CFrame knee;
    CFrame end;
    CFrame hang;

    Body() {
        for (CFrame *frame : {&root, &chain, &hip, &knee, &end, &hang}) {
            frame->Initialize();
        }
        std::strcpy(root.name, "body");
        std::strcpy(chain.name, "chn5");
        std::strcpy(hip.name, "jnt5_1");
        std::strcpy(knee.name, "jnt5_2");
        std::strcpy(end.name, "eff5");
        std::strcpy(hang.name, "cloth1");
        chain.SetParent(&root);
        hip.SetParent(&chain);
        knee.SetParent(&hip);
        end.SetParent(&knee);
        hang.SetParent(&root);
        hip.local[3][1] = -2.0f;
        knee.local[3][1] = -2.0f;
        end.local[3][1] = -2.0f;
    }
};

// The leg runs from (0, -2, 0) through the knee to (0, -6, 0); each half's capsule is 0.2 of its length
// thick, at least 0.4.
constexpr float kLegRadius = 0.4f;

float FromLeg(const float *point) {
    const float y = std::clamp(point[1], -6.0f, -2.0f);
    return std::hypot(point[0], point[1] - y, point[2]);
}

// A 5 × 6 cloth in front of the leg, pulled back through it by a strong wind-like gravity.
std::unique_ptr<CCloth> HangCloth(CFrame &hang) {
    auto cloth = std::make_unique<CCloth>(16, 16, 1.0f);
    cloth->num_i = 5;
    cloth->num_j = 6;
    cloth->frame = &hang;
    cloth->bound = nullptr;
    cloth->wind = nullptr;
    cloth->floor_on = 0;
    cloth->stop = 0;
    cloth->normal_scale = 1.0f;
    for (int axis = 0; axis < 3; axis++) {
        cloth->stiffness[axis] = 0.005f;
        cloth->follow[axis] = 0.0f;
    }
    cloth->gravity[0] = 0.0f;
    cloth->gravity[1] = -0.05f;
    cloth->gravity[2] = -0.2f;
    for (int i = 0; i < 5; i++) {
        for (int j = 0; j < 6; j++) {
            cloth->home[i][j][0] = static_cast<float>(i) - 2.0f;
            cloth->home[i][j][1] = -static_cast<float>(j);
            cloth->home[i][j][2] = 2.2f;
            cloth->home[i][j][3] = 1.0f;
            cloth->mask[i][j] = 0;
            cloth->rest[i][j][0] = 1.0f;
            cloth->rest[i][j][1] = 1.0f;
        }
    }
    cloth->position[0] = 0.0f;
    cloth->position[1] = 0.0f;
    cloth->position[2] = 2.2f;
    cloth->position[3] = 1.0f;
    cloth->Step(-1);
    return cloth;
}

float Nearest(const CCloth &cloth, float (*from)(const float *) = FromLeg) {
    float nearest = 1e9f;
    for (int i = 0; i < cloth.num_i; i++) {
        for (int j = 0; j < cloth.num_j; j++) {
            nearest = std::min(nearest, from(cloth.point[i][j]));
        }
    }
    return nearest;
}

struct ConfigScope {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
                                 ("chronicle-cloth-test-" + std::to_string(dc::test::ProcessId()));

    explicit ConfigScope(bool enhanced) {
        PathsSetSaveRoot(root);
        Config config;
        config.enhanced_cloth = enhanced;
        EXPECT_TRUE(ConfigChange(config));
    }

    ~ConfigScope() {
        ConfigChange(Config{});
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
};

// A torso along y from (0, -1, 0) to (0, -5, 0), 1.6 thick, and an arm across in front of it along x at
// height -3, its axis at the given depth, 0.6 thick.
std::vector<cloth_body::Capsule> ArmBeforeChest(float arm_depth = 2.0f, bool torso = true) {
    std::vector<cloth_body::Capsule> capsules = {
        {{-3.0f, -3.0f, arm_depth}, {3.0f, -3.0f, arm_depth}, 0.6f}
    };
    if (torso) {
        capsules.push_back({
            {0.0f, -1.0f, 0.0f},
            {0.0f, -5.0f, 0.0f},
            1.6f, true
        });
    }
    return capsules;
}

// Rests high on the chest, well away from the arm.
constexpr float kHome[3] = {0.0f, -1.5f, 2.5f};

// Pushes `vertex` out of `capsules`, with the limbs' clearances taken when the cloth settled on the body
// as `settled` has it (the same body unless a limb has moved since).
void Push(const std::vector<cloth_body::Capsule> &capsules, const std::vector<cloth_body::Capsule> &settled, const float *home,
          float *vertex) {
    std::vector<float> clearances;
    for (const cloth_body::Capsule &capsule : settled) {
        clearances.push_back(capsule.torso ? 0.0f : cloth_body::Clearance(capsule, home));
    }
    cloth_body::PushOutOfBody(capsules, clearances, home, vertex);
}

void Push(const std::vector<cloth_body::Capsule> &capsules, const float *home, float *vertex) {
    Push(capsules, capsules, home, vertex);
}

} // namespace

TEST(ClothEnhanced, KeepsTheClothOutOfTheBody) {
    ConfigScope config(true);
    Body        body;
    auto        cloth = HangCloth(body.hang);
    for (int step = 0; step < 120; step++) {
        cloth->Step(0);
        EXPECT_GE(Nearest(*cloth), kLegRadius - 1e-3f) << "step " << step;
    }
    // Pinned along its first column, where the frame holds it.
    for (int i = 0; i < cloth->num_i; i++) {
        EXPECT_NEAR(cloth->point[i][0][2], 2.2f, 1e-4f);
    }
}

TEST(ClothEnhanced, TheOriginalClothPassesThroughIt) {
    ConfigScope config(false);
    Body        body;
    auto        cloth = HangCloth(body.hang);
    float       nearest = 1e9f;
    for (int step = 0; step < 120; step++) {
        cloth->Step(0);
        nearest = std::min(nearest, Nearest(*cloth));
    }
    EXPECT_LT(nearest, kLegRadius);
}

TEST(ClothEnhanced, PushesAVertexOutOfAnArm) {
    float vertex[3] = {0.0f, -3.0f, 2.3f};
    Push(ArmBeforeChest(), kHome, vertex);
    EXPECT_NEAR(vertex[2], 2.65f, 1e-4f);
}

TEST(ClothEnhanced, AnArmPressesTheClothOnlyAsFarAsTheChest) {
    // Between the arm and the chest: pushed in, onto the chest's surface and no further.
    float vertex[3] = {0.0f, -3.0f, 1.8f};
    Push(ArmBeforeChest(), kHome, vertex);
    EXPECT_NEAR(vertex[0], 0.0f, 1e-4f);
    EXPECT_NEAR(vertex[1], -3.0f, 1e-4f);
    EXPECT_NEAR(vertex[2], 1.65f, 1e-4f);
}

TEST(ClothEnhanced, AnArmNeverPressesTheClothDeeperThanItRests) {
    // Resting inside the chest's capsule, and sunk nearer than that: back to its rest depth, and the arm
    // in front doesn't press it past it.
    const float home[3] = {0.0f, -1.5f, 1.6f};
    float       vertex[3] = {0.0f, -3.0f, 1.5f};
    Push(ArmBeforeChest(1.7f), home, vertex);
    EXPECT_NEAR(vertex[2], 1.6f, 1e-4f);
}

TEST(ClothEnhanced, KeepsTheClothOffTheChest) {
    float vertex[3] = {0.0f, -3.0f, 0.5f};
    Push(ArmBeforeChest(6.0f), kHome, vertex);
    EXPECT_NEAR(vertex[2], 1.65f, 1e-4f);
}

TEST(ClothEnhanced, ClothRestingInsideTheChestGoesNoDeeperThanItRests) {
    // Resting 1.2 from the spine, inside the chest's 1.65: sunk to 0.5, it goes back to 1.2, not 1.65.
    const float home[3] = {0.0f, -1.5f, 1.2f};
    float       vertex[3] = {0.0f, -3.0f, 0.5f};
    Push(ArmBeforeChest(6.0f), home, vertex);
    EXPECT_NEAR(vertex[2], 1.2f, 1e-4f);
    float resting[3] = {0.0f, -3.0f, 1.2f};
    Push(ArmBeforeChest(6.0f), home, resting);
    EXPECT_NEAR(resting[2], 1.2f, 1e-6f);
}

TEST(ClothEnhanced, ClothRestingAgainstAnArmGoesNoDeeperThanItRests) {
    // Resting 0.3 from the arm's axis, inside its 0.65: pressed to 0.2, it goes back to 0.3, not 0.65.
    const float home[3] = {0.0f, -3.0f, 2.3f};
    float       vertex[3] = {0.0f, -3.0f, 1.8f};
    Push(ArmBeforeChest(), home, vertex);
    EXPECT_NEAR(vertex[2], 1.7f, 1e-4f);
    float resting[3] = {home[0], home[1], home[2]};
    Push(ArmBeforeChest(), home, resting);
    EXPECT_NEAR(resting[2], 2.3f, 1e-6f);
}

TEST(ClothEnhanced, AnArmRaisedThroughWhereTheClothRestsPushesIt) {
    // The cloth settled with the arm far off; the arm then comes up through where the cloth rests.
    const float home[3] = {0.0f, -3.0f, 2.5f};
    float       vertex[3] = {0.0f, -3.0f, 2.5f};
    Push(ArmBeforeChest(2.4f), ArmBeforeChest(6.0f), home, vertex);
    EXPECT_NEAR(vertex[2], 3.05f, 1e-4f);
}

TEST(ClothEnhanced, WithoutATorsoAnArmPushesAllTheWay) {
    float vertex[3] = {0.0f, -3.0f, 1.8f};
    Push(ArmBeforeChest(2.0f, false), kHome, vertex);
    EXPECT_NEAR(vertex[2], 1.35f, 1e-4f);
}

namespace {

// A box from `low` to `high`, as twelve triangles.
void AddBox(cloth_body::Mesh &mesh, std::array<float, 3> low, std::array<float, 3> high) {
    const auto first = static_cast<std::uint32_t>(mesh.vertices.size());
    for (int corner = 0; corner < 8; corner++) {
        mesh.vertices.push_back({(corner & 1) ? high[0] : low[0], (corner & 2) ? high[1] : low[1], (corner & 4) ? high[2] : low[2]});
    }
    // Each side, as its two corners varying.
    for (const auto &[fixed, u, v] : {
             std::array{0, 1, 2},
             std::array{1, 0, 2},
             std::array{2, 0, 1}
    }) {
        for (int at : {0, 1}) {
            auto index = [&](int du, int dv) {
                return first + static_cast<std::uint32_t>((at << fixed) | (du << u) | (dv << v));
            };
            mesh.triangles.push_back({index(0, 0), index(1, 0), index(1, 1)});
            mesh.triangles.push_back({index(0, 0), index(1, 1), index(0, 1)});
        }
    }
}

// A torso along y from (0, -1, 0) to (0, -5, 0), 1.6 thick before it is fitted, its side towards +x.
cloth_body::Capsule Torso() {
    cloth_body::Capsule torso = {
        {0.0f, -1.0f, 0.0f},
        {0.0f, -5.0f, 0.0f},
        1.6f, true
    };
    torso.side = {1.0f, 0.0f, 0.0f};
    return torso;
}

// A body round that torso 2 wide, its chest 1.2 in front of the spine (+z) and its back 0.6 behind.
cloth_body::Mesh Chest() {
    cloth_body::Mesh mesh;
    AddBox(mesh, {-1.0f, -5.5f, -0.6f}, {1.0f, -0.5f, 1.2f});
    return mesh;
}

float ReachTowards(const cloth_body::Capsule &torso, float x, float z) {
    const float point[3] = {x, -3.0f, z};
    return cloth_body::Reach(torso, point);
}

} // namespace

TEST(ClothEnhanced, FitsTheTorsoToTheBody) {
    std::vector<cloth_body::Capsule> capsules = {Torso()};
    cloth_body::FitTorsos(capsules, {}, Chest());
    ASSERT_TRUE(capsules[0].fitted);
    // Between its sectors the shape runs straight from one to the next, across a flat side a little
    // further out than the side itself.
    EXPECT_NEAR(ReachTowards(capsules[0], 0.0f, 5.0f), 1.2f, 0.05f);
    EXPECT_NEAR(ReachTowards(capsules[0], 0.0f, -5.0f), 0.6f, 0.05f);
    EXPECT_NEAR(ReachTowards(capsules[0], 5.0f, 0.0f), 1.0f, 0.05f);
}

TEST(ClothEnhanced, AFittedBackLetsTheClothLieOnIt) {
    std::vector<cloth_body::Capsule> capsules = {Torso()};
    cloth_body::FitTorsos(capsules, {}, Chest());
    const float home[3] = {0.0f, -3.0f, -1.5f};
    // Behind the back, where the round torso would have pushed it out to 1.65.
    float lying[3] = {0.0f, -3.0f, -0.8f};
    cloth_body::PushOutOfBody(capsules, std::vector<float>(1), home, lying);
    EXPECT_NEAR(lying[2], -0.8f, 1e-6f);
    // In the back, out onto it.
    float inside[3] = {0.0f, -3.0f, -0.3f};
    cloth_body::PushOutOfBody(capsules, std::vector<float>(1), home, inside);
    EXPECT_NEAR(inside[2], -0.65f, 0.05f);
}

TEST(ClothEnhanced, AHandAgainstTheTorsoDoesNotWidenIt) {
    std::vector<cloth_body::Capsule> capsules = {Torso()};
    // Hanging at the torso's side, in the way of the directions either side of +x.
    const cloth_body::Capsule hand = {
        {2.1f, -2.0f, 0.0f},
        {2.1f, -4.0f, 0.0f},
        0.6f
    };
    cloth_body::Mesh body = Chest();
    AddBox(body, {1.8f, -4.2f, -0.8f}, {2.4f, -1.8f, 0.8f});
    cloth_body::FitTorsos(capsules, std::span(&hand, 1), body);
    EXPECT_NEAR(ReachTowards(capsules[0], 5.0f, 0.0f), 1.0f, 0.05f);
}

TEST(ClothEnhanced, ATorsoWithoutAMeshStaysRound) {
    std::vector<cloth_body::Capsule> capsules = {Torso()};
    cloth_body::FitTorsos(capsules, {}, cloth_body::Mesh{});
    EXPECT_FALSE(capsules[0].fitted);
    EXPECT_FLOAT_EQ(ReachTowards(capsules[0], 0.0f, -5.0f), 1.6f);
}

} // namespace dc::test
