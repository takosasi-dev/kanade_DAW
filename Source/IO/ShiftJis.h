#pragma once
#include <juce_core/juce_core.h>

namespace ss
{
    /** Classic .ust / oto.ini files are frequently Shift-JIS, not UTF-8. Tries
        UTF-8 first and falls back to the platform's own Shift-JIS conversion
        table (Win32 MultiByteToWideChar / macOS CFString) when the bytes are
        not valid UTF-8 - deliberately not a hand-rolled JIS X 0208 table,
        which would risk silent, hard-to-catch mis-decoding of lyrics. */
    juce::String decodeUstText (const void* data, size_t numBytes);
}
