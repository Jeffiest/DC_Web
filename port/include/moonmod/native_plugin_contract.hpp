#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "moonmod/native_plugin.h"

namespace moonmod {

enum class NativePluginValidation {
    Valid,
    NullDescriptor,
    IncompleteDescriptor,
    UnsupportedPluginAbi,
    UnsupportedHostAbi,
    PluginIdMismatch,
    InvalidPluginId,
    InvalidPermissionList,
    UnknownPermission,
    MissingLoadCallback,
    MissingUnloadCallback,
};

NativePluginValidation ValidateNativePluginV1(const MoonModNativePluginV1 *plugin,
                                               std::string_view expected_plugin_id,
                                               std::uint32_t host_abi_major,
                                               std::uint32_t host_abi_minor,
                                               std::span<const std::string_view> manifest_permissions);
std::string_view NativePluginValidationMessage(NativePluginValidation validation);

} // namespace moonmod
