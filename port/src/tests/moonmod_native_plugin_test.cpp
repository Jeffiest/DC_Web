#include <gtest/gtest.h>

#include <array>
#include <string_view>

#include "moonmod/native_plugin_contract.hpp"



extern "C" const MoonModNativePluginV1 *MoonModTestNativePluginV1(void);

TEST(MoonModNativePluginAbi, ValidatesCCompiledV1DescriptorAndDeclaredPermissions) {
    constexpr std::array<std::string_view, 3> permissions = {"game.read", "content.register", "native.extension"};
    const MoonModNativePluginV1 *plugin = MoonModTestNativePluginV1();
    EXPECT_EQ(moonmod::ValidateNativePluginV1(plugin, "example.native-probe", 1, 0, permissions),
              moonmod::NativePluginValidation::Valid);
    EXPECT_EQ(moonmod::ValidateNativePluginV1(nullptr, "example.native-probe", 1, 0, permissions),
              moonmod::NativePluginValidation::NullDescriptor);
    EXPECT_EQ(moonmod::NativePluginValidationMessage(moonmod::NativePluginValidation::Valid),
              "native plugin descriptor is compatible");
}

TEST(MoonModNativePluginAbi, RejectsMismatchedIdsHostVersionsAndUndeclaredPermissions) {
    constexpr std::array<std::string_view, 2> permissions = {"game.read", "native.extension"};
    const MoonModNativePluginV1 *plugin = MoonModTestNativePluginV1();
    EXPECT_EQ(moonmod::ValidateNativePluginV1(plugin, "other.plugin", 1, 0, permissions),
              moonmod::NativePluginValidation::PluginIdMismatch);
    EXPECT_EQ(moonmod::ValidateNativePluginV1(plugin, "example.native-probe", 1, 0, permissions),
              moonmod::NativePluginValidation::InvalidPermissionList);
    EXPECT_EQ(moonmod::ValidateNativePluginV1(plugin, "example.native-probe", 2, 0, permissions),
              moonmod::NativePluginValidation::UnsupportedHostAbi);
}

TEST(MoonModNativePluginAbi, RejectsShortTablesUnsupportedMajorsAndMissingCallbacks) {
    constexpr std::array<std::string_view, 3> permissions = {"game.read", "content.register", "native.extension"};
    MoonModNativePluginV1 plugin = *MoonModTestNativePluginV1();
    plugin.struct_size = sizeof(std::uint32_t) * 3;
    EXPECT_EQ(moonmod::ValidateNativePluginV1(&plugin, "example.native-probe", 1, 0, permissions),
              moonmod::NativePluginValidation::IncompleteDescriptor);
    plugin = *MoonModTestNativePluginV1();
    plugin.abi_major = 2;
    EXPECT_EQ(moonmod::ValidateNativePluginV1(&plugin, "example.native-probe", 1, 0, permissions),
              moonmod::NativePluginValidation::UnsupportedPluginAbi);
    plugin = *MoonModTestNativePluginV1();
    plugin.unload = nullptr;
    EXPECT_EQ(moonmod::ValidateNativePluginV1(&plugin, "example.native-probe", 1, 0, permissions),
              moonmod::NativePluginValidation::MissingUnloadCallback);
}
