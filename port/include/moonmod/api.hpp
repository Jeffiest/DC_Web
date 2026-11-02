#pragma once

#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "moonmod/manifest.hpp"

namespace moonmod {

struct ApiOperationSpec {
    std::string_view path;
    std::string_view domain;
    std::string_view operation;
    std::string_view phase;
    std::string_view permission;
    std::string_view summary;
    int              domain_api_major = 1;
};

struct EventSpec {
    std::string_view name;
    std::string_view domain;
    std::string_view games;
    std::string_view summary;
};

struct ApiCapability {
    std::string path;
    bool        supported = false;
    std::string reason;
};

enum class ApiCallStatus {
    Ok,
    Unsupported,
    InvalidArgument,
    PermissionDenied,
    PhaseViolation,
    Failed,
};

struct ApiCallResult {
    ApiCallStatus status = ApiCallStatus::Unsupported;
    Json          value;
    std::string   message;
};

using ApiCallHandler = std::function<ApiCallResult(std::string_view mod_id, const ApiOperationSpec &operation,
                                                    const Json &arguments)>;

std::span<const ApiOperationSpec> ApiCatalog();
std::span<const EventSpec>        EventCatalog();
const ApiOperationSpec           *FindApiOperation(std::string_view path);
bool                              IsCoreApiOperation(std::string_view path);
void                              CompleteApiCapabilityLedger(std::string_view game_id,
                                                               std::vector<ApiCapability> &capabilities);
std::string_view                  ApiCallStatusName(ApiCallStatus status);
bool                              IsKnownPermission(std::string_view permission);

} // namespace moonmod
