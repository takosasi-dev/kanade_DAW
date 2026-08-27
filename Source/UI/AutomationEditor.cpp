#include "UI/AutomationEditor.h"
#include "Mixer/BuiltinFx.h"
#include "UI/UiSupport.h"
#include <algorithm>
#include <cmath>

namespace ss::automation
{
    namespace
    {
        // Two points closer together than this are the same point: one grid
        // click, one breakpoint.
        constexpr double sameBeat = 1.0e-6;
        // What a dragged point keeps between itself and a neighbour it must not
        // overtake.  Small enough to be invisible, big enough to survive the
        // double -> float -> double round trip through the project file.
        constexpr double minGap = 1.0e-4;
    }

    float valueAt (const Points& points, double beat) noexcept
    {
        if (points.empty())            return 0.0f;
        if (beat <= points.front().first) return points.front().second;
        if (beat >= points.back().first)  return points.back().second;

        // ponytail: linear scan. Lanes hold a handful of points and the UI walks
        // them once per repaint; make it a lower_bound if a lane ever holds
        // thousands.
        for (size_t i = 1; i < points.size(); ++i)
        {
            if (beat > points[i].first)
                continue;

            const double span = points[i].first - points[i - 1].first;
            if (span <= 0.0)
                return points[i].second;

            const double t = (beat - points[i - 1].first) / span;
            return (float) (points[i - 1].second + (points[i].second - points[i - 1].second) * t);
        }

        return points.back().second;
    }

    int insertPoint (Points& points, double beat, float value)
    {
        beat  = juce::jmax (0.0, beat);
        value = juce::jlimit (0.0f, 1.0f, value);

        for (size_t i = 0; i < points.size(); ++i)
        {
            if (std::abs (points[i].first - beat) < sameBeat)
            {
                points[i].second = value;
                return (int) i;
            }

            if (points[i].first > beat)
            {
                points.insert (points.begin() + (std::ptrdiff_t) i, { beat, value });
                return (int) i;
            }
        }

        points.push_back ({ beat, value });
        return (int) points.size() - 1;
    }

    void erasePoints (Points& points, std::vector<int> indices)
    {
        std::sort (indices.begin(), indices.end(), std::greater<int>());
        indices.erase (std::unique (indices.begin(), indices.end()), indices.end());

        for (const int i : indices)
            if (juce::isPositiveAndBelow (i, (int) points.size()))
                points.erase (points.begin() + (std::ptrdiff_t) i);
    }

    double clampBeatDelta (const Points& points, const std::vector<int>& moving, double wanted) noexcept
    {
        if (moving.empty() || points.empty())
            return wanted;

        std::vector<char> isMoving (points.size(), 0);
        for (const int i : moving)
            if (juce::isPositiveAndBelow (i, (int) points.size()))
                isMoving[(size_t) i] = 1;

        for (const int i : moving)
        {
            if (! juce::isPositiveAndBelow (i, (int) points.size()))
                continue;

            const double origin = points[(size_t) i].first;

            if (wanted > 0.0)
            {
                for (size_t j = (size_t) i + 1; j < points.size(); ++j)
                    if (! isMoving[j])
                    {
                        wanted = juce::jmin (wanted, points[j].first - minGap - origin);
                        break;
                    }
            }
            else
            {
                double floorBeat = 0.0;                 // beats are never negative
                for (int j = i - 1; j >= 0; --j)
                    if (! isMoving[(size_t) j])
                    {
                        floorBeat = points[(size_t) j].first + minGap;
                        break;
                    }

                wanted = juce::jmax (wanted, floorBeat - origin);
            }
        }

        return wanted > 0.0 ? juce::jmax (0.0, wanted) : juce::jmin (0.0, wanted);
    }

    juce::String formatValue (const juce::String& parameterId, float value)
    {
        value = juce::jlimit (0.0f, 1.0f, value);

        if (parameterId == "gain")
            return formatDb (-60.0f + value * 66.0f);

        if (parameterId == "pan")
        {
            const int amount = juce::roundToInt (std::abs (value * 2.0f - 1.0f) * 100.0f);
            return amount == 0 ? juce::String ("C")
                               : (value < 0.5f ? "L" : "R") + juce::String (amount);
        }

        if (parameterId == "mute")
            return value >= 0.5f ? TRANS ("Muted") : TRANS ("Unmuted");

        return juce::String (juce::roundToInt (value * 100.0f)) + "%";
    }

    int slotIndexOf (const juce::String& parameterId) noexcept
    {
        if (! parameterId.startsWith ("fx:") && ! parameterId.startsWith ("plugin:"))
            return -1;

        const auto slot = parameterId.fromFirstOccurrenceOf (":", false, false)
                                     .upToFirstOccurrenceOf (":", false, false);
        return slot.containsOnly ("0123456789") && slot.isNotEmpty() ? slot.getIntValue() : -1;
    }

    juce::String displayName (const Track& track, const juce::String& parameterId)
    {
        if (parameterId == "gain") return TRANS ("Gain");
        if (parameterId == "pan")  return TRANS ("Pan");
        if (parameterId == "mute") return TRANS ("Mute");

        const int slot = slotIndexOf (parameterId);
        const auto tail = parameterId.fromLastOccurrenceOf (":", false, false);

        if (parameterId.startsWith ("fx:"))
            return (juce::isPositiveAndBelow (slot, (int) track.builtinFx.size())
                      ? getBuiltinEffectDisplayName (track.builtinFx[(size_t) slot].type)
                      : TRANS ("FX")) + ": " + tail;

        if (parameterId.startsWith ("plugin:"))
        {
            // ponytail: parameters are named by index here because the real name
            // needs a loaded instance; the picker menu shows the real names.
            const auto owner = juce::isPositiveAndBelow (slot, (int) track.plugins.size())
                                 ? track.plugins[(size_t) slot].displayName : parameterId;
            return owner + ": P" + tail;
        }

        return parameterId;
    }
}

//==============================================================================
namespace ss
{

/*  The parts of the automation editor that are arithmetic rather than pixels:
    curve evaluation, breakpoint ordering, and the snap the lane shares with the
    rest of the timeline.  Everything else about a lane is a listening / looking
    test.                                                                      */
class AutomationUnitTests final : public juce::UnitTest
{
public:
    AutomationUnitTests() : juce::UnitTest ("ScoreSmith automation", "ScoreSmith") {}

    void runTest() override
    {
        using namespace ss::automation;

        beginTest ("curve evaluation");
        {
            expectEquals (valueAt ({}, 0.0), 0.0f);
            expectEquals (valueAt ({}, 99.0), 0.0f);

            const Points one { { 4.0, 0.25f } };
            expectEquals (valueAt (one, 0.0),  0.25f, "a single point holds flat to the left");
            expectEquals (valueAt (one, 4.0),  0.25f);
            expectEquals (valueAt (one, 99.0), 0.25f, "a single point holds flat to the right");

            const Points ramp { { 0.0, 0.0f }, { 4.0, 1.0f }, { 8.0, 0.5f } };
            expectEquals (valueAt (ramp, -1.0), 0.0f, "flat before the first point");
            expectEquals (valueAt (ramp, 0.0),  0.0f);
            expectWithinAbsoluteError (valueAt (ramp, 1.0), 0.25f, 1.0e-6f);
            expectWithinAbsoluteError (valueAt (ramp, 2.0), 0.5f,  1.0e-6f);
            expectEquals (valueAt (ramp, 4.0), 1.0f);
            expectWithinAbsoluteError (valueAt (ramp, 6.0), 0.75f, 1.0e-6f);
            expectEquals (valueAt (ramp, 8.0),  0.5f);
            expectEquals (valueAt (ramp, 40.0), 0.5f, "flat after the last point");

            // Two points on the same beat is a step, not a divide by zero.
            const Points step { { 0.0, 0.0f }, { 2.0, 0.0f }, { 2.0, 1.0f }, { 4.0, 1.0f } };
            expectEquals (valueAt (step, 1.0), 0.0f);
            expectEquals (valueAt (step, 3.0), 1.0f);
        }

        beginTest ("breakpoint insert keeps the lane sorted");
        {
            Points points;

            expectEquals (insertPoint (points, 4.0, 0.5f), 0);
            expectEquals (insertPoint (points, 1.0, 0.2f), 0, "an earlier beat lands in front");
            expectEquals (insertPoint (points, 8.0, 0.9f), 2, "a later beat lands at the end");
            expectEquals (insertPoint (points, 2.0, 0.3f), 1);
            expectEquals ((int) points.size(), 4);
            expectSorted (points);

            // Clicking the same spot twice moves the point instead of stacking
            // two of them - a lane must never hold a duplicate beat.
            expectEquals (insertPoint (points, 4.0, 0.75f), 2);
            expectEquals ((int) points.size(), 4);
            expectEquals (points[2].second, 0.75f);

            expectEquals (insertPoint (points, -5.0, 2.0f), 0, "a negative beat clamps to zero");
            expectEquals (points[0].first, 0.0);
            expectEquals (points[0].second, 1.0f, "values clamp into 0..1");
            expectEquals (insertPoint (points, 3.0, -1.0f), 3);
            expectEquals (points[3].second, 0.0f);
            expectSorted (points);
        }

        beginTest ("breakpoint delete");
        {
            Points points { { 0.0, 0.0f }, { 1.0, 0.1f }, { 2.0, 0.2f }, { 3.0, 0.3f } };

            erasePoints (points, { 2 });
            expectEquals ((int) points.size(), 3);
            expectEquals (points[2].first, 3.0, "the survivors keep their order");

            // Out of order, duplicated and out of range all at once: a marquee
            // selection hands them over exactly like this.
            erasePoints (points, { 2, 0, 2, 99, -1 });
            expectEquals ((int) points.size(), 1);
            expectEquals (points[0].first, 1.0);

            erasePoints (points, { 0 });
            expect (points.empty(), "deleting the last point leaves an empty lane");
            erasePoints (points, { 0 });
            expect (points.empty(), "deleting from an empty lane is a no-op");
        }

        beginTest ("dragging a breakpoint cannot reorder the lane");
        {
            const Points points { { 0.0, 0.0f }, { 4.0, 0.5f }, { 8.0, 1.0f } };

            expectWithinAbsoluteError (clampBeatDelta (points, { 1 }, 2.0), 2.0, 1.0e-9,
                                       "a move with room to spare is untouched");
            expect (clampBeatDelta (points, { 1 }, 9.0) < 4.0,
                    "the middle point stops short of the one after it");
            expect (clampBeatDelta (points, { 1 }, -9.0) > -4.0,
                    "and short of the one before it");

            // The whole selection moves rigidly, so only the points that are
            // staying put get a vote.
            expectWithinAbsoluteError (clampBeatDelta (points, { 1, 2 }, 3.0), 3.0, 1.0e-9,
                                       "nothing to the right of the selection means no limit");
            expect (clampBeatDelta (points, { 0, 1 }, 9.0) < 4.0);

            expectWithinAbsoluteError (clampBeatDelta (points, { 0 }, -3.0), 0.0, 1.0e-9,
                                       "beat 0 is the floor");
            expectWithinAbsoluteError (clampBeatDelta (points, {}, 5.0), 5.0, 1.0e-9);
            expectWithinAbsoluteError (clampBeatDelta ({}, { 0 }, 5.0), 5.0, 1.0e-9);

            // What the drag actually does with the clamped delta.
            Points moved = points;
            const double delta = clampBeatDelta (points, { 1 }, 9.0);
            moved[1].first += delta;
            expectSorted (moved);
        }

        beginTest ("lane edits snap to the timeline grid");
        {
            UiState state;
            state.snapEnabled = true;
            state.grid = Quantise::quarter;

            expectWithinAbsoluteError (state.snap (1.3), 1.0, 1.0e-9);
            expectWithinAbsoluteError (state.snap (1.6), 2.0, 1.0e-9);

            state.grid = Quantise::sixteenth;
            expectWithinAbsoluteError (state.snap (1.3), 1.25, 1.0e-9);

            state.snapEnabled = false;
            expectWithinAbsoluteError (state.snap (1.3), 1.3, 1.0e-9);

            // A drag snaps the point it started on, then moves the rest of the
            // selection by that same delta - so the anchor lands on the grid.
            state.snapEnabled = true;
            state.grid = Quantise::quarter;
            const double anchor = 2.0, raw = 1.4;
            const double snapped = state.snap (anchor + raw) - anchor;
            expectWithinAbsoluteError (anchor + snapped, 3.0, 1.0e-9);
        }

        beginTest ("value readouts follow the parameter-id convention");
        {
            expect (formatValue ("pan", 0.5f) == "C");
            expect (formatValue ("pan", 0.0f) == "L100");
            expect (formatValue ("pan", 1.0f) == "R100");
            expect (formatValue ("mute", 1.0f) != formatValue ("mute", 0.0f));
            expect (formatValue ("fx:0:mix", 0.5f) == "50%");
            expect (formatValue ("gain", 0.0f).isNotEmpty());

            expectEquals (slotIndexOf ("fx:3:mix"), 3);
            expectEquals (slotIndexOf ("plugin:12:7"), 12);
            expectEquals (slotIndexOf ("gain"), -1);
            expectEquals (slotIndexOf ("fx::mix"), -1);
        }
    }

private:
    void expectSorted (const automation::Points& points)
    {
        for (size_t i = 1; i < points.size(); ++i)
            expect (points[i].first >= points[i - 1].first,
                    "lane is out of order at index " + juce::String ((int) i));
    }
};

static AutomationUnitTests automationUnitTests;

}
