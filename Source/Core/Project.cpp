#include "Core/Project.h"

namespace ss
{
    //==============================================================================
    TempoMap::TempoMap()
    {
        events   = { { 0.0, 120.0 } };
        timeSigs = { { 0.0, 4, 4 } };
    }

    void TempoMap::setEvents (std::vector<TempoEvent> e)
    {
        if (e.empty())
            e.push_back ({ 0.0, 120.0 });

        std::sort (e.begin(), e.end(), [] (const TempoEvent& a, const TempoEvent& b) { return a.beat < b.beat; });
        e.front().beat = 0.0;          // the map must always start at zero
        events = std::move (e);
    }

    void TempoMap::setTimeSignatures (std::vector<TimeSignatureEvent> t)
    {
        if (t.empty())
            t.push_back ({ 0.0, 4, 4 });

        std::sort (t.begin(), t.end(), [] (const TimeSignatureEvent& a, const TimeSignatureEvent& b) { return a.beat < b.beat; });
        t.front().beat = 0.0;
        timeSigs = std::move (t);
    }

    double TempoMap::bpmAt (double beats) const noexcept
    {
        double bpm = events.front().bpm;

        for (const auto& e : events)
        {
            if (e.beat > beats)
                break;

            bpm = e.bpm;
        }

        return bpm;
    }

    /*  A tempo map is piecewise-constant in BPM, so seconds accumulate segment by
        segment. Linear scans are fine: tempo maps run to tens of entries, and this
        is never called from the audio thread's inner loop.                        */
    double TempoMap::beatsToSeconds (double beats) const noexcept
    {
        double seconds = 0.0, cursor = 0.0;

        for (size_t i = 0; i < events.size(); ++i)
        {
            const auto segmentEnd = (i + 1 < events.size()) ? events[i + 1].beat
                                                            : std::numeric_limits<double>::max();
            const auto segmentTop = juce::jmin (beats, segmentEnd);

            if (segmentTop > cursor)
            {
                seconds += (segmentTop - cursor) * 60.0 / events[i].bpm;
                cursor = segmentTop;
            }

            if (cursor >= beats)
                break;
        }

        return seconds;
    }

    double TempoMap::secondsToBeats (double seconds) const noexcept
    {
        double beats = 0.0, elapsed = 0.0;

        for (size_t i = 0; i < events.size(); ++i)
        {
            const auto secondsPerBeat = 60.0 / events[i].bpm;
            const auto segmentBeats = (i + 1 < events.size()) ? events[i + 1].beat - events[i].beat
                                                              : std::numeric_limits<double>::max();
            const auto segmentSeconds = segmentBeats * secondsPerBeat;

            if (elapsed + segmentSeconds >= seconds)
                return beats + (seconds - elapsed) / secondsPerBeat;

            elapsed += segmentSeconds;
            beats   += segmentBeats;
        }

        return beats;
    }

    TimeSignatureEvent TempoMap::timeSignatureAt (double beats) const noexcept
    {
        auto result = timeSigs.front();

        for (const auto& t : timeSigs)
        {
            if (t.beat > beats)
                break;

            result = t;
        }

        return result;
    }

    void TempoMap::barAndBeat (double beats, int& barOut, double& beatInBarOut) const noexcept
    {
        double cursor = 0.0;
        int bar = 1;

        for (size_t i = 0; i < timeSigs.size(); ++i)
        {
            const auto& ts = timeSigs[i];
            const auto beatsPerBar = ts.numerator * 4.0 / ts.denominator;
            const auto segmentEnd  = (i + 1 < timeSigs.size()) ? timeSigs[i + 1].beat
                                                               : std::numeric_limits<double>::max();
            const auto available = juce::jmin (beats, segmentEnd) - cursor;

            if (beats < segmentEnd || i + 1 == timeSigs.size())
            {
                const auto barsIn = std::floor (available / beatsPerBar);
                barOut = bar + (int) barsIn;
                beatInBarOut = available - barsIn * beatsPerBar;
                return;
            }

            bar += (int) std::floor (available / beatsPerBar);
            cursor = segmentEnd;
        }

        barOut = bar;
        beatInBarOut = 0.0;
    }

    //==============================================================================
    float MidiClip::lowestConfidence() const noexcept
    {
        auto lowest = 1.0f;

        for (const auto& n : notes)
            lowest = juce::jmin (lowest, n.confidence);

        return lowest;
    }

    void MidiClip::sortNotes()
    {
        std::stable_sort (notes.begin(), notes.end(), [] (const Note& a, const Note& b)
        {
            return a.startBeats != b.startBeats ? a.startBeats < b.startBeats
                                                : a.pitch < b.pitch;
        });
    }

    //==============================================================================
    Track::Track (TrackId idToUse, TrackType typeToUse, juce::String nameToUse)
        : name (std::move (nameToUse)), id (idToUse), type (typeToUse)
    {
    }

    AudioClip* Track::findAudioClip (ClipId clipId) noexcept
    {
        for (auto& c : audioClips)
            if (c.id == clipId)
                return &c;

        return nullptr;
    }

    MidiClip* Track::findMidiClip (ClipId clipId) noexcept
    {
        for (auto& c : midiClips)
            if (c.id == clipId)
                return &c;

        return nullptr;
    }

    UtauClip* Track::findUtauClip (ClipId clipId) noexcept
    {
        for (auto& c : utauClips)
            if (c.id == clipId)
                return &c;

        return nullptr;
    }

    SessionClip* Track::findSessionClip (SceneId sceneId) noexcept
    {
        auto it = sessionSlots.find (sceneId);
        return it != sessionSlots.end() ? &it->second : nullptr;
    }

    void Track::setSessionClip (SceneId sceneId, SessionClip clip)
    {
        sessionSlots[sceneId] = std::move (clip);
    }

    void Track::clearSessionClip (SceneId sceneId)
    {
        sessionSlots.erase (sceneId);
    }

    double Track::endBeats() const noexcept
    {
        double end = 0.0;

        for (const auto& c : audioClips) end = juce::jmax (end, c.endBeats());
        for (const auto& c : midiClips)  end = juce::jmax (end, c.endBeats());
        for (const auto& c : utauClips)  end = juce::jmax (end, c.endBeats());

        return end;
    }

    //==============================================================================
    Project::Project()  = default;
    Project::~Project() = default;

    Track& Project::addTrack (TrackType type, const juce::String& trackName)
    {
        static const juce::Colour palette[] = {
            juce::Colour (0xff4a90d9), juce::Colour (0xffd97a4a), juce::Colour (0xff5ec27a),
            juce::Colour (0xffc25e9e), juce::Colour (0xffc2b45e), juce::Colour (0xff7a5ec2)
        };

        auto track = std::make_unique<Track> (++lastTrackId, type, trackName);
        track->colour = palette[(size_t) (lastTrackId - 1) % (size_t) juce::numElementsInArray (palette)];

        auto& ref = *track;
        tracks.push_back (std::move (track));
        markDirty();
        return ref;
    }

    void Project::removeTrack (TrackId trackId)
    {
        const auto before = tracks.size();

        tracks.erase (std::remove_if (tracks.begin(), tracks.end(),
                                      [trackId] (const std::unique_ptr<Track>& t) { return t->getId() == trackId; }),
                      tracks.end());

        if (tracks.size() != before)
            markDirty();
    }

    void Project::moveTrack (int fromIndex, int toIndex)
    {
        if (! juce::isPositiveAndBelow (fromIndex, (int) tracks.size())
            || ! juce::isPositiveAndBelow (toIndex, (int) tracks.size())
            || fromIndex == toIndex)
            return;

        auto moved = std::move (tracks[(size_t) fromIndex]);
        tracks.erase (tracks.begin() + fromIndex);
        tracks.insert (tracks.begin() + toIndex, std::move (moved));
        markDirty();
    }

    Track* Project::findTrack (TrackId trackId) noexcept
    {
        for (auto& t : tracks)
            if (t->getId() == trackId)
                return t.get();

        return nullptr;
    }

    Bus& Project::addBus (const juce::String& busName)
    {
        Bus bus;
        bus.id   = ++lastBusId;
        bus.name = busName.isNotEmpty() ? busName : ("Bus " + juce::String (bus.id));

        buses.push_back (std::move (bus));
        markDirty();
        return buses.back();
    }

    void Project::removeBus (int busId)
    {
        const auto before = buses.size();

        buses.erase (std::remove_if (buses.begin(), buses.end(),
                                     [busId] (const Bus& b) { return b.id == busId; }),
                     buses.end());

        if (buses.size() == before)
            return;

        // Anything that fed the bus now feeds the master, and its send is gone -
        // a route pointing at a bus that no longer exists would silently vanish.
        for (auto& t : tracks)
        {
            if (t->outputBus == busId)
                t->outputBus = 0;

            t->sends.erase (std::remove_if (t->sends.begin(), t->sends.end(),
                                            [busId] (const Track::Send& s) { return s.busId == busId; }),
                            t->sends.end());
        }

        markDirty();
    }

    Bus* Project::findBus (int busId) noexcept
    {
        for (auto& b : buses)
            if (b.id == busId)
                return &b;

        return nullptr;
    }

    Scene& Project::addScene (const juce::String& sceneName)
    {
        Scene scene;
        scene.id   = nextSceneId();
        scene.name = sceneName.isNotEmpty() ? sceneName : ("Scene " + juce::String (scene.id));

        scenes.push_back (std::move (scene));
        markDirty();
        return scenes.back();
    }

    void Project::removeScene (SceneId sceneId)
    {
        const auto before = scenes.size();

        scenes.erase (std::remove_if (scenes.begin(), scenes.end(),
                                      [sceneId] (const Scene& s) { return s.id == sceneId; }),
                      scenes.end());

        if (scenes.size() == before)
            return;

        for (auto& t : tracks)
            t->clearSessionClip (sceneId);

        markDirty();
    }

    Scene* Project::findScene (SceneId sceneId) noexcept
    {
        for (auto& s : scenes)
            if (s.id == sceneId)
                return &s;

        return nullptr;
    }

    bool Project::anyTrackSoloed() const noexcept
    {
        for (const auto& t : tracks)
            if (t->soloed)
                return true;

        return false;
    }

    double Project::endBeats() const noexcept
    {
        double end = 0.0;

        for (const auto& t : tracks)
            end = juce::jmax (end, t->endBeats());

        return end;
    }

    void Project::markDirty()
    {
        dirty = true;
        sendChangeMessage();
    }

    juce::File Project::getMediaFolder() const
    {
        auto folder = file.existsAsFile()
                        ? file.getParentDirectory().getChildFile ("Media")
                        : juce::File::getSpecialLocation (juce::File::tempDirectory)
                              .getChildFile ("ScoreSmith").getChildFile (juce::File::createLegalFileName (name));

        folder.createDirectory();
        return folder;
    }
}
