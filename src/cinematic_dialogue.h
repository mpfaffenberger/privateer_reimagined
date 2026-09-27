#pragma once

#include "audio.h"

namespace cinematic {
// One owned speech lane. Music and action SFX keep their independent voices.
// Explicit stop on director teardown/seek avoids speech surviving a replay.
class DialogueTrack {
public:
    void stop() {
        const VoiceId previous = voice_;
        voice_ = 0;
        if (previous) audio::stop(previous); // finished/stale IDs are safe
    }

    void play(const std::string& path) {
        stop();
        if (!path.empty()) voice_ = audio::play_file(path, 1.0f, false);
    }

private:
    VoiceId voice_ = 0;
};
} // namespace cinematic
