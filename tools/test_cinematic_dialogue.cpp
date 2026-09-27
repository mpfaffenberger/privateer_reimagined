#include "cinematic_dialogue.h"
#include <cstdio>
#include <string>
#include <vector>

namespace {
std::vector<std::string> events;
VoiceId next_voice = 1;
int failures = 0;
void check(bool ok, const char* name) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    failures += !ok;
}
}

// Stub only the audio device: exercise the actual dialogue ownership logic.
namespace audio {
VoiceId play_file(const std::string& path, float gain, bool loop) {
    check(gain == 1.0f && !loop, "dialogue is full-volume nonlooping playback");
    events.push_back("play:" + path);
    return path == "missing" ? 0 : next_voice++;
}
void stop(VoiceId voice) { events.push_back("stop:" + std::to_string(voice)); }
}

int main() {
    cinematic::DialogueTrack speech;
    speech.stop();
    check(events.empty(), "empty teardown makes no audio calls");
    speech.play("vera");
    speech.play("grayson");
    check(events == std::vector<std::string>{"play:vera", "stop:1", "play:grayson"},
          "old speech stops before the next line starts");
    speech.stop(); // director seek/skip/end/reload path
    speech.stop();
    check(events.back() == "stop:2" && events.size() == 4,
          "teardown stops owned speech exactly once");
    speech.play("vera-replay");
    speech.play("");
    check(events.back() == "stop:3", "silent line clears old speech without opening an empty file");
    speech.play("missing");
    const auto before = events.size();
    speech.stop();
    check(events.size() == before, "failed audio load leaves no voice to stop");
    speech.play("final-line");
    speech.stop();
    check(events.back() == "stop:4", "replay after failed load has clean ownership");
    return failures ? 1 : 0;
}
