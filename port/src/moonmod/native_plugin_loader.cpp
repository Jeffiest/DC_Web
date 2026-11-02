#include "moonmod/native_plugin_loader.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <ranges>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "moonmod/native_plugin_contract.hpp"

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>


#endif

namespace moonmod {
namespace {

constexpr std::size_t kNativeStringLimit = 4096;
constexpr std::size_t kNativeJsonLimit = 64 * 1024;
constexpr std::size_t kNativeEventLimit = 128;

bool ReadCString(const char *value, std::size_t limit, std::string_view &out) {
    if (value == nullptr) return false;
    std::size_t size = 0;
    while (size <= limit && value[size] != '\0') ++size;
    if (size > limit) return false;
    out = std::string_view(value, size);
    return true;
}

MoonModNativeStatusV1 WriteBuffer(MoonModNativeBufferV1 *buffer, std::string_view value) {
    if (buffer == nullptr || (buffer->capacity != 0 && buffer->data == nullptr)) {
        return MOONMOD_NATIVE_INVALID_ARGUMENT_V1;
    }
    if (value.size() > UINT32_MAX) return MOONMOD_NATIVE_FAILED_V1;
    buffer->length = static_cast<std::uint32_t>(value.size());
    if (buffer->capacity <= value.size()) return MOONMOD_NATIVE_BUFFER_TOO_SMALL_V1;
    if (!value.empty()) std::memcpy(buffer->data, value.data(), value.size());
    buffer->data[value.size()] = '\0';
    return MOONMOD_NATIVE_OK_V1;
}

MoonModNativeStatusV1 ToNativeStatus(ApiCallStatus status) {
    switch (status) {
        case ApiCallStatus::Ok: return MOONMOD_NATIVE_OK_V1;
        case ApiCallStatus::Unsupported: return MOONMOD_NATIVE_UNSUPPORTED_V1;
        case ApiCallStatus::InvalidArgument: return MOONMOD_NATIVE_INVALID_ARGUMENT_V1;
        case ApiCallStatus::PermissionDenied: return MOONMOD_NATIVE_PERMISSION_DENIED_V1;
        case ApiCallStatus::PhaseViolation:
        case ApiCallStatus::Failed: return MOONMOD_NATIVE_FAILED_V1;
    }
    return MOONMOD_NATIVE_FAILED_V1;
}

std::string PlatformName() {
#if defined(_WIN32) && defined(_M_X64)
    return "windows-x64";
#elif defined(_WIN32) && defined(_M_ARM64)
    return "windows-arm64";
#elif defined(_WIN32)
    return "windows-x86";
#elif defined(__APPLE__) && defined(__aarch64__)
    return "macos-arm64";
#elif defined(__APPLE__)
    return "macos-x64";
#elif defined(__aarch64__)
    return "linux-arm64";
#elif defined(__x86_64__)
    return "linux-x64";
#else
    return "unknown";
#endif
}

} // namespace

struct NativePluginInstance::Impl {
    struct Subscription {
        std::string event_name;
        MoonModNativeEventCallbackV1 callback = nullptr;
        void *user_data = nullptr;
    };

    NativePluginLoadConfig config;
    std::vector<std::string> granted_permissions;
    std::map<std::uint64_t, Subscription> subscriptions;
    std::uint64_t next_subscription = 1;
    std::thread::id game_thread;
    MoonModNativeHostV1 host{};
    const MoonModNativePluginV1 *plugin = nullptr;
    void *plugin_state = nullptr;
    bool load_attempted = false;
    bool loaded = false;
#if defined(_WIN32)
    HMODULE library = nullptr;
#else
    void *library = nullptr;
#endif

    bool HasPermission(std::string_view permission) const {
        return std::ranges::find(granted_permissions, permission) != granted_permissions.end();
    }

    bool OnGameThread() const { return std::this_thread::get_id() == game_thread; }

    static Impl *From(void *context) { return static_cast<Impl *>(context); }

    static MoonModNativeStatusV1 HostLog(void *context, const char *level, const char *message) {
        Impl *self = From(context);
        if (self == nullptr || (!self->loaded && !self->load_attempted)) return MOONMOD_NATIVE_FAILED_V1;
        if (!self->OnGameThread()) return MOONMOD_NATIVE_FAILED_V1;
        if (!self->HasPermission("native.extension")) return MOONMOD_NATIVE_PERMISSION_DENIED_V1;
        std::string_view parsed_level, parsed_message;
        if (!ReadCString(level, 16, parsed_level) || !ReadCString(message, kNativeStringLimit, parsed_message) ||
            parsed_message.empty()) return MOONMOD_NATIVE_INVALID_ARGUMENT_V1;
        if (parsed_level != "debug" && parsed_level != "info" && parsed_level != "warning" && parsed_level != "error") {
            return MOONMOD_NATIVE_INVALID_ARGUMENT_V1;
        }
        if (self->config.log) self->config.log(parsed_level, self->config.plugin_id, parsed_message);
        return MOONMOD_NATIVE_OK_V1;
    }

    const ApiCapability *Capability(std::string_view path) const {
        const auto found = std::ranges::find(config.api_capabilities, path, &ApiCapability::path);
        return found == config.api_capabilities.end() ? nullptr : &*found;
    }

    bool OperationPermissionGranted(const ApiOperationSpec &operation) const {
        return operation.permission.empty() || HasPermission(operation.permission);
    }

    static MoonModNativeStatusV1 HostApiSupports(void *context, const char *path, int32_t *supported,
                                                  MoonModNativeBufferV1 *reason) {
        Impl *self = From(context);
        if (self == nullptr || !self->OnGameThread()) return MOONMOD_NATIVE_FAILED_V1;
        std::string_view parsed_path;
        if (!ReadCString(path, 256, parsed_path) || parsed_path.empty() || supported == nullptr || reason == nullptr) {
            return MOONMOD_NATIVE_INVALID_ARGUMENT_V1;
        }
        const ApiOperationSpec *operation = FindApiOperation(parsed_path);
        if (operation == nullptr) return MOONMOD_NATIVE_INVALID_ARGUMENT_V1;
        if (!self->OperationPermissionGranted(*operation)) {
            *supported = 0;
            WriteBuffer(reason, "native extension did not declare the operation permission");
            return MOONMOD_NATIVE_PERMISSION_DENIED_V1;
        }
        const ApiCapability *capability = self->Capability(parsed_path);
        if (capability == nullptr) {
            *supported = 0;
            return WriteBuffer(reason, "this game adapter does not advertise the operation");
        }
        *supported = capability->supported ? 1 : 0;
        return WriteBuffer(reason, capability->reason);
    }

    static MoonModNativeStatusV1 HostApiCall(void *context, const char *path, const char *arguments_json,
                                             MoonModNativeBufferV1 *result_json,
                                             MoonModNativeBufferV1 *error_json) {
        Impl *self = From(context);
        if (self == nullptr || !self->OnGameThread()) return MOONMOD_NATIVE_FAILED_V1;
        std::string_view parsed_path, parsed_arguments;
        if (!ReadCString(path, 256, parsed_path) || parsed_path.empty() ||
            !ReadCString(arguments_json, kNativeJsonLimit, parsed_arguments) || result_json == nullptr ||
            error_json == nullptr || !self->config.api_handler) return MOONMOD_NATIVE_INVALID_ARGUMENT_V1;
        const ApiOperationSpec *operation = FindApiOperation(parsed_path);
        if (operation == nullptr) return MOONMOD_NATIVE_INVALID_ARGUMENT_V1;
        if (!self->OperationPermissionGranted(*operation)) {
            WriteBuffer(error_json, Json{{"code", "permission_denied"},
                                        {"message", "native extension did not declare the operation permission"}}.dump());
            return MOONMOD_NATIVE_PERMISSION_DENIED_V1;
        }
        const ApiCapability *capability = self->Capability(parsed_path);
        if (capability == nullptr || !capability->supported) {
            const std::string reason = capability == nullptr ? "this game adapter does not advertise the operation"
                                                              : capability->reason;
            WriteBuffer(error_json, Json{{"code", "unsupported"}, {"message", reason}}.dump());
            return MOONMOD_NATIVE_UNSUPPORTED_V1;
        }
        try {
            const Json arguments = Json::parse(parsed_arguments);
            if (!arguments.is_array()) return MOONMOD_NATIVE_INVALID_ARGUMENT_V1;
            const ApiCallResult result = self->config.api_handler(self->config.plugin_id, *operation, arguments);
            if (result.status == ApiCallStatus::Ok) {
                const std::string serialized = result.value.dump();
                const MoonModNativeStatusV1 written = WriteBuffer(result_json, serialized);
                WriteBuffer(error_json, "");
                return written;
            }
            WriteBuffer(result_json, "");
            const std::string failure = Json{{"code", ApiCallStatusName(result.status)},
                                             {"message", result.message}}.dump();
            const MoonModNativeStatusV1 error_written = WriteBuffer(error_json, failure);
            if (error_written != MOONMOD_NATIVE_OK_V1) return error_written;
            return ToNativeStatus(result.status);
        } catch (const std::exception &) {
            WriteBuffer(error_json, Json{{"code", "invalid_argument"}, {"message", "arguments are not valid JSON"}}.dump());
            return MOONMOD_NATIVE_INVALID_ARGUMENT_V1;
        }
    }

    static MoonModNativeStatusV1 HostSubscribe(void *context, const char *event_name,
                                               MoonModNativeEventCallbackV1 callback, void *user_data,
                                               uint64_t *subscription_id) {
        Impl *self = From(context);
        if (self == nullptr || !self->OnGameThread()) return MOONMOD_NATIVE_FAILED_V1;
        std::string_view parsed_name;
        if (!ReadCString(event_name, 128, parsed_name) || parsed_name.empty() || callback == nullptr ||
            subscription_id == nullptr) return MOONMOD_NATIVE_INVALID_ARGUMENT_V1;
        if (!self->HasPermission("game.read")) return MOONMOD_NATIVE_PERMISSION_DENIED_V1;
        if (self->subscriptions.size() >= kNativeEventLimit || self->next_subscription == 0) {
            return MOONMOD_NATIVE_FAILED_V1;
        }
        const std::uint64_t id = self->next_subscription++;
        self->subscriptions.emplace(id, Subscription{std::string(parsed_name), callback, user_data});
        *subscription_id = id;
        return MOONMOD_NATIVE_OK_V1;
    }

    static MoonModNativeStatusV1 HostUnsubscribe(void *context, uint64_t subscription_id) {
        Impl *self = From(context);
        if (self == nullptr || !self->OnGameThread()) return MOONMOD_NATIVE_FAILED_V1;
        return self->subscriptions.erase(subscription_id) == 1 ? MOONMOD_NATIVE_OK_V1
                                                                : MOONMOD_NATIVE_INVALID_ARGUMENT_V1;
    }

    void Publish(std::string_view event_name, const Json &payload, std::uint64_t frame) {
        if (!loaded || !OnGameThread()) return;
        const std::string name(event_name);
        const std::string serialized = payload.dump();
        if (serialized.size() > kNativeJsonLimit) return;
        std::vector<std::pair<std::uint64_t, Subscription>> snapshot;
        for (const auto &[id, subscription] : subscriptions) {
            if (subscription.event_name == event_name) snapshot.emplace_back(id, subscription);
        }
        for (const auto &[id, subscription] : snapshot) {
            if (!subscriptions.contains(id)) continue;
            try {
                subscription.callback(name.c_str(), serialized.c_str(), frame, subscription.user_data);
            } catch (...) {
                if (config.log) config.log("error", config.plugin_id, "native event callback threw across the ABI boundary");
            }
        }
    }

    void CloseLibrary() {
        if (library == nullptr) return;
#if defined(_WIN32)
        FreeLibrary(library);
#else
        dlclose(library);
#endif
        library = nullptr;
    }
};

NativePluginInstance::NativePluginInstance() : impl_(std::make_unique<Impl>()) {}
NativePluginInstance::~NativePluginInstance() { Unload(); }

bool NativePluginInstance::Load(NativePluginLoadConfig config, std::string &error) {
    Unload();
    error.clear();
    impl_->config = std::move(config);
    impl_->game_thread = std::this_thread::get_id();
    impl_->granted_permissions.clear();
    impl_->subscriptions.clear();
    impl_->next_subscription = 1;
    if (impl_->config.platform.empty()) impl_->config.platform = PlatformName();

#if defined(_WIN32)
    impl_->library = LoadLibraryExW(impl_->config.library.c_str(), nullptr,
                                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (impl_->library == nullptr) {
        error = "native extension library could not be loaded by Windows";
        return false;
    }
    const auto entry = reinterpret_cast<MoonModGetNativePluginV1>(
        GetProcAddress(impl_->library, MOONMOD_NATIVE_PLUGIN_ENTRY_NAME_V1));
#else
    impl_->library = dlopen(impl_->config.library.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (impl_->library == nullptr) {
        const char *reason = dlerror();
        error = reason == nullptr ? "native extension library could not be loaded" : reason;
        return false;
    }
    const auto entry = reinterpret_cast<MoonModGetNativePluginV1>(
        dlsym(impl_->library, MOONMOD_NATIVE_PLUGIN_ENTRY_NAME_V1));
#endif
    if (entry == nullptr) {
        error = "native extension does not export MoonMod_GetNativePluginV1";
        impl_->CloseLibrary();
        return false;
    }
    try {
        impl_->plugin = entry();
    } catch (...) {
        error = "native extension entry point threw across the ABI boundary";
        impl_->plugin = nullptr;
        impl_->CloseLibrary();
        return false;
    }
    std::vector<std::string_view> declared;
    declared.reserve(impl_->config.manifest_permissions.size());
    for (const auto &permission : impl_->config.manifest_permissions) declared.push_back(permission);
    const NativePluginValidation validation = ValidateNativePluginV1(impl_->plugin, impl_->config.plugin_id, 1, 0, declared);
    if (validation != NativePluginValidation::Valid) {
        error = std::string(NativePluginValidationMessage(validation));
        impl_->plugin = nullptr;
        impl_->CloseLibrary();
        return false;
    }
    for (std::uint32_t i = 0; i < impl_->plugin->required_permission_count; ++i) {
        impl_->granted_permissions.emplace_back(impl_->plugin->required_permissions[i]);
    }
    impl_->host.struct_size = sizeof(MoonModNativeHostV1);
    impl_->host.abi_major = MOONMOD_NATIVE_ABI_MAJOR_V1;
    impl_->host.abi_minor = MOONMOD_NATIVE_ABI_MINOR_V1;
    impl_->host.context = impl_.get();
    impl_->host.game_id = impl_->config.game_id.c_str();
    impl_->host.build_id = impl_->config.build_id.c_str();
    impl_->host.platform = impl_->config.platform.c_str();
    impl_->host.log = Impl::HostLog;
    impl_->host.api_supports = Impl::HostApiSupports;
    impl_->host.api_call_json = Impl::HostApiCall;
    impl_->host.subscribe = Impl::HostSubscribe;
    impl_->host.unsubscribe = Impl::HostUnsubscribe;
    std::array<char, 512> error_buffer{};
    MoonModNativeBufferV1 message{error_buffer.data(), static_cast<std::uint32_t>(error_buffer.size()), 0};
    impl_->load_attempted = true;
    MoonModNativeStatusV1 status = MOONMOD_NATIVE_FAILED_V1;
    try {
        status = impl_->plugin->load(&impl_->host, &impl_->plugin_state, &message);
    } catch (...) {
        error = "native extension load callback threw across the ABI boundary";
    }
    impl_->load_attempted = false;
    if (status != MOONMOD_NATIVE_OK_V1 || !error.empty()) {
        if (message.data != nullptr && message.length > 0) {
            error.assign(message.data, std::min<std::size_t>(message.length, error_buffer.size() - 1));
        }
        if (error.empty()) error = "native extension load callback failed with status " + std::to_string(status);
        impl_->plugin = nullptr;
        impl_->plugin_state = nullptr;
        impl_->subscriptions.clear();
        impl_->CloseLibrary();
        return false;
    }
    impl_->loaded = true;
    return true;
}

void NativePluginInstance::Publish(std::string_view event_name, const Json &payload, std::uint64_t frame) {
    impl_->Publish(event_name, payload, frame);
}

void NativePluginInstance::Unload() {
    if (!impl_) return;
    if (impl_->loaded && impl_->plugin != nullptr && impl_->library != nullptr) {
        try { impl_->plugin->unload(&impl_->host, impl_->plugin_state); } catch (...) {}
    }
    impl_->loaded = false;
    impl_->load_attempted = false;
    impl_->subscriptions.clear();
    impl_->plugin = nullptr;
    impl_->plugin_state = nullptr;
    impl_->granted_permissions.clear();
    impl_->CloseLibrary();
}

bool NativePluginInstance::Loaded() const { return impl_ && impl_->loaded; }
std::string_view NativePluginInstance::PluginId() const {
    return impl_ ? std::string_view(impl_->config.plugin_id) : std::string_view{};
}

} // namespace moonmod
