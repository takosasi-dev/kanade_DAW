#pragma once
#include "Core/Project.h"
#include "Plugins/PluginManager.h"
#include <juce_core/juce_core.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <map>

namespace ss::io::dawproject
{
    /** Thin wrapper over juce::ZipFile::Builder so the export passes in
        later tasks (structure, clips, automation, plugin state, scenes)
        can each add their own entries independently, then write once. */
    class ArchiveWriter
    {
    public:
        void addXml (const juce::String& storedPathName, const juce::XmlElement& xml)
        {
            const auto text = xml.toString();
            addBytes (storedPathName, juce::MemoryBlock (text.toUTF8().getAddress(), text.getNumBytesAsUTF8()));
        }

        void addFile (const juce::String& storedPathName, const juce::File& sourceFile)
        {
            builder.addFile (sourceFile, 6, storedPathName);
        }

        void addBytes (const juce::String& storedPathName, const juce::MemoryBlock& data)
        {
            builder.addEntry (new juce::MemoryInputStream (data, true), 6,
                              storedPathName, juce::Time::getCurrentTime());
        }

        bool writeTo (const juce::File& target, juce::String& errorOut)
        {
            // FileOutputStream opens with OPEN_ALWAYS and seeks to the end - without this
            // a re-export would append a second archive onto the first, corrupting both.
            target.deleteFile();
            juce::FileOutputStream out (target);
            if (! out.openedOk())
            {
                errorOut = "Could not open \"" + target.getFullPathName() + "\" for writing.";
                return false;
            }
            if (! builder.writeToStream (out, nullptr))
            {
                errorOut = "Failed to write the DAWproject archive.";
                return false;
            }
            return true;
        }

    private:
        juce::ZipFile::Builder builder;
    };

    /** Thin wrapper over juce::ZipFile for reading a .dawproject archive
        written by ArchiveWriter (or by another DAW). */
    class ArchiveReader
    {
    public:
        explicit ArchiveReader (const juce::File& source) : zip (source) {}

        std::unique_ptr<juce::XmlElement> readProjectXml (juce::String& errorOut) const
        {
            const auto index = zip.getIndexOfFileName ("project.xml");
            if (index < 0)
            {
                errorOut = "This file has no project.xml entry - it is not a valid .dawproject.";
                return nullptr;
            }

            std::unique_ptr<juce::InputStream> stream (zip.createStreamForEntry (index));
            if (stream == nullptr)
            {
                errorOut = "Could not read the project.xml entry from the archive.";
                return nullptr;
            }

            auto xml = juce::parseXML (stream->readEntireStreamAsString());
            if (xml == nullptr)
            {
                errorOut = "project.xml did not parse as valid XML.";
                return nullptr;
            }
            return xml;
        }

        bool readEntry (const juce::String& entryPath, juce::MemoryBlock& dataOut) const
        {
            const auto index = zip.getIndexOfFileName (entryPath);
            if (index < 0)
                return false;

            std::unique_ptr<juce::InputStream> stream (zip.createStreamForEntry (index));
            if (stream == nullptr)
                return false;

            dataOut.reset();
            stream->readIntoMemoryBlock (dataOut);
            return true;
        }

        bool extractEntryToFile (const juce::String& entryPath, const juce::File& targetFile) const
        {
            const auto index = zip.getIndexOfFileName (entryPath);
            if (index < 0)
                return false;

            std::unique_ptr<juce::InputStream> stream (zip.createStreamForEntry (index));
            if (stream == nullptr)
                return false;

            targetFile.getParentDirectory().createDirectory();
            targetFile.deleteFile();   // see ArchiveWriter::writeTo - streams append, they don't truncate
            juce::FileOutputStream out (targetFile);
            if (! out.openedOk())
                return false;

            out.writeFromInputStream (*stream, -1);
            return true;
        }

    private:
        mutable juce::ZipFile zip;
    };

    juce::String trackXmlId      (TrackId id);
    juce::String channelXmlId    (TrackId id);
    juce::String busTrackXmlId   (int busId);
    juce::String busChannelXmlId (int busId);
    extern const juce::String masterTrackXmlId;
    extern const juce::String masterChannelXmlId;

    struct ImportIds
    {
        std::map<juce::String, TrackId> trackByXmlId;
        std::map<juce::String, int>     busIdByChannelXmlId;
        juce::String                    masterChannelXmlId;
    };

    ImportIds parseStructure (const juce::XmlElement& root, Project& project, juce::StringArray& warningsOut);

    void parseClips (const juce::XmlElement& root, Project& project, const ImportIds& ids,
                      ArchiveReader& archive, juce::StringArray& warningsOut);

    void parseDevicesAndAutomation (const juce::XmlElement& root, Project& project, PluginManager& plugins,
                                     const ImportIds& ids, ArchiveReader& archive, juce::StringArray& warningsOut);

    void parseScenes (const juce::XmlElement& root, Project& project, const ImportIds& ids,
                       ArchiveReader& archive, juce::StringArray& warningsOut);

    std::unique_ptr<juce::XmlElement> buildProjectSkeleton (const Project& project,
                                                             juce::StringArray& warningsOut);

    void addClips (juce::XmlElement& lanes, const Project& project, juce::AudioFormatManager&,
                    ArchiveWriter& writer, juce::StringArray& warningsOut);

    void addDevicesAndAutomation (juce::XmlElement& root, const Project& project, PluginManager& plugins,
                                   ArchiveWriter& writer, juce::StringArray& warningsOut);

    void addScenes (juce::XmlElement& root, const Project& project, juce::AudioFormatManager&,
                     ArchiveWriter& writer, juce::StringArray& warningsOut);
}

namespace ss::io
{
    bool exportDawProject (const juce::File& target, const Project& project, PluginManager& plugins,
                            juce::String& errorOut, juce::StringArray& warningsOut);

    bool importDawProject (const juce::File& source, Project& project, PluginManager& plugins,
                            juce::String& errorOut, juce::StringArray& warningsOut);
}
