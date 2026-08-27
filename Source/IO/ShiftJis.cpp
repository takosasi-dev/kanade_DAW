#include "IO/ShiftJis.h"
#include <vector>

#if JUCE_WINDOWS
 #include <windows.h>
#elif JUCE_MAC
 #include <CoreFoundation/CoreFoundation.h>
#endif

namespace ss
{
namespace
{
    /* ponytail: this is a byte-shape heuristic, not a full UTF-8 validator -
       it does not reject overlong encodings, surrogate-range codepoints, or
       out-of-range codepoints. For actual UST lyric text (hiragana/katakana,
       Shift-JIS lead bytes 0x81-0x9F) this never matters: those lead bytes
       always fail every check below and correctly fall through to
       decodeShiftJis(). The narrow residual risk is Shift-JIS text using
       lead bytes 0xE0-0xFC (rarer kanji / IBM-extension ranges) coinciding
       with a byte pattern that also looks like valid multi-byte UTF-8 -
       tightening this check (rejecting overlongs/surrogates) does not
       actually close that gap, since SJIS's real second-byte range overlaps
       genuinely valid UTF-8 continuation shapes there too. Upgrade path if
       this bites in practice: a per-file encoding override in Preferences,
       or a statistical charset-detection pass instead of pure byte-shape
       sniffing. */
    bool isValidUtf8 (const juce::uint8* data, size_t size)
    {
        size_t i = 0;

        while (i < size)
        {
            const auto b0 = data[i];
            size_t extra;

            if      ((b0 & 0x80) == 0x00) extra = 0;
            else if ((b0 & 0xE0) == 0xC0) extra = 1;
            else if ((b0 & 0xF0) == 0xE0) extra = 2;
            else if ((b0 & 0xF8) == 0xF0) extra = 3;
            else return false;

            if (i + extra >= size)
                return false;

            for (size_t k = 1; k <= extra; ++k)
                if ((data[i + k] & 0xC0) != 0x80)
                    return false;

            i += extra + 1;
        }

        return true;
    }

   #if JUCE_WINDOWS
    juce::String decodeShiftJis (const juce::uint8* data, size_t size)
    {
        if (size == 0)
            return {};

        const auto wideLen = ::MultiByteToWideChar (932, 0, (LPCSTR) data, (int) size, nullptr, 0);

        if (wideLen <= 0)
            return {};

        std::vector<WCHAR> wide ((size_t) wideLen);
        ::MultiByteToWideChar (932, 0, (LPCSTR) data, (int) size, wide.data(), wideLen);

        return juce::String (juce::CharPointer_UTF16 ((const juce::CharPointer_UTF16::CharType*) wide.data()),
                             (size_t) wideLen);
    }
   #elif JUCE_MAC
    juce::String decodeShiftJis (const juce::uint8* data, size_t size)
    {
        // ponytail: cannot be tested here - no Mac dev/test rig exists for this
        // project (see docs/STATUS.md's precedent for the same limitation on
        // the plugin-editor HWND-reparenting code). Uses CFString's own table
        // rather than a hand-rolled one, same reasoning as the Windows path.
        auto* cfData = CFDataCreate (kCFAllocatorDefault, data, (CFIndex) size);
        auto* cfString = CFStringCreateFromExternalRepresentation (kCFAllocatorDefault, cfData,
                                                                   kCFStringEncodingDOSJapanese);
        juce::String result;

        if (cfString != nullptr)
        {
            result = juce::String::fromCFString (cfString);
            CFRelease (cfString);
        }

        CFRelease (cfData);
        return result;
    }
   #else
    juce::String decodeShiftJis (const juce::uint8*, size_t) { return {}; }
   #endif
}

juce::String decodeUstText (const void* data, size_t numBytes)
{
    const auto* bytes = static_cast<const juce::uint8*> (data);

    if (isValidUtf8 (bytes, numBytes))
        return juce::String::fromUTF8 ((const char*) bytes, (int) numBytes);

    return decodeShiftJis (bytes, numBytes);
}
}
