#pragma once

#include <string>

struct DiscordActivity {
    std::string details;
    std::string state;
    std::string large_image;
    std::string large_text;

    bool operator==(const DiscordActivity &) const = default;
};

inline constexpr const char *kDiscordClientId = "1556980864171245638";

void DiscordStart(std::string client_id);
void DiscordSetActivity(const DiscordActivity &activity);
void DiscordStop();

std::string DiscordActivityCommand(const DiscordActivity &activity, long long start_time, int pid, int nonce);
