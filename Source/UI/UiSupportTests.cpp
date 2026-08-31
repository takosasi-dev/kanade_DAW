#include "UI/UiSupport.h"

namespace ss
{

class UiSupportUnitTests final : public juce::UnitTest
{
public:
    UiSupportUnitTests() : juce::UnitTest ("ScoreSmith UiSupport", "ScoreSmith") {}

    void runTest() override
    {
        beginTest ("formatPan");
        {
            expectEquals (formatPan (0.0f), juce::String ("C"));
            expectEquals (formatPan (-0.2f), juce::String ("L20"));
            expectEquals (formatPan (0.45f), juce::String ("R45"));
            expectEquals (formatPan (-1.0f), juce::String ("L100"));
            expectEquals (formatPan (1.0f), juce::String ("R100"));
        }
    }
};

static UiSupportUnitTests uiSupportUnitTests;

}
