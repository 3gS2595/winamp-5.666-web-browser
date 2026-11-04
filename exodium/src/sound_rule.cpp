#include "sound_rule.h"

namespace exodium {

SoundStep SoundRule::Look(const SoundSituation& now) {
    if (!now.audible) declined_ = false;
    else if (wasOurs_ && !now.ours) declined_ = true;
    wasOurs_ = now.ours;

    if (now.ours) {
        if (!now.enabled) return SoundStep::kStop;
        if (now.audible) {
            quietSince_.reset();
            return SoundStep::kNothing;
        }
        if (!quietSince_) quietSince_ = now.nowMs;
        return now.nowMs - *quietSince_ >= kQuietMs ? SoundStep::kStop : SoundStep::kNothing;
    }
    quietSince_.reset();
    if (!now.enabled || !now.audible || declined_ || now.winamp != WinampPlayback::kStopped) {
        return SoundStep::kNothing;
    }
    return SoundStep::kStart;
}

void SoundRule::Did(SoundStep step) noexcept {
    if (step == SoundStep::kNothing) return;
    wasOurs_ = step == SoundStep::kStart;
    quietSince_.reset();
}

} // namespace exodium
