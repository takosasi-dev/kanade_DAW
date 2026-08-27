#include "AI/Transcriber.h"

#include <algorithm>

/*  Source separation (spec 8.2-1).

    This shells out to a local Demucs-compatible executable that the user points
    at in Preferences.  Nothing is bundled and nothing is downloaded: v0.6 fixed
    inference as local-only, so if no executable is configured the feature is
    simply unavailable and says so.  We never touch the network - if the
    configured tool wants to fetch model weights on first run, that is between
    the user and that tool.                                                     */

namespace ss
{

namespace
{
    /** Demucs prints a tqdm-style bar; the last percentage in the buffered tail
        is the freshest progress reading we can get without parsing the bar. */
    float parseProgressPercent (const juce::String& text)
    {
        const auto end = text.lastIndexOfChar ('%');

        if (end <= 0)
            return -1.0f;

        int start = end;

        while (start > 0)
        {
            const auto c = text[start - 1];

            if ((c >= '0' && c <= '9') || c == '.')
                --start;
            else
                break;
        }

        if (start == end)
            return -1.0f;

        return juce::jlimit (0.0f, 1.0f, text.substring (start, end).getFloatValue() * 0.01f);
    }

    bool isAudioFile (const juce::File& f)
    {
        return f.hasFileExtension ("wav;flac;aiff;aif;mp3;ogg;m4a");
    }
}

//==============================================================================
StemSeparator::StemSeparator (Settings& s) : settings (s) {}

StemSeparator::~StemSeparator()
{
    // Caller contract: do not destroy this while separate() is still running on
    // a worker.  Killing here only covers the "worker already returned" case.
    cancelled = true;

    if (process != nullptr)
        process->kill();
}

juce::String StemSeparator::toString (Stem s)
{
    switch (s)
    {
        case Stem::vocals: return "Vocals";
        case Stem::drums:  return "Drums";
        case Stem::bass:   return "Bass";
        case Stem::piano:  return "Piano";
        case Stem::guitar: return "Guitar";
        case Stem::other:  return "Other";
    }

    return "Other";
}

bool StemSeparator::isAvailable() const
{
    return settings.getStemSeparatorExecutable().existsAsFile();
}

juce::String StemSeparator::getUnavailableReason() const
{
    const auto exe = settings.getStemSeparatorExecutable();

    if (exe.getFullPathName().isEmpty())
        return "No stem separator is configured. Set Preferences > AI > Stem separator to a "
               "local Demucs-compatible executable; ScoreSmith never downloads one for you.";

    if (! exe.existsAsFile())
        return "The configured stem separator was not found: " + exe.getFullPathName();

    return {};
}

//==============================================================================
juce::Result StemSeparator::separate (const juce::File& source, const juce::File& outputFolder,
                                      std::vector<Output>& out,
                                      std::function<void (float, const juce::String&)> progress)
{
    out.clear();
    cancelled = false;

    if (! isAvailable())
        return juce::Result::fail (getUnavailableReason());

    if (! source.existsAsFile())
        return juce::Result::fail ("Source file not found: " + source.getFullPathName());

    if (const auto created = outputFolder.createDirectory(); created.failed())
        return created;

    const auto exe = settings.getStemSeparatorExecutable();

    // ponytail: one fixed argument shape ("-o <dir> <file>"), no model or
    // shifts/overlap options - a user who needs those wraps the tool in a script.
    // Kept to the flags every Demucs build and most wrapper scripts accept, so a
    // user's own shell script works as a drop-in.
    juce::StringArray args;
    args.add (exe.getFullPathName());
    args.add ("-o");
    args.add (outputFolder.getFullPathName());
    args.add (source.getFullPathName());

    if (progress)
        progress (0.0f, "Starting " + exe.getFileName() + "...");

    process = std::make_unique<juce::ChildProcess>();

    if (! process->start (args, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
    {
        process.reset();
        return juce::Result::fail ("Could not launch " + exe.getFullPathName());
    }

    juce::String tail;
    char buffer[2048];

    const auto drain = [&] ()
    {
        const auto numRead = process->readProcessOutput (buffer, (int) sizeof (buffer));

        if (numRead > 0)
        {
            tail += juce::String::fromUTF8 (buffer, numRead);

            // The progress bar spews a lot of text; only the tail is ever useful,
            // either for the percentage or for the error message at the end.
            if (tail.length() > 4096)
                tail = tail.getLastCharacters (4096);
        }

        return numRead;
    };

    while (process->isRunning())
    {
        // cancel() only raises the flag - the process handle stays owned by this
        // thread alone, which is cheaper than a lock and removes the race.
        if (cancelled.load())
        {
            process->kill();
            process.reset();
            return juce::Result::fail ("Separation cancelled.");
        }

        if (drain() <= 0)
        {
            juce::Thread::sleep (50);
            continue;
        }

        if (progress)
            if (const auto pct = parseProgressPercent (tail); pct >= 0.0f)
                progress (pct, "Separating stems...");
    }

    while (drain() > 0) {}

    const auto exitCode = process->getExitCode();
    process.reset();

    if (cancelled.load())
        return juce::Result::fail ("Separation cancelled.");

    if (exitCode != 0)
        return juce::Result::fail ("Stem separation failed (exit code " + juce::String (exitCode) + "). "
                                   + tail.getLastCharacters (400).trim());

    //--- collect what it produced --------------------------------------------
    static const std::pair<const char*, Stem> knownStems[]
    {
        { "vocals", Stem::vocals }, { "drums",  Stem::drums  }, { "bass",   Stem::bass   },
        { "piano",  Stem::piano  }, { "guitar", Stem::guitar }, { "other",  Stem::other  }
    };

    for (const auto& [name, stem] : knownStems)
    {
        // Demucs writes <out>/<model>/<track>/<stem>.wav, but wrapper scripts
        // flatten that, so search the whole tree and take the newest match.
        const auto matches = outputFolder.findChildFiles (juce::File::findFiles, true,
                                                          juce::String (name) + ".*");
        juce::File best;

        for (const auto& f : matches)
            if (isAudioFile (f)
                && (best == juce::File() || f.getLastModificationTime() > best.getLastModificationTime()))
                best = f;

        if (best != juce::File())
            out.push_back ({ stem, best });
    }

    if (out.empty())
        return juce::Result::fail ("The separator finished but left no stem files in "
                                   + outputFolder.getFullPathName());

    if (progress)
        progress (1.0f, "Separated " + juce::String ((int) out.size()) + " stems.");

    return juce::Result::ok();
}

void StemSeparator::cancel()
{
    cancelled = true;
}

} // namespace ss
