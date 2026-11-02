#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "moonmod/chronicle_adapter.hpp"
#include "editatra.hpp"
#include "editarea.hpp"
#include "editground.hpp"
#include "editloop.hpp"
#include "editpartsinfo.hpp"
#include "gamemode.hpp"
#include "main.hpp"
#include "savedata.hpp"

namespace dc::game {}
using namespace dc::game;


namespace {

namespace fs = std::filesystem;

struct TempRoot {
    fs::path path = fs::temp_directory_path() /
                    ("moonmod-georama-event-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ~TempRoot() {
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

int g_fake_georama_place_calls = 0;
int g_fake_georama_remove_calls = 0;
int g_fake_georama_rotation = -9;
bool g_fake_georama_reject_place = false;
bool g_fake_georama_reject_remove = false;

int FakeGeoramaPlace(CEditGround *ground, int part_id, float x, float y, float z, int rotation) {
    ++g_fake_georama_place_calls;
    g_fake_georama_rotation = rotation;
    if (g_fake_georama_reject_place) return -1;
    int slot = -1;
    for (int index = 0; index < 128; ++index) {
        if (ground->parts[index].handle < 0) {
            slot = index;
            break;
        }
    }
    EDITPARTS_INFO *plot = ground->parts_info->GetPartsInfo(part_id);
    if (slot < 0 || plot == nullptr || plot->placed >= plot->stock) return -1;
    ++plot->placed;
    CMapParts &part = ground->parts[slot];
    part.handle = part_id;
    part.parts_no = part_id;
    part.rot_y = rotation;
    part.pos[0] = x;
    part.pos[1] = y;
    part.pos[2] = z;
    part.pos[3] = 1.0f;
    return slot;
}

int FakeGeoramaRemove(CEditGround *ground, int *out_part_id, int *out_rotation,
                      float x, float y, float z) {
    ++g_fake_georama_remove_calls;
    if (g_fake_georama_reject_remove) return -1;
    for (int index = 0; index < 128; ++index) {
        CMapParts &part = ground->parts[index];
        if (part.handle < 0 || std::abs(part.pos[0] - x) > 0.001f ||
            std::abs(part.pos[1] - y) > 0.001f || std::abs(part.pos[2] - z) > 0.001f) {
            continue;
        }
        *out_part_id = part.parts_no;
        *out_rotation = part.rot_y;
        if (EDITPARTS_INFO *plot = ground->parts_info->GetPartsInfo(part.parts_no); plot != nullptr) {
            --plot->placed;
        }
        part.handle = -1;
        part.parts_no = -1;
        return index;
    }
    return -1;
}

} // namespace

TEST(MoonModGeoramaApi, ReadsNativeTownPlotsRequestsAndPlacedParts) {
    CSaveData *previous_save = SaveData;
    struct Restore {
        CSaveData *save;
        ~Restore() { SaveData = save; }
    } restore{previous_save};

    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);
    const moonmod::ApiOperationSpec *operation = moonmod::FindApiOperation("georama.get");
    const moonmod::ApiOperationSpec *request_set = moonmod::FindApiOperation("georama.request_set");
    ASSERT_NE(operation, nullptr);
    ASSERT_NE(request_set, nullptr);
    EXPECT_TRUE(std::ranges::any_of(config.api_capabilities, [](const moonmod::ApiCapability &capability) {
        return capability.path == "georama.get" && capability.supported;
    }));
    EXPECT_TRUE(std::ranges::any_of(config.api_capabilities, [](const moonmod::ApiCapability &capability) {
        return capability.path == "georama.request_set" && capability.supported;
    }));

    SaveData = nullptr;
    EXPECT_EQ(config.api_handler("test.moonmod.georama", *operation, moonmod::Json::array()).status,
              moonmod::ApiCallStatus::Failed);
    EXPECT_EQ(config.api_handler("test.moonmod.georama", *request_set,
                                 moonmod::Json::array({moonmod::Json{{"town_id", 2}, {"request_id", 1},
                                                                     {"completed", true}}})).status,
              moonmod::ApiCallStatus::Failed);

    auto save = std::make_unique<CSaveData>();
    save->Initialize();
    SaveData = save.get();
    CDngStatusData *status = SaveData->GetDngStatus();
    ASSERT_NE(status, nullptr);
    status->cur_georama = 2;

    SV_GEORAMA_DATA *town = SaveData->GetGrdData(2);
    ASSERT_NE(town, nullptr);
    town->request_count = 3;
    town->request_complete[0] = 1;
    town->request_complete[1] = 0;
    town->request_complete[2] = 1;
    town->parts_info[4].completion_flags = 0x13;
    town->parts_info[4].obtained = 1;
    town->parts_info[4].placed = 2;
    town->parts_info[4].stock = 5;
    town->parts_info[4].element_enabled[1] = 1;

    town->placed_parts[0] = {4, 2, 1.25f, 3.5f, -6.75f};
    town->placed_parts[1] = {9, 1, -8.0f, 0.0f, 12.0f};
    town->placed_parts[2].part_id = -1;

    const moonmod::ApiCallResult current_town =
        config.api_handler("test.moonmod.georama", *operation, moonmod::Json::array());
    ASSERT_EQ(current_town.status, moonmod::ApiCallStatus::Ok) << current_town.message;
    EXPECT_EQ(current_town.value.at("town_id"), 2);
    EXPECT_EQ(current_town.value.at("id"), "chronicle:town:2");
    EXPECT_EQ(current_town.value.at("request_count"), 3);
    EXPECT_EQ(current_town.value.at("completed_requests"), 2);
    ASSERT_EQ(current_town.value.at("plots").size(), 24u);
    EXPECT_EQ(current_town.value.at("plots").at(4).at("completion_flags"), 0x13);
    EXPECT_TRUE(current_town.value.at("plots").at(4).at("obtained"));
    EXPECT_EQ(current_town.value.at("plots").at(4).at("placed_count"), 2);
    EXPECT_EQ(current_town.value.at("plots").at(4).at("stock"), 5);
    EXPECT_TRUE(current_town.value.at("plots").at(4).at("elements").at(1));
    ASSERT_EQ(current_town.value.at("placed_parts").size(), 2u);
    EXPECT_EQ(current_town.value.at("placed_parts").at(0).at("id"), "chronicle:georama-part:4");
    EXPECT_EQ(current_town.value.at("placed_parts").at(0).at("rotation_quarter_turns"), 2);
    EXPECT_FLOAT_EQ(current_town.value.at("placed_parts").at(0).at("position").at("x"), 1.25f);
    EXPECT_FLOAT_EQ(current_town.value.at("placed_parts").at(0).at("position").at("y"), 3.5f);
    EXPECT_FLOAT_EQ(current_town.value.at("placed_parts").at(0).at("position").at("z"), -6.75f);

    const moonmod::ApiCallResult request_completed = config.api_handler(
        "test.moonmod.georama", *request_set,
        moonmod::Json::array({moonmod::Json{{"town_id", 2}, {"request_id", 1}, {"completed", true}}}));
    ASSERT_EQ(request_completed.status, moonmod::ApiCallStatus::Ok) << request_completed.message;
    EXPECT_FALSE(request_completed.value.at("previous").get<bool>());
    EXPECT_TRUE(request_completed.value.at("completed").get<bool>());
    EXPECT_TRUE(request_completed.value.at("changed").get<bool>());
    EXPECT_EQ(town->request_complete[1], 1);
    const moonmod::ApiCallResult updated_town =
        config.api_handler("test.moonmod.georama", *operation, moonmod::Json::array({moonmod::Json{{"town_id", 2}}}));
    ASSERT_EQ(updated_town.status, moonmod::ApiCallStatus::Ok);
    EXPECT_EQ(updated_town.value.at("completed_requests"), 3);
    const moonmod::ApiCallResult request_cleared = config.api_handler(
        "test.moonmod.georama", *request_set,
        moonmod::Json::array({moonmod::Json{{"town_id", 2}, {"request_id", 1}, {"completed", false}}}));
    ASSERT_EQ(request_cleared.status, moonmod::ApiCallStatus::Ok);
    EXPECT_TRUE(request_cleared.value.at("previous").get<bool>());
    EXPECT_FALSE(request_cleared.value.at("completed").get<bool>());
    EXPECT_TRUE(request_cleared.value.at("changed").get<bool>());
    EXPECT_EQ(town->request_complete[1], 0);

    status->cur_georama = 6;
    const moonmod::ApiCallResult no_current_town =
        config.api_handler("test.moonmod.georama", *operation, moonmod::Json::array());
    EXPECT_EQ(no_current_town.status, moonmod::ApiCallStatus::Failed);

    const moonmod::ApiCallResult explicit_town = config.api_handler(
        "test.moonmod.georama", *operation, moonmod::Json::array({moonmod::Json{{"town_id", 2}}}));
    ASSERT_EQ(explicit_town.status, moonmod::ApiCallStatus::Ok) << explicit_town.message;
    EXPECT_EQ(explicit_town.value.at("town_id"), 2);

    for (const moonmod::Json &invalid : {
             moonmod::Json::array({1}),
             moonmod::Json::array({moonmod::Json{{"town_id", 6}}}),
             moonmod::Json::array({moonmod::Json{{"town_id", 1}, {"unexpected", true}}}),
             moonmod::Json::array({moonmod::Json{{"unexpected", true}}}),
         }) {
        EXPECT_EQ(config.api_handler("test.moonmod.georama", *operation, invalid).status,
                  moonmod::ApiCallStatus::InvalidArgument);
    }
    for (const moonmod::Json &invalid : {
             moonmod::Json::array({moonmod::Json{{"town_id", 2}, {"request_id", 3}, {"completed", true}}}),
             moonmod::Json::array({moonmod::Json{{"town_id", 6}, {"request_id", 0}, {"completed", true}}}),
             moonmod::Json::array({moonmod::Json{{"town_id", 2}, {"request_id", 0}, {"completed", 1}}}),
         }) {
        EXPECT_EQ(config.api_handler("test.moonmod.georama", *request_set, invalid).status,
                  moonmod::ApiCallStatus::InvalidArgument);
    }

    town->placed_parts[0].pos_x = std::nanf("");
    const moonmod::ApiCallResult malformed_native = config.api_handler(
        "test.moonmod.georama", *operation, moonmod::Json::array({moonmod::Json{{"town_id", 2}}}));
    EXPECT_EQ(malformed_native.status, moonmod::ApiCallStatus::Failed);
}

TEST(MoonModGeoramaApi, SetsDefinedPlotElementsThroughActiveNativeEditorState) {
    CSaveData *previous_save = SaveData;
    const s32 previous_mode = mode;
    const int previous_edit_map = NowEditMap;
    const CEditPartsInfo previous_parts = EditPartsInfo;
    struct Restore {
        CSaveData *save;
        s32 mode;
        int edit_map;
        CEditPartsInfo parts;
        ~Restore() {
            SaveData = save;
            ::mode = mode;
            NowEditMap = edit_map;
            EditPartsInfo = parts;
        }
    } restore{previous_save, previous_mode, previous_edit_map, previous_parts};

    auto save = std::make_unique<CSaveData>();
    save->Initialize();
    SaveData = save.get();
    SaveData->GetDngStatus()->cur_georama = 2;
    mode = GAME_MODE_EDIT;
    NowEditMap = 2;

    EDITPARTS_INFO *plot = EditPartsInfo.GetPartsInfo(4);
    ASSERT_NE(plot, nullptr);
    plot->parts_no = 4;
    for (EDITPARTS_ELEMENT &element : plot->elements) {
        element.id = -1;
        element.required_element = -1;
        element.enabled = 0;
    }
    plot->elements[0].id = 3;
    plot->elements[0].enabled = 1;
    plot->elements[1].id = 7;
    plot->elements[1].required_element = 0;
    SV_EDIT_PARTS_INFO *saved_plot = SaveData->GetEditPartsInfo(2, 4);
    ASSERT_NE(saved_plot, nullptr);
    saved_plot->element_enabled[0] = 1;

    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);
    const moonmod::ApiOperationSpec *set = moonmod::FindApiOperation("georama.element_set");
    ASSERT_NE(set, nullptr);
    EXPECT_TRUE(std::ranges::any_of(config.api_capabilities, [](const moonmod::ApiCapability &capability) {
        return capability.path == "georama.element_set" && capability.supported && capability.reason.empty();
    }));

    const moonmod::Json enable = {{"town_id", 2}, {"plot_id", 4}, {"element_id", 1}, {"enabled", true}};
    const moonmod::ApiCallResult enabled = config.api_handler(
        "test.moonmod.georama-element", *set, moonmod::Json::array({enable}));
    ASSERT_EQ(enabled.status, moonmod::ApiCallStatus::Ok) << enabled.message;
    EXPECT_FALSE(enabled.value.at("previous"));
    EXPECT_TRUE(enabled.value.at("enabled"));
    EXPECT_TRUE(enabled.value.at("changed"));
    EXPECT_EQ(saved_plot->element_enabled[1], 1);
    EXPECT_EQ(plot->elements[1].enabled, 1);

    const moonmod::ApiCallResult idempotent = config.api_handler(
        "test.moonmod.georama-element", *set, moonmod::Json::array({enable}));
    ASSERT_EQ(idempotent.status, moonmod::ApiCallStatus::Ok) << idempotent.message;
    EXPECT_FALSE(idempotent.value.at("changed"));

    const moonmod::Json disable_required = {{"town_id", 2}, {"plot_id", 4}, {"element_id", 0}, {"enabled", false}};
    const moonmod::ApiCallResult dependency = config.api_handler(
        "test.moonmod.georama-element", *set, moonmod::Json::array({disable_required}));
    EXPECT_EQ(dependency.status, moonmod::ApiCallStatus::Failed);
    EXPECT_EQ(saved_plot->element_enabled[0], 1);
    EXPECT_EQ(plot->elements[0].enabled, 1);

    moonmod::Json disable = enable;
    disable["enabled"] = false;
    const moonmod::ApiCallResult disabled = config.api_handler(
        "test.moonmod.georama-element", *set, moonmod::Json::array({disable}));
    ASSERT_EQ(disabled.status, moonmod::ApiCallStatus::Ok) << disabled.message;
    EXPECT_TRUE(disabled.value.at("previous"));
    EXPECT_FALSE(disabled.value.at("enabled"));
    EXPECT_TRUE(disabled.value.at("changed"));
    EXPECT_EQ(saved_plot->element_enabled[1], 0);
    EXPECT_EQ(plot->elements[1].enabled, 0);

    for (const moonmod::Json &invalid : {
             moonmod::Json::array({moonmod::Json{{"town_id", 2}, {"plot_id", 4}, {"element_id", 6}, {"enabled", true}}}),
             moonmod::Json::array({moonmod::Json{{"town_id", 2}, {"plot_id", 4}, {"element_id", 0}, {"enabled", 1}}}),
         }) {
        EXPECT_EQ(config.api_handler("test.moonmod.georama-element", *set, invalid).status,
                  moonmod::ApiCallStatus::InvalidArgument);
    }

    plot->elements[2].id = -1;
    const moonmod::Json absent = {{"town_id", 2}, {"plot_id", 4}, {"element_id", 2}, {"enabled", true}};
    EXPECT_EQ(config.api_handler("test.moonmod.georama-element", *set,
                                 moonmod::Json::array({absent})).status,
              moonmod::ApiCallStatus::InvalidArgument);

    NowEditMap = 3;
    EXPECT_EQ(config.api_handler("test.moonmod.georama-element", *set,
                                 moonmod::Json::array({enable})).status,
              moonmod::ApiCallStatus::Failed);
}

TEST(MoonModGeoramaApi, PlacesAndRemovesNativePlotPartsWithStaleSafeIds) {
    CSaveData *previous_save = SaveData;
    CEditGround *previous_ground = pEditGround;
    EDIT_MAP_INFO *previous_map_info = EditMapInfo;
    const s32 previous_mode = mode;
    const int previous_edit_map = NowEditMap;
    const CEditPartsInfo previous_parts = EditPartsInfo;
    struct Restore {
        CSaveData *save;
        CEditGround *ground;
        EDIT_MAP_INFO *map_info;
        s32 mode;
        int edit_map;
        CEditPartsInfo parts;
        ~Restore() {
            moonmod::SetChronicleGeoramaRoutinesForTests(nullptr, nullptr);
            SaveData = save;
            pEditGround = ground;
            EditMapInfo = map_info;
            ::mode = mode;
            NowEditMap = edit_map;
            EditPartsInfo = parts;
        }
    } restore{previous_save, previous_ground, previous_map_info, previous_mode, previous_edit_map, previous_parts};

    auto save = std::make_unique<CSaveData>();
    save->Initialize();
    SaveData = save.get();
    SaveData->GetDngStatus()->cur_georama = 2;
    SV_GEORAMA_DATA *town = SaveData->GetGrdData(2);
    ASSERT_NE(town, nullptr);
    town->request_count = 0;
    town->placed_parts[0].part_id = -1;

    EditPartsInfo = {};
    EditPartsInfo.parts_max = 24;
    for (int index = 0; index < 24; ++index) {
        EditPartsInfo.parts[index].parts_no = index;
        EditPartsInfo.parts[index].width = 1;
        EditPartsInfo.parts[index].height = 1;
        EditPartsInfo.parts[index].stock = 0;
        EditPartsInfo.parts[index].placed = 0;
    }
    EDITPARTS_INFO *plot = EditPartsInfo.GetPartsInfo(4);
    ASSERT_NE(plot, nullptr);
    plot->stock = 1;
    SV_EDIT_PARTS_INFO *saved_plot = SaveData->GetEditPartsInfo(2, 4);
    ASSERT_NE(saved_plot, nullptr);

    CEditGround ground{};
    ground.map_no = 2;
    ground.parts_info = &EditPartsInfo;
    ground.effect_count = 0;
    for (CMapParts &part : ground.parts) part.handle = -1;
    CEditArea area{};
    area.width = 16;
    area.height = 8;
    area.offset_x = -200.0f;
    area.offset_y = 10.0f;
    area.offset_z = 50.0f;
    area.unit_size = 25.0f;
    area.unit_alt = 12.5f;
    ground.areas[0] = &area;
    CMapParts native_templates[24]{};
    for (int index = 0; index < 24; ++index) {
        native_templates[index].handle = index;
        native_templates[index].info = &EditPartsInfo.parts[index];
    }
    ground.plot_parts = native_templates;
    pEditGround = &ground;
    EditMapInfo = nullptr;
    mode = GAME_MODE_EDIT;
    NowEditMap = 2;

    g_fake_georama_place_calls = 0;
    g_fake_georama_remove_calls = 0;
    g_fake_georama_rotation = -9;
    g_fake_georama_reject_place = false;
    g_fake_georama_reject_remove = false;
    moonmod::SetChronicleGeoramaRoutinesForTests(FakeGeoramaPlace, FakeGeoramaRemove);

    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);
    const moonmod::ApiOperationSpec *get = moonmod::FindApiOperation("georama.get");
    const moonmod::ApiOperationSpec *place = moonmod::FindApiOperation("georama.place");
    const moonmod::ApiOperationSpec *remove = moonmod::FindApiOperation("georama.remove");
    ASSERT_NE(get, nullptr);
    ASSERT_NE(place, nullptr);
    ASSERT_NE(remove, nullptr);
    for (const char *path : {"georama.get", "georama.place", "georama.remove"}) {
        EXPECT_TRUE(std::ranges::any_of(config.api_capabilities, [path](const moonmod::ApiCapability &capability) {
            return capability.path == path && capability.supported && capability.reason.empty();
        })) << path;
    }

    const moonmod::Json placement = {{"town_id", 2}, {"part_id", 4},
                                     {"position", {{"x", 12.5}, {"y", 0.0}, {"z", -8.25}}},
                                     {"rotation_quarter_turns", 3}};
    plot->stock = 0;
    const moonmod::ApiCallResult no_stock = config.api_handler(
        "test.moonmod.georama-placement", *place,
        moonmod::Json::array({moonmod::Json{{"town_id", 2}, {"part_id", 4},
                                            {"position", {{"x", 1}, {"y", 0}, {"z", 1}}},
                                            {"rotation_quarter_turns", 0}}}));
    EXPECT_EQ(no_stock.status, moonmod::ApiCallStatus::Failed);
    EXPECT_EQ(g_fake_georama_place_calls, 0);

    plot->stock = 1;
    const moonmod::ApiCallResult placed = config.api_handler(
        "test.moonmod.georama-placement", *place, moonmod::Json::array({placement}));
    ASSERT_EQ(placed.status, moonmod::ApiCallStatus::Ok) << placed.message;
    EXPECT_EQ(g_fake_georama_place_calls, 1);
    EXPECT_EQ(g_fake_georama_rotation, -1);
    EXPECT_EQ(placed.value.value("part_id", -1), 4);
    EXPECT_EQ(placed.value.value("rotation_quarter_turns", -1), 3);
    EXPECT_EQ(placed.value.value("remaining_built", -1), 0);
    EXPECT_EQ(saved_plot->placed, 1);
    const std::string placement_id = placed.value.value("placement_id", "");
    ASSERT_FALSE(placement_id.empty());

    const moonmod::ApiCallResult listed = config.api_handler(
        "test.moonmod.georama-placement", *get, moonmod::Json::array({moonmod::Json{{"town_id", 2}}}));
    ASSERT_EQ(listed.status, moonmod::ApiCallStatus::Ok) << listed.message;
    ASSERT_EQ(listed.value.at("placed_parts").size(), 1u);
    EXPECT_EQ(listed.value.at("placed_parts")[0].value("placement_id", ""), placement_id);
    EXPECT_EQ(listed.value.at("placed_parts")[0].value("rotation_quarter_turns", -1), 3);
    ASSERT_EQ(listed.value.at("placement_areas").size(), 1u);
    EXPECT_EQ(listed.value.at("placement_areas")[0].value("width_cells", -1), 16);
    EXPECT_FLOAT_EQ(listed.value.at("placement_areas")[0].at("origin").at("x"), -200.0f);
    ASSERT_TRUE(std::ranges::any_of(listed.value.at("native_pieces"), [](const moonmod::Json &piece) {
        return piece.value("part_id", -1) == 4 && piece.value("width_cells", -1) == 1 &&
               piece.value("available_built", -1) == 0;
    }));

    const moonmod::ApiCallResult removed = config.api_handler(
        "test.moonmod.georama-placement", *remove, moonmod::Json::array({moonmod::Json{{"placement_id", placement_id}}}));
    ASSERT_EQ(removed.status, moonmod::ApiCallStatus::Ok) << removed.message;
    EXPECT_TRUE(removed.value.value("removed", false));
    EXPECT_EQ(removed.value.value("part_id", -1), 4);
    EXPECT_EQ(removed.value.value("remaining_built", -1), 1);
    EXPECT_EQ(g_fake_georama_remove_calls, 1);
    EXPECT_EQ(saved_plot->placed, 0);
    int saved_count = 0;
    const SV_GRD_PART *saved_parts = SaveData->GetParts(2, &saved_count);
    ASSERT_NE(saved_parts, nullptr);
    EXPECT_EQ(saved_count, 0);
    EXPECT_EQ(saved_parts[0].part_id, -1);

    const moonmod::ApiCallResult stale = config.api_handler(
        "test.moonmod.georama-placement", *remove, moonmod::Json::array({moonmod::Json{{"placement_id", placement_id}}}));
    EXPECT_EQ(stale.status, moonmod::ApiCallStatus::Failed);
    EXPECT_EQ(g_fake_georama_remove_calls, 1);

    for (const moonmod::Json &invalid : {
             moonmod::Json::array({moonmod::Json{{"town_id", 2}, {"part_id", 4},
                                                 {"position", {{"x", 1}, {"y", 2}, {"z", 3}}},
                                                 {"rotation_quarter_turns", 4}}}),
             moonmod::Json::array({moonmod::Json{{"town_id", 2}, {"part_id", 4},
                                                 {"position", {{"x", 100001}, {"y", 2}, {"z", 3}}},
                                                 {"rotation_quarter_turns", 0}}}),
         }) {
        EXPECT_EQ(config.api_handler("test.moonmod.georama-placement", *place, invalid).status,
                  moonmod::ApiCallStatus::InvalidArgument);
    }
}

TEST(MoonModGeoramaApi, PublishesChangedWorldWritesOnceForQuestObjectives) {
    moonmod::SharedRuntime().Stop();
    CSaveData *previous_save = SaveData;
    CEditGround *previous_ground = pEditGround;
    EDIT_MAP_INFO *previous_map_info = EditMapInfo;
    const s32 previous_mode = mode;
    const int previous_edit_map = NowEditMap;
    const CEditPartsInfo previous_parts = EditPartsInfo;
    struct Restore {
        CSaveData *save;
        CEditGround *ground;
        EDIT_MAP_INFO *map_info;
        s32 mode;
        int edit_map;
        CEditPartsInfo parts;
        ~Restore() {
            moonmod::SharedRuntime().Stop();
            moonmod::SetChronicleGeoramaRoutinesForTests(nullptr, nullptr);
            SaveData = save;
            pEditGround = ground;
            EditMapInfo = map_info;
            ::mode = mode;
            NowEditMap = edit_map;
            EditPartsInfo = parts;
        }
    } restore{previous_save, previous_ground, previous_map_info, previous_mode, previous_edit_map, previous_parts};

    auto save = std::make_unique<CSaveData>();
    save->Initialize();
    SaveData = save.get();
    SaveData->GetDngStatus()->cur_georama = 2;
    mode = GAME_MODE_EDIT;
    NowEditMap = 2;
    EditPartsInfo = {};
    EditPartsInfo.parts_max = 24;
    for (int index = 0; index < 24; ++index) {
        EditPartsInfo.parts[index].parts_no = index;
        EditPartsInfo.parts[index].stock = 0;
        EditPartsInfo.parts[index].placed = 0;
    }
    EDITPARTS_INFO *plot = EditPartsInfo.GetPartsInfo(4);
    ASSERT_NE(plot, nullptr);
    plot->parts_no = 4;
    plot->stock = 1;
    plot->elements[1].id = 7;
    plot->elements[1].required_element = -1;
    plot->elements[1].enabled = 0;
    SV_GEORAMA_DATA *town = SaveData->GetGrdData(2);
    ASSERT_NE(town, nullptr);
    town->request_count = 3;
    town->request_complete[1] = 0;

    CEditGround ground{};
    ground.map_no = 2;
    ground.parts_info = &EditPartsInfo;
    ground.effect_count = 0;
    for (CMapParts &part : ground.parts) part.handle = -1;
    CMapParts native_templates[24]{};
    for (int index = 0; index < 24; ++index) {
        native_templates[index].handle = index;
        native_templates[index].info = &EditPartsInfo.parts[index];
    }
    ground.plot_parts = native_templates;
    pEditGround = &ground;
    EditMapInfo = nullptr;
    moonmod::SetChronicleGeoramaRoutinesForTests(FakeGeoramaPlace, FakeGeoramaRemove);
    g_fake_georama_place_calls = 0;
    g_fake_georama_remove_calls = 0;
    g_fake_georama_reject_place = false;
    g_fake_georama_reject_remove = false;

    TempRoot root;
    const fs::path package = root.path / "mods" / "test.moonmod.georama-event";
    Write(package / "mod.json", R"json({
      "schema_version":1,"id":"test.moonmod.georama-event","name":"Georama Event Probe","version":"1.0.0",
      "framework_api":1,"games":["chronicle"],"entry":"main.lua",
      "permissions":["content.register","game.read","game.write"]
    })json");
    Write(package / "main.lua", R"lua(
assert(game.quests.register({
  id = "request", title = "Complete a town request",
  objectives = {{id = "request", title = "Complete request", target = 1,
    event = "game.georama.request_changed", match = {town_id = 2, request_id = 1, completed = true}}}
}))
assert(game.quests.register({
  id = "plot-element", title = "Enable a plot element",
  objectives = {{id = "element", title = "Enable plot element", target = 1,
    event = "game.georama.element_changed", match = {town_id = 2, plot_id = 4, element_id = 1, enabled = true}}}
}))
assert(game.quests.register({
  id = "story-flag", title = "Set a story flag",
  objectives = {{id = "flag", title = "Set flag", target = 1,
    event = "game.story_flag_changed", match = {id = 17, value = true}}}
}))
assert(game.quests.register({
  id = "story-value", title = "Set a story value",
  objectives = {{id = "value", title = "Set value", target = 1,
    event = "game.story_value_changed", match = {id = 17, value = 900}}}
}))
assert(game.quests.register({
  id = "map-flag", title = "Set a map flag",
  objectives = {{id = "flag", title = "Set map flag", target = 1,
    event = "game.map_flag_changed", match = {map_id = 2, flag_id = 7, value = true}}}
}))
assert(game.quests.register({
  id = "piece-place", title = "Place a native Georama piece",
  objectives = {{id = "place", title = "Place piece", target = 1,
    event = "game.georama.piece_placed", match = {town_id = 2, part_id = 4}}}
}))
assert(game.quests.register({
  id = "piece-remove", title = "Remove a native Georama piece",
  objectives = {{id = "remove", title = "Remove piece", target = 1,
    event = "game.georama.piece_removed", match = {town_id = 2, part_id = 4}}}
}))
local applied = false
moonmod.on("game.georama.request_changed", function(event)
  assert(event.town_id == 2 and event.request_id == 1 and event.completed == true)
  assert(event.changed == true and event.source_mod == "test.moonmod.georama-event")
  moonmod.log("info", "GEORAMA_EVENT:" .. tostring(event.request_id))
end)
moonmod.on("game.georama.element_changed", function(event)
  assert(event.town_id == 2 and event.plot_id == 4 and event.element_id == 1 and event.enabled == true)
  assert(event.changed == true and event.source_mod == "test.moonmod.georama-event")
  moonmod.log("info", "GEORAMA_ELEMENT_EVENT:" .. tostring(event.element_id))
end)
moonmod.on("game.story_flag_changed", function(event)
  assert(event.id == 17 and event.value == true and event.changed == true)
  assert(event.source_mod == "test.moonmod.georama-event")
  moonmod.log("info", "WORLD_EVENT:story-flag")
end)
moonmod.on("game.story_value_changed", function(event)
  assert(event.id == 17 and event.value == 900 and event.changed == true)
  assert(event.source_mod == "test.moonmod.georama-event")
  moonmod.log("info", "WORLD_EVENT:story-value")
end)
moonmod.on("game.map_flag_changed", function(event)
  assert(event.map_id == 2 and event.flag_id == 7 and event.value == true and event.changed == true)
  assert(event.source_mod == "test.moonmod.georama-event")
  moonmod.log("info", "WORLD_EVENT:map-flag")
end)
moonmod.on("game.georama.piece_placed", function(event)
  assert(event.town_id == 2 and event.part_id == 4 and type(event.placement_id) == "string")
  assert(event.changed == true and event.source_mod == "test.moonmod.georama-event")
  moonmod.log("info", "GEORAMA_PIECE_EVENT:placed")
end)
moonmod.on("game.georama.piece_removed", function(event)
  assert(event.town_id == 2 and event.part_id == 4 and event.removed == true)
  assert(event.changed == true and event.source_mod == "test.moonmod.georama-event")
  moonmod.log("info", "GEORAMA_PIECE_EVENT:removed")
end)
moonmod.on("game.tick", function()
  if applied then return end
  assert(game.quests.start({id = "request"}))
  assert(game.quests.start({id = "plot-element"}))
  assert(game.quests.start({id = "story-flag"}))
  assert(game.quests.start({id = "story-value"}))
  assert(game.quests.start({id = "map-flag"}))
  assert(game.quests.start({id = "piece-place"}))
  assert(game.quests.start({id = "piece-remove"}))
  assert(assert(game.georama.request_set({town_id = 2, request_id = 1, completed = true})).changed)
  assert(not assert(game.georama.request_set({town_id = 2, request_id = 1, completed = true})).changed)
  assert(assert(game.georama.element_set({town_id = 2, plot_id = 4, element_id = 1, enabled = true})).changed)
  assert(assert(game.world.story_flag_set({id = 17, value = true})).changed)
  assert(not assert(game.world.story_flag_set({id = 17, value = true})).changed)
  assert(assert(game.world.story_value_set({id = 17, value = 900})).changed)
  assert(not assert(game.world.story_value_set({id = 17, value = 900})).changed)
  assert(assert(game.world.map_flag_set({map_id = 2, flag_id = 7, value = true})).changed)
  assert(not assert(game.world.map_flag_set({map_id = 2, flag_id = 7, value = true})).changed)
  local placed = assert(game.georama.place({town_id = 2, part_id = 4,
    position = {x = 12.5, y = 0, z = -8.25}, rotation_quarter_turns = 3}))
  assert(placed.placement_id and placed.rotation_quarter_turns == 3)
  assert(assert(game.georama.remove({placement_id = placed.placement_id})).removed)
  applied = true
end)
)lua");
    Write(root.path / "save" / "moonmod" / "profile.json",
          R"json({"format_version":1,"enabled":{"test.moonmod.georama-event":true},"settings":{}})json");

    std::vector<std::string> logs;
    moonmod::RuntimeConfig config;
    config.game_id = "chronicle";
    config.game_name = "Chronicle";
    moonmod::ConfigureChronicleApi(config);
    config.mod_directories = {root.path / "mods"};
    config.save_root = root.path / "save";
    config.world_location_reader = [] {
        return moonmod::ApiCallResult{moonmod::ApiCallStatus::Failed, nullptr, "no active world in event probe"};
    };
    config.log = [&](std::string_view, std::string_view, std::string_view message) { logs.emplace_back(message); };
    std::string error;
    ASSERT_TRUE(moonmod::SharedRuntime().Start(std::move(config), error)) << error;
    ASSERT_EQ(moonmod::SharedRuntime().Mods().size(), 1u);
    ASSERT_EQ(moonmod::SharedRuntime().Mods()[0].state, moonmod::ModState::Loaded);

    moonmod::SharedRuntime().Pump();
    EXPECT_TRUE(std::ranges::none_of(logs, [](const std::string &message) {
        return message.find("GEORAMA_EVENT:") != std::string::npos ||
               message.find("GEORAMA_ELEMENT_EVENT:") != std::string::npos ||
               message.find("GEORAMA_PIECE_EVENT:") != std::string::npos ||
               message.find("WORLD_EVENT:") != std::string::npos;
    }));
    moonmod::SharedRuntime().Pump();
    const auto event_count = std::ranges::count_if(logs, [](const std::string &message) {
        return message.find("GEORAMA_EVENT:1") != std::string::npos;
    });
    EXPECT_EQ(event_count, 1);
    EXPECT_EQ(std::ranges::count_if(logs, [](const std::string &message) {
        return message.find("GEORAMA_ELEMENT_EVENT:1") != std::string::npos;
    }), 1);
    EXPECT_EQ(std::ranges::count_if(logs, [](const std::string &message) {
        return message.find("GEORAMA_PIECE_EVENT:placed") != std::string::npos;
    }), 1);
    EXPECT_EQ(std::ranges::count_if(logs, [](const std::string &message) {
        return message.find("GEORAMA_PIECE_EVENT:removed") != std::string::npos;
    }), 1);
    for (const std::string_view event_name : {"WORLD_EVENT:story-flag", "WORLD_EVENT:story-value", "WORLD_EVENT:map-flag"}) {
        EXPECT_EQ(std::ranges::count_if(logs, [event_name](const std::string &message) {
            return message.find(event_name) != std::string::npos;
        }), 1);
    }
    const moonmod::ApiCallResult quests = moonmod::SharedRuntime().Quests("test.moonmod.georama-event");
    ASSERT_EQ(quests.status, moonmod::ApiCallStatus::Ok) << quests.message;
    ASSERT_EQ(quests.value.at("quests").size(), 7u);
    EXPECT_TRUE(std::ranges::all_of(quests.value.at("quests"), [](const moonmod::Json &quest) {
        return quest.at("status") == "completed" && quest.at("objectives").at(0).at("progress") == 1;
    }));
}

TEST(MoonModGeoramaApi, RegistersStableDataOnlyPieceVariantsInExtendedNativeSlots) {
    CSaveData *previous_save = SaveData;
    CEditGround *previous_ground = pEditGround;
    CEditPartsInfo previous_parts = EditPartsInfo;
    const int previous_edit_map = NowEditMap;
    const s32 previous_mode = mode;
    struct Restore {
        CEditGround *ground;
        CSaveData *save;
        CEditPartsInfo parts;
        int edit_map;
        s32 game_mode;
        ~Restore() {
            moonmod::ResetChronicleGeoramaForTests();
            SaveData = save;
            pEditGround = ground;
            EditPartsInfo = parts;
            NowEditMap = edit_map;
            mode = game_mode;
        }
    } restore{previous_ground, previous_save, previous_parts, previous_edit_map, previous_mode};

    moonmod::ResetChronicleGeoramaForTests();
    EditPartsInfo = {};
    EditPartsInfo.Initialize(2);
    auto save = std::make_unique<CSaveData>();
    save->Initialize();
    save->GetDngStatus()->cur_georama = 2;
    SaveData = save.get();
    CMapParts templates[CHRONICLE_GEORAMA_PART_CAPACITY]{};
    for (CMapParts &part : templates) part.handle = -1;
    templates[4].handle = 4;
    templates[4].parts_no = 4;
    templates[4].subtype = MAP_PARTS_SUBTYPE_NONE;
    templates[4].info = &EditPartsInfo.parts[4];

    CEditGround ground{};
    ground.map_no = 2;
    ground.plot_parts = templates;
    ground.parts_info = &EditPartsInfo;
    for (CMapParts &part : ground.parts) part.handle = -1;
    pEditGround = &ground;
    NowEditMap = 2;
    mode = GAME_MODE_EDIT;

    TempRoot root;
    moonmod::RuntimeConfig config;
    config.save_root = root.path / "save";
    moonmod::ConfigureChronicleApi(config);
    const moonmod::ApiOperationSpec *register_piece = moonmod::FindApiOperation("georama.register");
    const moonmod::ApiOperationSpec *register_content = moonmod::FindApiOperation("content.register");
    const moonmod::ApiOperationSpec *unregister_content = moonmod::FindApiOperation("content.unregister");
    const moonmod::ApiOperationSpec *list_content = moonmod::FindApiOperation("content.list");
    const moonmod::ApiOperationSpec *place_piece = moonmod::FindApiOperation("georama.place");
    const moonmod::ApiOperationSpec *get_georama = moonmod::FindApiOperation("georama.get");
    const moonmod::ApiOperationSpec *remove_piece = moonmod::FindApiOperation("georama.remove");
    ASSERT_NE(register_piece, nullptr);
    ASSERT_NE(register_content, nullptr);
    ASSERT_NE(unregister_content, nullptr);
    ASSERT_NE(list_content, nullptr);
    ASSERT_NE(place_piece, nullptr);
    ASSERT_NE(get_georama, nullptr);
    ASSERT_NE(remove_piece, nullptr);
    EXPECT_TRUE(std::ranges::any_of(config.api_capabilities, [](const moonmod::ApiCapability &capability) {
        return capability.path == "georama.register" && capability.supported && capability.reason.empty();
    }));

    const moonmod::Json definition = {{"id", "garden_post"}, {"name", "Garden Post"},
                                      {"template_part_id", 4}, {"town_id", 2}, {"stock", 3}};
    const auto direct = config.api_handler("test.moonmod.georama", *register_piece,
                                           moonmod::Json::array({definition}));
    ASSERT_EQ(direct.status, moonmod::ApiCallStatus::Ok) << direct.message;
    ASSERT_EQ(direct.value.at("native_part_id"), CHRONICLE_GEORAMA_NATIVE_PART_COUNT);
    EXPECT_EQ(direct.value.at("id"), "test.moonmod.georama:garden_post");
    EXPECT_TRUE(direct.value.at("materialized").get<bool>());
    ASSERT_NE(EditPartsInfo.GetPartsInfo(CHRONICLE_GEORAMA_NATIVE_PART_COUNT), nullptr);
    EXPECT_EQ(EditPartsInfo.GetPartsInfo(CHRONICLE_GEORAMA_NATIVE_PART_COUNT)->stock, 3);
    EXPECT_EQ(templates[CHRONICLE_GEORAMA_NATIVE_PART_COUNT].handle, CHRONICLE_GEORAMA_NATIVE_PART_COUNT);
    ASSERT_NE(GetEditAtraPartsData(2, CHRONICLE_GEORAMA_NATIVE_PART_COUNT), nullptr);
    EXPECT_EQ(GetEditAtraPartsData(2, CHRONICLE_GEORAMA_NATIVE_PART_COUNT)->max, 3);
    EXPECT_EQ(EditPartsInfo.GetNextParts(-1), CHRONICLE_GEORAMA_NATIVE_PART_COUNT);

    CEditPartsInfo offscreen_town_catalog{};
    offscreen_town_catalog.Initialize(3);
    EXPECT_EQ(offscreen_town_catalog.GetPartsInfo(CHRONICLE_GEORAMA_NATIVE_PART_COUNT), nullptr);
    EXPECT_EQ(offscreen_town_catalog.GetNextParts(-1), -1);
    EXPECT_NE(EditPartsInfo.GetPartsInfo(CHRONICLE_GEORAMA_NATIVE_PART_COUNT), nullptr);
    EXPECT_NE(GetEditAtraPartsData(2, CHRONICLE_GEORAMA_NATIVE_PART_COUNT), nullptr);

    const auto typed = config.api_handler("test.moonmod.georama", *register_content,
                                          moonmod::Json::array({moonmod::Json{{"type", "georama_piece"},
                                                                            {"definition", definition}}}));
    EXPECT_EQ(typed.status, moonmod::ApiCallStatus::Ok) << typed.message;
    const auto listed = config.api_handler("test.moonmod.georama", *list_content, moonmod::Json::array());
    ASSERT_EQ(listed.status, moonmod::ApiCallStatus::Ok) << listed.message;
    // only this test's piece among the georama content (a bundled mod loaded by another test may leave its weapons)
    moonmod::Json pieces = moonmod::Json::array();
    for (const auto &entry : listed.value.at("entries")) {
        if (entry.at("content_type") == "georama_piece") pieces.push_back(entry);
    }
    ASSERT_EQ(pieces.size(), 1u);

    moonmod::SetChronicleGeoramaRoutinesForTests(FakeGeoramaPlace, FakeGeoramaRemove);
    const auto placed = config.api_handler("test.moonmod.georama", *place_piece,
                                           moonmod::Json::array({moonmod::Json{{"town_id", 2},
                                                                              {"part_id", "test.moonmod.georama:garden_post"},
                                                                              {"position", {{"x", 12.0}, {"y", 0.0}, {"z", -6.0}}},
                                                                              {"rotation_quarter_turns", 1}}}));
    ASSERT_EQ(placed.status, moonmod::ApiCallStatus::Ok) << placed.message;
    EXPECT_EQ(placed.value.at("id"), "test.moonmod.georama:garden_post");
    const auto blocked_unregister = config.api_handler(
        "test.moonmod.georama", *unregister_content,
        moonmod::Json::array({moonmod::Json{{"type", "georama_piece"}, {"id", "garden_post"}}}));
    EXPECT_EQ(blocked_unregister.status, moonmod::ApiCallStatus::Failed);

    const auto readback = config.api_handler("test.moonmod.georama", *get_georama, moonmod::Json::array());
    ASSERT_EQ(readback.status, moonmod::ApiCallStatus::Ok) << readback.message;
    ASSERT_EQ(readback.value.at("placed_parts").size(), 1u);
    EXPECT_EQ(readback.value.at("placed_parts").at(0).at("id"), "test.moonmod.georama:garden_post");
    const std::string placement_id = placed.value.at("placement_id").get<std::string>();
    const auto removed = config.api_handler("test.moonmod.georama", *remove_piece,
                                            moonmod::Json::array({moonmod::Json{{"placement_id", placement_id}}}));
    ASSERT_EQ(removed.status, moonmod::ApiCallStatus::Ok) << removed.message;
    EXPECT_EQ(removed.value.at("id"), "test.moonmod.georama:garden_post");
    EXPECT_EQ(config.api_handler("test.moonmod.georama", *get_georama, moonmod::Json::array()).value
                  .at("placed_parts").size(), 0u);
    const auto unregistered = config.api_handler(
        "test.moonmod.georama", *unregister_content,
        moonmod::Json::array({moonmod::Json{{"type", "georama_piece"}, {"id", "garden_post"}}}));
    ASSERT_EQ(unregistered.status, moonmod::ApiCallStatus::Ok) << unregistered.message;
    EXPECT_TRUE(unregistered.value.at("removed").get<bool>());

    const int stable_part_id = direct.value.at("native_part_id").get<int>();
    moonmod::ResetChronicleGeoramaForTests();
    const auto reloaded = config.api_handler("test.moonmod.georama", *register_piece,
                                             moonmod::Json::array({definition}));
    ASSERT_EQ(reloaded.status, moonmod::ApiCallStatus::Ok) << reloaded.message;
    EXPECT_EQ(reloaded.value.at("native_part_id"), stable_part_id);
    EXPECT_EQ(reloaded.value.at("materialized"), true);

    const auto removed_after_reload = config.api_handler(
        "test.moonmod.georama", *unregister_content,
        moonmod::Json::array({moonmod::Json{{"type", "georama_piece"}, {"id", "garden_post"}}}));
    ASSERT_EQ(removed_after_reload.status, moonmod::ApiCallStatus::Ok) << removed_after_reload.message;
    const moonmod::Json sparse_definition = { {"id", "sparse_slot_piece"}, {"name", "Sparse Slot Piece"},
                                              {"template_part_id", 4}, {"town_id", 2}, {"stock", 1} };
    const auto sparse = config.api_handler("test.moonmod.georama", *register_piece,
                                           moonmod::Json::array({sparse_definition}));
    ASSERT_EQ(sparse.status, moonmod::ApiCallStatus::Ok) << sparse.message;
    EXPECT_EQ(sparse.value.at("native_part_id"), CHRONICLE_GEORAMA_NATIVE_PART_COUNT + 1);
    EXPECT_EQ(EditPartsInfo.GetNextParts(-1), CHRONICLE_GEORAMA_NATIVE_PART_COUNT + 1);
}

TEST(MoonModGeoramaApi, RegisteredPieceVariantsCanHaveTheirOwnFootprint) {
    CSaveData *previous_save = SaveData;
    CEditGround *previous_ground = pEditGround;
    CEditPartsInfo previous_parts = EditPartsInfo;
    const int previous_edit_map = NowEditMap;
    const s32 previous_mode = mode;
    struct Restore {
        CEditGround *ground;
        CSaveData *save;
        CEditPartsInfo parts;
        int edit_map;
        s32 game_mode;
        ~Restore() {
            moonmod::ResetChronicleGeoramaForTests();
            SaveData = save;
            pEditGround = ground;
            EditPartsInfo = parts;
            NowEditMap = edit_map;
            mode = game_mode;
        }
    } restore{previous_ground, previous_save, previous_parts, previous_edit_map, previous_mode};

    moonmod::ResetChronicleGeoramaForTests();
    EditPartsInfo = {};
    EditPartsInfo.Initialize(2);
    auto save = std::make_unique<CSaveData>();
    save->Initialize();
    save->GetDngStatus()->cur_georama = 2;
    SaveData = save.get();
    // A 2x2 template whose filled cells are 5.
    u8 template_cells[4] = {0, 5, 5, 5};
    EPARTS_INFO_HEADER template_shape{};
    template_shape.width = 2;
    template_shape.height = 2;
    template_shape.cell = template_cells;
    EditPartsInfo.parts[4].width = 2;
    EditPartsInfo.parts[4].height = 2;
    EditPartsInfo.parts[4].header = &template_shape;
    CMapParts templates[CHRONICLE_GEORAMA_PART_CAPACITY]{};
    for (CMapParts &part : templates) part.handle = -1;
    templates[4].handle = 4;
    templates[4].parts_no = 4;
    templates[4].subtype = MAP_PARTS_SUBTYPE_NONE;
    templates[4].info = &EditPartsInfo.parts[4];
    CEditGround ground{};
    ground.map_no = 2;
    ground.plot_parts = templates;
    ground.parts_info = &EditPartsInfo;
    for (CMapParts &part : ground.parts) part.handle = -1;
    pEditGround = &ground;
    NowEditMap = 2;
    mode = GAME_MODE_EDIT;

    TempRoot root;
    moonmod::RuntimeConfig config;
    config.save_root = root.path / "save";
    moonmod::ConfigureChronicleApi(config);
    const moonmod::ApiOperationSpec *register_piece = moonmod::FindApiOperation("georama.register");
    ASSERT_NE(register_piece, nullptr);
    auto define = [](const char *id, moonmod::Json footprint) {
        return moonmod::Json::array({moonmod::Json{{"id", id}, {"name", "Long Fence"}, {"template_part_id", 4},
                                                   {"town_id", 2}, {"footprint", std::move(footprint)}}});
    };

    // A footprint without cells fills every cell with the template's value.
    const auto filled = config.api_handler("test.moonmod.georama", *register_piece,
                                           define("long_fence", {{"width", 3}, {"height", 1}}));
    ASSERT_EQ(filled.status, moonmod::ApiCallStatus::Ok) << filled.message;
    EXPECT_EQ(filled.value.at("footprint").at("width"), 3);
    const EDITPARTS_INFO *fence = EditPartsInfo.GetPartsInfo(filled.value.at("native_part_id").get<int>());
    ASSERT_NE(fence, nullptr);
    ASSERT_NE(fence->header, nullptr);
    EXPECT_NE(fence->header, &template_shape);
    EXPECT_EQ(fence->width, 3);
    EXPECT_EQ(fence->height, 1);
    EXPECT_EQ(fence->header->width, 3);
    for (int cell = 0; cell < 3; ++cell) EXPECT_EQ(fence->header->cell[cell], 5);
    // The template keeps its own shape.
    EXPECT_EQ(EditPartsInfo.GetPartsInfo(4)->header, &template_shape);
    EXPECT_EQ(EditPartsInfo.GetPartsInfo(4)->width, 2);

    const auto shaped = config.api_handler("test.moonmod.georama", *register_piece,
                                           define("corner", {{"width", 2}, {"height", 3},
                                                             {"cells", {5, 0, 5, 0, 5, 5}}}));
    ASSERT_EQ(shaped.status, moonmod::ApiCallStatus::Ok) << shaped.message;
    const EDITPARTS_INFO *corner = EditPartsInfo.GetPartsInfo(shaped.value.at("native_part_id").get<int>());
    ASSERT_NE(corner, nullptr);
    EXPECT_EQ(corner->height, 3);
    EXPECT_EQ(corner->header->cell[1], 0);
    EXPECT_EQ(corner->header->cell[5], 5);
    // Placement reads the cells through CMapParts::GetInfoData.
    CMapParts &placed = templates[shaped.value.at("native_part_id").get<int>()];
    placed.rot_y = 0;
    EXPECT_EQ(placed.GetInfoData(1, 0), 0);
    EXPECT_EQ(placed.GetInfoData(1, 2), 5);

    EXPECT_EQ(config.api_handler("test.moonmod.georama", *register_piece,
                                 define("bad_count", {{"width", 2}, {"height", 2}, {"cells", {5, 5}}})).status,
              moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(config.api_handler("test.moonmod.georama", *register_piece,
                                 define("empty", {{"width", 1}, {"height", 1}, {"cells", {0}}})).status,
              moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(config.api_handler("test.moonmod.georama", *register_piece,
                                 define("huge", {{"width", 33}, {"height", 1}})).status,
              moonmod::ApiCallStatus::InvalidArgument);
}
