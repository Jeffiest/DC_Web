#include <gtest/gtest.h>

#include <memory>

#include "moonmod/chronicle_adapter.hpp"
#include "moonmod/runtime.hpp"
#include "savedata.hpp"
#include "userstatus.hpp"

namespace dc::game {}
using namespace dc::game;

TEST(MoonModEconomyApi, ReadsCreditsClampsAddsAndRejectsInvalidOrUnaffordableSpending) {
    CSaveData *previous_save = SaveData;
    auto save = std::make_unique<CSaveData>();
    save->Initialize();
    SaveData = save.get();
    CDngStatusData *status = save->GetDngStatus();
    ASSERT_NE(status, nullptr);
    status->money = 100;

    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);
    const moonmod::ApiOperationSpec *get = moonmod::FindApiOperation("economy.get");
    const moonmod::ApiOperationSpec *add = moonmod::FindApiOperation("economy.add");
    const moonmod::ApiOperationSpec *spend = moonmod::FindApiOperation("economy.spend");
    ASSERT_NE(get, nullptr);
    ASSERT_NE(add, nullptr);
    ASSERT_NE(spend, nullptr);

    const moonmod::ApiCallResult initial = config.api_handler("test.moonmod.economy", *get, moonmod::Json::array());
    ASSERT_EQ(initial.status, moonmod::ApiCallStatus::Ok) << initial.message;
    EXPECT_EQ(initial.value.value("currency", ""), "gilda");
    EXPECT_EQ(initial.value.value("amount", -1), 100);
    EXPECT_EQ(initial.value.value("max_amount", -1), 65535);

    const moonmod::ApiCallResult credited = config.api_handler(
        "test.moonmod.economy", *add, moonmod::Json::array({{{"amount", 25}}}));
    ASSERT_EQ(credited.status, moonmod::ApiCallStatus::Ok) << credited.message;
    EXPECT_EQ(credited.value.value("added", -1), 25);
    EXPECT_EQ(status->money, 125);

    const moonmod::ApiCallResult spent = config.api_handler(
        "test.moonmod.economy", *spend, moonmod::Json::array({{{"amount", 40}}}));
    ASSERT_EQ(spent.status, moonmod::ApiCallStatus::Ok) << spent.message;
    EXPECT_EQ(spent.value.value("spent", -1), 40);
    EXPECT_EQ(status->money, 85);

    const moonmod::ApiCallResult insufficient = config.api_handler(
        "test.moonmod.economy", *spend, moonmod::Json::array({{{"amount", 86}}}));
    EXPECT_EQ(insufficient.status, moonmod::ApiCallStatus::Failed);
    EXPECT_EQ(status->money, 85);

    status->money = 65530;
    const moonmod::ApiCallResult clamped = config.api_handler(
        "test.moonmod.economy", *add, moonmod::Json::array({{{"amount", 20}}}));
    ASSERT_EQ(clamped.status, moonmod::ApiCallStatus::Ok) << clamped.message;
    EXPECT_EQ(clamped.value.value("amount", -1), 65535);
    EXPECT_EQ(clamped.value.value("added", -1), 5);

    const moonmod::ApiCallResult invalid = config.api_handler(
        "test.moonmod.economy", *add, moonmod::Json::array({{{"amount", 0}}}));
    EXPECT_EQ(invalid.status, moonmod::ApiCallStatus::InvalidArgument);
    const moonmod::ApiCallResult oversized = config.api_handler(
        "test.moonmod.economy", *spend, moonmod::Json::array({{{"amount", 32768}}}));
    EXPECT_EQ(oversized.status, moonmod::ApiCallStatus::InvalidArgument);
    const moonmod::ApiCallResult huge_unsigned = config.api_handler(
        "test.moonmod.economy", *spend,
        moonmod::Json::parse("[{\"amount\":18446744073709551615}]"));
    EXPECT_EQ(huge_unsigned.status, moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(status->money, 65535);

    SaveData = previous_save;
}
