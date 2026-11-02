#pragma once

#include <functional>
#include <memory>
#include <string_view>

#include <filesystem>
#include <memory>
#include "moonmod/runtime.hpp"

namespace audio {
class Mixer;
class PcmTrack;
class StreamChannel;
} // namespace audio

namespace moonmod {

class AudioApi {
public:
    explicit AudioApi(audio::Mixer &mixer);
    ~AudioApi();
    AudioApi(const AudioApi &) = delete;
    AudioApi &operator=(const AudioApi &) = delete;

    ApiCallResult Call(std::string_view mod_id, const std::filesystem::path &package_root,
                       const ApiOperationSpec &operation, const Json &arguments);
    void UnloadOwner(std::string_view mod_id);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Binds the package-scoped audio API to the game's mixer and advertises its
// catalogued operations. It composes owner cleanup with any adapter cleanup.
void ConfigureAudioApi(RuntimeConfig &config);

// The game's own sound, set by the game adapter: a sound effect by its table number, and a music
// set loaded and one of its tracks played, or the music stopped.
struct RetailAudio {
    std::function<void(int se_no, int volume)>      play_se;
    std::function<bool(int set_no, int track_no)>   play_music;
    std::function<void()>                           stop_music;
};
void SetRetailAudio(RetailAudio retail);

// A mod's recording for a game music sequence (audio.music_set), or null. Asked before the
// soundtrack option's own mapping, so a mod's music plays whichever soundtrack is chosen.
std::shared_ptr<const audio::PcmTrack> ModMusicFor(std::string_view sequence);

} // namespace moonmod
