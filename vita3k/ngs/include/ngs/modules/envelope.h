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

#pragma once

#include <ngs/system.h>
#include <ngs/types.h>

#define SCE_NGS_ENVELOPE_PARAMS_STRUCT_ID 0x01015CE3

#define SCE_NGS_ENVELOPE_MAX_POINTS 4

enum SceNgsEnvelopeCurveType : uint32_t {
    SCE_NGS_ENVELOPE_LINEAR,
    SCE_NGS_ENVELOPE_CURVED
};

struct SceNgsEnvelopePoint {
    SceUInt32 uMsecsToNextPoint;
    SceFloat32 fAmplitude;
    SceNgsEnvelopeCurveType eCurveType;
};

struct SceNgsEnvelopeParams {
    SceNgsParamsDescriptor desc;
    SceNgsEnvelopePoint envelopePoints[SCE_NGS_ENVELOPE_MAX_POINTS];
    SceUInt32 uReleaseMsecs;
    SceUInt32 uNumPoints;
    SceUInt32 uLoopStart;
    SceInt32 nLoopEnd;
};

struct SceNgsEnvelopeStates {
    SceFloat32 fCurrentHeight;
    SceFloat32 fPosition;
    SceFloat32 fReleaseScale;
    SceInt32 nCurrentPoint;
    SceInt32 nReleasing;
};

namespace ngs {

// Host-side position of the envelope between two scheduler updates; the guest-visible
// SceNgsEnvelopeStates is refreshed from it at every update.
struct EnvelopeLogicalState : public ModuleLogicalState {
    int32_t point = 0; // the segment point -> point + 1 is being played
    double segment_ms = 0.0; // time spent in that segment
    bool holding = false; // the envelope reached its end (or a zero-length loop) and holds its amplitude
    bool releasing = false; // keyed off: fading from release_start to 0 over uReleaseMsecs (envelope release)
    double release_ms = 0.0; // time spent in the release
    float release_start = 0.0f;
    float height = 0.0f; // amplitude applied to the last frame
};

struct EnvelopeModule : public Module {
public:
    bool process(KernelState &kern, const MemState &mem, const SceUID thread_id, ModuleData &data, std::unique_lock<std::recursive_mutex> &scheduler_lock, std::unique_lock<std::mutex> &voice_lock) override;
    uint32_t module_id() const override { return 0x5CE3; }
    uint32_t get_guest_state_size() const override { return sizeof(SceNgsEnvelopeStates); }
    std::unique_ptr<ModuleLogicalState> create_logical_state() const override;
    void on_state_change(const MemState &mem, ModuleData &data, const VoiceState previous) override;
    // True when this module will turn a key-off into a release (it has parameters and is not bypassed).
    static bool handles_key_off(const MemState &mem, ModuleData &data);

    static constexpr uint32_t get_max_parameter_size() {
        return sizeof(SceNgsEnvelopeParams);
    }
    uint32_t get_buffer_parameter_size() const override {
        return get_max_parameter_size();
    }
};

} // namespace ngs
