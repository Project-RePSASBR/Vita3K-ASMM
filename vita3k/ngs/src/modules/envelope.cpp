// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#include <ngs/modules/envelope.h>
#include <util/log.h>

#include <algorithm>
#include <memory>

namespace ngs {

// SceNgsEnvelopeParams holds up to SCE_NGS_ENVELOPE_MAX_POINTS points (amplitude + time to the next point).
// uLoopStart / nLoopEnd describe the sustain loop: nLoopEnd == -1 loops up to the last point, below -1 means
// no loop, and a loop of zero length holds its amplitude. Without a loop the envelope holds the amplitude of
// its last point once it gets there; when that amplitude is 0 the voice is finished (it can never become
// audible again). A key-off starts the release: the current amplitude fades to 0 over uReleaseMsecs, then
// the voice is finished (envelope release). Curves are treated as linear (tentative).
//
// PlayStation All-Stars (SCREAM) depends on this: 0 ms releases are its hard stops, 55 s releases let an
// interrupted one-shot play out, and short self-terminating envelopes silence sample tails.

namespace {

struct EnvelopeShape {
    int n = 1;
    int loop_start = 0;
    int loop_end = -1;
    bool loop = false;
    double loop_total_ms = 0.0;
    const SceNgsEnvelopeParams *p = nullptr;

    explicit EnvelopeShape(const SceNgsEnvelopeParams *params)
        : p(params) {
        n = std::clamp<int>(static_cast<int>(p->uNumPoints), 1, SCE_NGS_ENVELOPE_MAX_POINTS);
        if (p->nLoopEnd == -1)
            loop_end = n - 1;
        else if (p->nLoopEnd >= 0)
            loop_end = std::min<int>(p->nLoopEnd, n - 1);
        loop_start = std::min<int>(static_cast<int>(p->uLoopStart), n - 1);
        loop = loop_end >= 0 && loop_start <= loop_end;
        if (loop)
            for (int i = loop_start; i <= loop_end; i++)
                loop_total_ms += duration(i);
    }

    float amplitude(const int i) const { return p->envelopePoints[std::clamp(i, 0, n - 1)].fAmplitude; }
    double duration(const int i) const { return static_cast<double>(p->envelopePoints[std::clamp(i, 0, n - 1)].uMsecsToNextPoint); }

    float current(const EnvelopeLogicalState &s) const {
        if (s.holding || s.point >= n - 1)
            return amplitude(s.point);
        const double d = duration(s.point);
        if (d <= 0.0)
            return amplitude(s.point);
        const float t = static_cast<float>(std::clamp(s.segment_ms / d, 0.0, 1.0));
        return amplitude(s.point) + (amplitude(s.point + 1) - amplitude(s.point)) * t;
    }

    void advance(EnvelopeLogicalState &s, const double dt_ms) const {
        if (s.holding)
            return;
        s.segment_ms += dt_ms;
        // several zero-length points can be crossed in one step
        for (int guard = 0; guard < 2 * SCE_NGS_ENVELOPE_MAX_POINTS && s.segment_ms >= duration(s.point); guard++) {
            if (loop && s.point == loop_end) {
                if (loop_total_ms <= 0.0) {
                    s.point = loop_start;
                    s.segment_ms = 0.0;
                    s.holding = true;
                    return;
                }
                s.segment_ms -= duration(s.point);
                s.point = loop_start;
                continue;
            }
            if (s.point >= n - 1) {
                s.segment_ms = 0.0;
                s.holding = true;
                return;
            }
            s.segment_ms -= duration(s.point);
            s.point++;
        }
    }

    // the envelope ran to its end without a loop and is silent for good
    bool ended_silent(const EnvelopeLogicalState &s) const {
        return s.holding && !loop && s.point >= n - 1 && amplitude(s.point) <= 1e-4f;
    }
};

} // namespace

std::unique_ptr<ModuleLogicalState> EnvelopeModule::create_logical_state() const {
    return std::make_unique<EnvelopeLogicalState>();
}

void EnvelopeModule::on_state_change(const MemState &mem, ModuleData &data, const VoiceState previous) {
    if (data.parent->state == VOICE_STATE_ACTIVE && previous == VOICE_STATE_AVAILABLE) {
        *data.get_logical_state<EnvelopeLogicalState>() = EnvelopeLogicalState{};
        *data.get_state<SceNgsEnvelopeStates>() = SceNgsEnvelopeStates{};
    }
}

bool EnvelopeModule::handles_key_off(const MemState &mem, ModuleData &data) {
    if (data.is_bypassed)
        return false;
    const SceNgsParamsDescriptor *desc = data.get_parameters<SceNgsParamsDescriptor>(mem);
    return desc && desc->id == SCE_NGS_ENVELOPE_PARAMS_STRUCT_ID;
}

bool EnvelopeModule::process(KernelState &kern, const MemState &mem, const SceUID thread_id, ModuleData &data, std::unique_lock<std::recursive_mutex> &scheduler_lock, std::unique_lock<std::mutex> &voice_lock) {
    if (data.is_bypassed)
        return false;

    const SceNgsEnvelopeParams *params = data.get_parameters<SceNgsEnvelopeParams>(mem);
    if (!params || params->desc.id != SCE_NGS_ENVELOPE_PARAMS_STRUCT_ID)
        return false; // never configured: identity

    EnvelopeLogicalState *s = data.get_logical_state<EnvelopeLogicalState>();
    SceNgsEnvelopeStates *guest = data.get_state<SceNgsEnvelopeStates>();
    const EnvelopeShape shape(params);

    const int frames = data.parent->rack->system->granularity;
    const double dt_ms = 1000.0 / static_cast<double>(std::max(1, data.parent->rack->system->sample_rate));
    float *pcm = reinterpret_cast<float *>(data.parent->products[0].data); // interleaved stereo, may be null
    const double release_total_ms = static_cast<double>(params->uReleaseMsecs);

    if (data.parent->is_keyed_off && !s->releasing) {
        s->releasing = true;
        s->release_ms = 0.0;
        s->release_start = shape.current(*s);
    }

    bool finished = false;
    float amp = s->height;
    for (int i = 0; i < frames; i++) {
        if (s->releasing) {
            if (release_total_ms <= 0.0 || s->release_ms >= release_total_ms) {
                amp = 0.0f;
                finished = true;
            } else {
                amp = s->release_start * static_cast<float>(1.0 - s->release_ms / release_total_ms);
                s->release_ms += dt_ms;
            }
        } else {
            amp = shape.current(*s);
            shape.advance(*s, dt_ms);
            if (shape.ended_silent(*s)) {
                amp = 0.0f;
                finished = true;
            }
        }
        if (pcm) {
            if (finished) {
                std::fill(pcm + 2 * i, pcm + 2 * frames, 0.0f);
            } else {
                pcm[2 * i] *= amp;
                pcm[2 * i + 1] *= amp;
            }
        }
        if (finished)
            break;
    }
    s->height = amp;

    guest->fCurrentHeight = s->height;
    guest->fPosition = static_cast<float>(s->segment_ms);
    guest->fReleaseScale = (s->releasing && release_total_ms > 0.0) ? static_cast<float>(1.0 - std::min(1.0, s->release_ms / release_total_ms)) : 1.0f;
    guest->nCurrentPoint = s->point;
    guest->nReleasing = s->releasing ? 1 : 0;

    // silent for good (release done, or a one-shot envelope that ended at 0): the scheduler invokes the
    // finished callback and stops the voice, exactly as for a player that ran out of data
    return finished;
}
} // namespace ngs
