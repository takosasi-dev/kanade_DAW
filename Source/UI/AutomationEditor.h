#pragma once
#include "Core/Project.h"
#include <juce_core/juce_core.h>
#include <utility>
#include <vector>

namespace ss::automation
{
    /*  The breakpoint maths behind the timeline's automation lanes (spec 8.4.5).

        Everything here is pure: it takes the point list straight out of
        Track::AutomationLane and hands back numbers, so it can be unit tested
        without a project, a device or a window.

        Parameter ids follow the convention the mixer reads on the audio thread:
            "gain"                        0..1 -> -60..+6 dB
            "pan"                         0..1 -> -1..+1
            "mute"                        >= 0.5 is muted
            "fx:<slot>:<paramId>"         0..1 across that parameter's min..max
            "plugin:<slot>:<paramIndex>"  0..1
        Values interpolate linearly and hold flat outside the outermost points. */

    using Points = std::vector<std::pair<double, float>>;

    /** The lane's value at `beat`.  Flat before the first point and after the
        last one; 0 for an empty lane (callers show "-" instead). */
    float valueAt (const Points&, double beat) noexcept;

    /** Inserts (beat, value) keeping the list sorted by beat.  A point already
        sitting on `beat` is overwritten rather than duplicated.  Returns the
        index the point ended up at. */
    int insertPoint (Points&, double beat, float value);

    /** Erases `indices` (any order, duplicates and out-of-range ignored).
        An emptied lane is left in place - that is a lane with no automation,
        not a broken one. */
    void erasePoints (Points&, std::vector<int> indices);

    /** Shrinks `wanted` (a beat delta applied to every index in `moving`) until
        none of them would cross a point that is staying put, or go negative.
        Keeping the list sorted this way means indices survive a drag and the
        audio thread never sees an out-of-order lane. */
    double clampBeatDelta (const Points&, const std::vector<int>& moving, double wanted) noexcept;

    /** Readout text for a normalised value, per the id convention above. */
    juce::String formatValue (const juce::String& parameterId, float value);

    /** Lane label, e.g. "Gain", "Reverb: mix", "Serum: P12". */
    juce::String displayName (const Track&, const juce::String& parameterId);

    /** The slot index encoded in an "fx:" / "plugin:" id, or -1. */
    int slotIndexOf (const juce::String& parameterId) noexcept;
}
