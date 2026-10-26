#pragma once

#include <array>
#include <chrono>
#include <cstdint>

namespace dc::load_profile {

enum class Kind {
    BackgroundRead,
    Metadata,
    Png,
    Texture,
    Visual,
    Mesh,
    Count
};

struct Counter {
    std::uint64_t calls = 0;
    std::uint64_t nanoseconds = 0;
};

inline thread_local bool                                                    active = false;
inline thread_local std::array<Counter, static_cast<unsigned>(Kind::Count)> counters;

// These are nested wall spans on the calling thread, not GPU execution timings.
class Activation {
public:
    Activation() : previous(active) { active = true; }

    ~Activation() { active = previous; }

private:
    bool previous;
};

class Span {
public:
    explicit Span(Kind kind) : kind(kind), enabled(active) {
        if (enabled) {
            started = Clock::now();
        }
    }

    ~Span() {
        if (enabled) {
            const auto elapsed = Clock::now() - started;
            auto      &counter = counters[static_cast<unsigned>(kind)];
            ++counter.calls;
            counter.nanoseconds += std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
        }
    }

private:
    using Clock = std::chrono::steady_clock;
    Kind              kind;
    bool              enabled;
    Clock::time_point started;
};

inline double MillisecondsSince(const decltype(counters) &before, Kind kind) {
    const auto i = static_cast<unsigned>(kind);
    return static_cast<double>(counters[i].nanoseconds - before[i].nanoseconds) / 1'000'000.0;
}

} // namespace dc::load_profile
