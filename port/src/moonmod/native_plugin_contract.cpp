#include "moonmod/native_plugin_contract.hpp"

#include <algorithm>
#include <cstddef>
#include <ranges>
#include <string_view>

#include "moonmod/api.hpp"



namespace moonmod {
namespace {

constexpr std::size_t kMaximumNativePermissions = 32;

bool ValidPluginId(std::string_view value) {
    return !value.empty() && value.size() <= 64 &&
           std::ranges::all_of(value, [](unsigned char character) {
               return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                      (character >= '0' && character <= '9') || character == '.' || character == '_' ||
                      character == '-';
           });
}

constexpr std::size_t kRequiredPluginV1Size =
    offsetof(MoonModNativePluginV1, unload) + sizeof(MoonModNativeUnloadV1);

} // namespace

NativePluginValidation ValidateNativePluginV1(const MoonModNativePluginV1 *plugin,
                                               std::string_view expected_plugin_id,
                                               std::uint32_t host_abi_major,
                                               std::uint32_t host_abi_minor,
                                               std::span<const std::string_view> manifest_permissions) {
    if (plugin == nullptr) return NativePluginValidation::NullDescriptor;
    if (plugin->struct_size < kRequiredPluginV1Size) return NativePluginValidation::IncompleteDescriptor;
    if (plugin->abi_major != MOONMOD_NATIVE_ABI_MAJOR_V1) return NativePluginValidation::UnsupportedPluginAbi;
    if (plugin->minimum_host_abi_major != host_abi_major ||
        plugin->minimum_host_abi_minor > host_abi_minor) {
        return NativePluginValidation::UnsupportedHostAbi;
    }
    if (plugin->plugin_id == nullptr || !ValidPluginId(plugin->plugin_id)) {
        return NativePluginValidation::InvalidPluginId;
    }
    if (!ValidPluginId(expected_plugin_id) || std::string_view(plugin->plugin_id) != expected_plugin_id) {
        return NativePluginValidation::PluginIdMismatch;
    }
    if (plugin->required_permission_count > kMaximumNativePermissions ||
        (plugin->required_permission_count != 0 && plugin->required_permissions == nullptr)) {
        return NativePluginValidation::InvalidPermissionList;
    }
    if (std::ranges::find(manifest_permissions, "native.extension") == manifest_permissions.end()) {
        return NativePluginValidation::InvalidPermissionList;
    }
    for (std::uint32_t index = 0; index < plugin->required_permission_count; ++index) {
        const char *permission = plugin->required_permissions[index];
        if (permission == nullptr || !IsKnownPermission(permission)) {
            return NativePluginValidation::UnknownPermission;
        }
        const std::string_view name(permission);
        bool duplicate = false;
        for (std::uint32_t previous = 0; previous < index; ++previous) {
            if (plugin->required_permissions[previous] != nullptr &&
                std::string_view(plugin->required_permissions[previous]) == name) {
                duplicate = true;
                break;
            }
        }
        if (std::ranges::find(manifest_permissions, name) == manifest_permissions.end() || duplicate) {
            return NativePluginValidation::InvalidPermissionList;
        }
    }
    if (plugin->load == nullptr) return NativePluginValidation::MissingLoadCallback;
    if (plugin->unload == nullptr) return NativePluginValidation::MissingUnloadCallback;
    return NativePluginValidation::Valid;
}

std::string_view NativePluginValidationMessage(NativePluginValidation validation) {
    switch (validation) {
        case NativePluginValidation::Valid: return "native plugin descriptor is compatible";
        case NativePluginValidation::NullDescriptor: return "native plugin entry returned a null descriptor";
        case NativePluginValidation::IncompleteDescriptor: return "native plugin descriptor is smaller than ABI v1";
        case NativePluginValidation::UnsupportedPluginAbi: return "native plugin ABI major is not supported";
        case NativePluginValidation::UnsupportedHostAbi: return "native plugin requires a newer or different host ABI";
        case NativePluginValidation::PluginIdMismatch: return "native plugin id does not match its package declaration";
        case NativePluginValidation::InvalidPluginId: return "native plugin id is invalid";
        case NativePluginValidation::InvalidPermissionList: return "native plugin permissions are invalid or undeclared";
        case NativePluginValidation::UnknownPermission: return "native plugin requests an unknown permission";
        case NativePluginValidation::MissingLoadCallback: return "native plugin does not export a load callback";
        case NativePluginValidation::MissingUnloadCallback: return "native plugin does not export an unload callback";
    }
    return "native plugin descriptor validation failed";
}

} // namespace moonmod
