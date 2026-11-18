#include "sound_rule.h"

namespace exodium {

SoundStep SoundRule::Look(const SoundSituation& now) {
    if (!now.audible) declined_ = false;
    else if (wasOurs_ && !now.ours) declined_ = true;
    wasOurs_ = now.ours;

    if (now.ours) return now.enabled && now.audible ? SoundStep::kNothing : SoundStep::kStop;
    if (!now.enabled || !now.audible || declined_ || now.winamp != WinampPlayback::kStopped) {
        return SoundStep::kNothing;
    }
    return SoundStep::kStart;
}

void SoundRule::Did(SoundStep step) noexcept {
    if (step == SoundStep::kNothing) return;
    wasOurs_ = step == SoundStep::kStart;
}

} // namespace exodium
