#include "Extensions/FormatExtension.h"
#include "Extensions/FormatExtensionManager.h"
#include "Extensions/FormatExtensionRunner.h"
#include "Core/Project.h"
#include "Plugins/PluginManager.h"
#include "Core/Settings.h"
#include "IO/DawProject.h"

namespace ss
{

class FormatExtensionUnitTests final : public juce::UnitTest
{
public:
    FormatExtensionUnitTests() : juce::UnitTest ("ScoreSmith Extensions", "ScoreSmith") {}

    void runTest() override
    {
        const auto tempRoot = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                   .getChildFile ("ScoreSmithFormatExtensionTest");
        tempRoot.deleteRecursively();
        tempRoot.createDirectory();

        beginTest ("parseFormatExtensionManifest parses a valid manifest");
        {
            auto folder = tempRoot.getChildFile ("valid");
            folder.createDirectory();
            folder.getChildFile ("tool.exe").replaceWithText ("");
            folder.getChildFile ("manifest.json").replaceWithText (R"JSON({
                "id": "com.example.reason-export",
                "name": "Reason Project Export",
                "version": "1.0.0",
                "fileExtension": "reason",
                "direction": "export",
                "executable": "tool.exe"
            })JSON");

            FormatExtension ext;
            juce::String warning;
            const bool ok = parseFormatExtensionManifest (folder, ext, warning);

            expect (ok, warning);
            expectEquals (ext.id, juce::String ("com.example.reason-export"));
            expectEquals (ext.name, juce::String ("Reason Project Export"));
            expectEquals (ext.version, juce::String ("1.0.0"));
            expectEquals (ext.fileExtension, juce::String ("reason"));
            expect (ext.direction == ExtensionDirection::exportOnly);
            expectEquals (ext.executable.getFullPathName(), folder.getChildFile ("tool.exe").getFullPathName());
            expectEquals (ext.folder.getFullPathName(), folder.getFullPathName());
        }

        beginTest ("parseFormatExtensionManifest defaults direction to both and version to empty");
        {
            auto folder = tempRoot.getChildFile ("defaults");
            folder.createDirectory();
            folder.getChildFile ("tool.exe").replaceWithText ("");
            folder.getChildFile ("manifest.json").replaceWithText (R"JSON({
                "id": "com.example.minimal",
                "name": "Minimal",
                "fileExtension": "xyz",
                "executable": "tool.exe"
            })JSON");

            FormatExtension ext;
            juce::String warning;
            expect (parseFormatExtensionManifest (folder, ext, warning), warning);
            expect (ext.direction == ExtensionDirection::both);
            expectEquals (ext.version, juce::String());
        }

        beginTest ("parseFormatExtensionManifest fails with a warning when manifest.json is missing");
        {
            auto folder = tempRoot.getChildFile ("no-manifest");
            folder.createDirectory();

            FormatExtension ext;
            juce::String warning;
            expect (! parseFormatExtensionManifest (folder, ext, warning));
            expect (warning.isNotEmpty());
        }

        beginTest ("parseFormatExtensionManifest fails with a warning on invalid JSON");
        {
            auto folder = tempRoot.getChildFile ("bad-json");
            folder.createDirectory();
            folder.getChildFile ("manifest.json").replaceWithText ("not json at all {{{");

            FormatExtension ext;
            juce::String warning;
            expect (! parseFormatExtensionManifest (folder, ext, warning));
            expect (warning.isNotEmpty());
        }

        beginTest ("parseFormatExtensionManifest fails with a warning when a required field is missing");
        {
            auto folder = tempRoot.getChildFile ("missing-field");
            folder.createDirectory();
            folder.getChildFile ("tool.exe").replaceWithText ("");
            folder.getChildFile ("manifest.json").replaceWithText (R"JSON({
                "name": "No Id",
                "fileExtension": "xyz",
                "executable": "tool.exe"
            })JSON");

            FormatExtension ext;
            juce::String warning;
            expect (! parseFormatExtensionManifest (folder, ext, warning));
            expect (warning.isNotEmpty());
        }

        beginTest ("parseFormatExtensionManifest fails with a warning when the executable does not exist");
        {
            auto folder = tempRoot.getChildFile ("missing-exe");
            folder.createDirectory();
            folder.getChildFile ("manifest.json").replaceWithText (R"JSON({
                "id": "com.example.ghost",
                "name": "Ghost",
                "fileExtension": "xyz",
                "executable": "does-not-exist.exe"
            })JSON");

            FormatExtension ext;
            juce::String warning;
            expect (! parseFormatExtensionManifest (folder, ext, warning));
            expect (warning.isNotEmpty());
        }

        beginTest ("parseFormatExtensionManifest fails with a warning when the executable path escapes the folder");
        {
            // Create a real file outside the extension folder to prove containment check works
            auto outsideFolder = tempRoot.getChildFile ("escape-target");
            outsideFolder.createDirectory();
            outsideFolder.getChildFile ("tool.exe").replaceWithText ("");

            auto folder = tempRoot.getChildFile ("escape-attempt");
            folder.createDirectory();
            folder.getChildFile ("manifest.json").replaceWithText (R"JSON({
                "id": "com.example.escape",
                "name": "Escape Test",
                "fileExtension": "xyz",
                "executable": "../escape-target/tool.exe"
            })JSON");

            FormatExtension ext;
            juce::String warning;
            expect (! parseFormatExtensionManifest (folder, ext, warning));
            expect (warning.isNotEmpty());
        }

        beginTest ("FormatExtensionManager::rescan discovers valid extensions across multiple scan paths");
        {
            auto scanRootA = tempRoot.getChildFile ("scanA");
            auto scanRootB = tempRoot.getChildFile ("scanB");
            scanRootA.createDirectory();
            scanRootB.createDirectory();

            auto extA = scanRootA.getChildFile ("ext-a");
            extA.createDirectory();
            extA.getChildFile ("tool.exe").replaceWithText ("");
            extA.getChildFile ("manifest.json").replaceWithText (R"JSON({
                "id": "com.example.a", "name": "Ext A", "fileExtension": "aaa",
                "direction": "export", "executable": "tool.exe"
            })JSON");

            auto extB = scanRootB.getChildFile ("ext-b");
            extB.createDirectory();
            extB.getChildFile ("tool.exe").replaceWithText ("");
            extB.getChildFile ("manifest.json").replaceWithText (R"JSON({
                "id": "com.example.b", "name": "Ext B", "fileExtension": "bbb",
                "direction": "import", "executable": "tool.exe"
            })JSON");

            // A sibling folder with no manifest.json at all - not an extension,
            // must not produce a warning.
            scanRootA.getChildFile ("just-a-folder").createDirectory();

            // A sibling folder WITH a manifest.json that's broken - must warn
            // but not block discovering ext-a/ext-b.
            auto broken = scanRootA.getChildFile ("broken");
            broken.createDirectory();
            broken.getChildFile ("manifest.json").replaceWithText ("not json {{{");

            FormatExtensionManager manager;
            juce::StringArray warnings;
            manager.rescan ({ scanRootA.getFullPathName(), scanRootB.getFullPathName() }, warnings);

            expectEquals ((int) manager.getExtensions().size(), 2);
            expectEquals (warnings.size(), 1);

            const auto exporters = manager.matching (ExtensionDirection::exportOnly);
            expectEquals ((int) exporters.size(), 1);
            expectEquals (exporters.front()->id, juce::String ("com.example.a"));

            const auto importers = manager.matching (ExtensionDirection::importOnly);
            expectEquals ((int) importers.size(), 1);
            expectEquals (importers.front()->id, juce::String ("com.example.b"));
        }

        beginTest ("FormatExtensionManager::matching treats direction \"both\" as matching either query");
        {
            auto scanRoot = tempRoot.getChildFile ("scanBoth");
            scanRoot.createDirectory();
            auto ext = scanRoot.getChildFile ("ext-both");
            ext.createDirectory();
            ext.getChildFile ("tool.exe").replaceWithText ("");
            ext.getChildFile ("manifest.json").replaceWithText (R"JSON({
                "id": "com.example.both", "name": "Ext Both", "fileExtension": "ccc",
                "executable": "tool.exe"
            })JSON");

            FormatExtensionManager manager;
            juce::StringArray warnings;
            manager.rescan ({ scanRoot.getFullPathName() }, warnings);

            expectEquals ((int) manager.matching (ExtensionDirection::exportOnly).size(), 1);
            expectEquals ((int) manager.matching (ExtensionDirection::importOnly).size(), 1);
        }

        beginTest ("FormatExtensionManager::matching(both) returns every extension unfiltered, regardless of each one's own direction");
        {
            auto scanRoot = tempRoot.getChildFile ("scanMixedDirections");
            scanRoot.createDirectory();

            auto extImport = scanRoot.getChildFile ("ext-import");
            extImport.createDirectory();
            extImport.getChildFile ("tool.exe").replaceWithText ("");
            extImport.getChildFile ("manifest.json").replaceWithText (R"JSON({
                "id": "com.example.mixed-import", "name": "Mixed Import", "fileExtension": "mia",
                "direction": "import", "executable": "tool.exe"
            })JSON");

            auto extExport = scanRoot.getChildFile ("ext-export");
            extExport.createDirectory();
            extExport.getChildFile ("tool.exe").replaceWithText ("");
            extExport.getChildFile ("manifest.json").replaceWithText (R"JSON({
                "id": "com.example.mixed-export", "name": "Mixed Export", "fileExtension": "mie",
                "direction": "export", "executable": "tool.exe"
            })JSON");

            auto extBoth = scanRoot.getChildFile ("ext-both-mixed");
            extBoth.createDirectory();
            extBoth.getChildFile ("tool.exe").replaceWithText ("");
            extBoth.getChildFile ("manifest.json").replaceWithText (R"JSON({
                "id": "com.example.mixed-both", "name": "Mixed Both", "fileExtension": "mib",
                "executable": "tool.exe"
            })JSON");

            FormatExtensionManager manager;
            juce::StringArray warnings;
            manager.rescan ({ scanRoot.getFullPathName() }, warnings);
            expectEquals ((int) manager.getExtensions().size(), 3);

            // matching(both) must return ALL three - including the importOnly
            // and exportOnly ones, not just the one whose own direction is
            // literally "both".
            const auto all = manager.matching (ExtensionDirection::both);
            expectEquals ((int) all.size(), 3);
        }

        beginTest ("FormatExtensionManager::rescan with an empty scan-path list is a no-op");
        {
            FormatExtensionManager manager;
            juce::StringArray warnings;
            manager.rescan ({}, warnings);
            expect (manager.getExtensions().empty());
            expect (warnings.isEmpty());
        }

        beginTest ("FormatExtensionManager::rescan replaces the previous discovery list wholesale");
        {
            auto scanRoot = tempRoot.getChildFile ("scanReplace");
            scanRoot.createDirectory();
            auto ext = scanRoot.getChildFile ("ext-one");
            ext.createDirectory();
            ext.getChildFile ("tool.exe").replaceWithText ("");
            ext.getChildFile ("manifest.json").replaceWithText (R"JSON({
                "id": "com.example.one", "name": "One", "fileExtension": "one",
                "executable": "tool.exe"
            })JSON");

            FormatExtensionManager manager;
            juce::StringArray warnings;
            manager.rescan ({ scanRoot.getFullPathName() }, warnings);
            expectEquals ((int) manager.getExtensions().size(), 1);

            // Rescanning an empty list must clear the previous result, not
            // append to it.
            manager.rescan ({}, warnings);
            expect (manager.getExtensions().empty());
        }

        beginTest ("runFormatExtensionExport writes a temp .dawproject, invokes the runner, and cleans up");
        {
            struct RecordingRunner final : public FormatExtensionRunner
            {
                juce::StringArray lastArgs;
                juce::File lastExpectedOutput;

                bool run (const juce::StringArray& args, const juce::File& expectedOutput,
                          juce::String&) override
                {
                    lastArgs = args;
                    lastExpectedOutput = expectedOutput;
                    // Simulate a well-behaved extension: create the requested output file.
                    expectedOutput.create();
                    return true;
                }
            } runner;

            Settings settings;
            PluginManager plugins (settings);
            Project project;
            project.addTrack (TrackType::audio, "Audio 1");

            FormatExtension ext;
            ext.id = "com.example.test";
            ext.name = "Test";
            ext.fileExtension = "test";
            ext.direction = ExtensionDirection::exportOnly;
            ext.executable = juce::File ("C:/fake/tool.exe");

            const auto outputFile = tempRoot.getChildFile ("export-out.test");
            outputFile.deleteFile();

            juce::String error;
            juce::StringArray warnings;
            const bool ok = io::runFormatExtensionExport (ext, project, plugins, outputFile, runner, error, warnings);

            expect (ok, error);
            expectEquals (runner.lastArgs.size(), 4);
            expectEquals (runner.lastArgs[0], ext.executable.getFullPathName());
            expectEquals (runner.lastArgs[1], juce::String ("--export"));
            expect (runner.lastArgs[2].endsWithIgnoreCase (".dawproject"));
            expectEquals (runner.lastArgs[3], outputFile.getFullPathName());
            expectEquals (runner.lastExpectedOutput.getFullPathName(), outputFile.getFullPathName());

            // The intermediate .dawproject was a temp file and must not be left behind.
            expect (! juce::File (runner.lastArgs[2]).existsAsFile());
        }

        beginTest ("runFormatExtensionExport fails and returns the runner's error when the runner reports failure");
        {
            struct FailingRunner final : public FormatExtensionRunner
            {
                bool run (const juce::StringArray&, const juce::File&, juce::String& errorOut) override
                {
                    errorOut = "extension exploded";
                    return false;
                }
            } runner;

            Settings settings;
            PluginManager plugins (settings);
            Project project;

            FormatExtension ext;
            ext.id = "com.example.test";
            ext.name = "Test";
            ext.fileExtension = "test";
            ext.executable = juce::File ("C:/fake/tool.exe");

            juce::String error;
            juce::StringArray warnings;
            const bool ok = io::runFormatExtensionExport (ext, project, plugins,
                                                       tempRoot.getChildFile ("wont-be-made.test"),
                                                       runner, error, warnings);

            expect (! ok);
            expectEquals (error, juce::String ("extension exploded"));
        }

        beginTest ("runFormatExtensionImport invokes the runner then imports the produced .dawproject");
        {
            // First, build a real .dawproject to hand back as "what the
            // extension produced" - runFormatExtensionImport must import it
            // for real, not just report success.
            Settings settings;
            PluginManager plugins (settings);
            Project sourceProject;
            sourceProject.addTrack (TrackType::audio, "Imported Track");
            const auto realDawProject = tempRoot.getChildFile ("real.dawproject");
            juce::String setupError;
            juce::StringArray setupWarnings;
            expect (io::exportDawProject (realDawProject, sourceProject, plugins, setupError, setupWarnings),
                    setupError);

            struct ProducesFileRunner final : public FormatExtensionRunner
            {
                juce::File sourceToServe;
                bool run (const juce::StringArray&, const juce::File& expectedOutput,
                          juce::String&) override
                {
                    return sourceToServe.copyFileTo (expectedOutput);
                }
            } runner;
            runner.sourceToServe = realDawProject;

            FormatExtension ext;
            ext.id = "com.example.test";
            ext.name = "Test";
            ext.fileExtension = "test";
            ext.direction = ExtensionDirection::importOnly;
            ext.executable = juce::File ("C:/fake/tool.exe");

            Project targetProject;
            juce::String error;
            juce::StringArray warnings;
            const bool ok = io::runFormatExtensionImport (ext, targetProject, plugins,
                                                       tempRoot.getChildFile ("input.test"),
                                                       runner, error, warnings);

            expect (ok, error);
            expect (targetProject.getTracks().size() >= 1);
            bool foundImportedTrack = false;
            for (const auto& t : targetProject.getTracks())
                if (t->name == "Imported Track")
                    foundImportedTrack = true;
            expect (foundImportedTrack, "the imported project should contain the track from the fake extension's .dawproject");
        }

        beginTest ("runFormatExtensionImport fails when the runner cannot produce a valid .dawproject");
        {
            struct MissingOutputRunner final : public FormatExtensionRunner
            {
                bool run (const juce::StringArray&, const juce::File&, juce::String& errorOut) override
                {
                    errorOut = "did not run";
                    return false;
                }
            } runner;

            Settings settings;
            PluginManager plugins (settings);
            Project project;

            FormatExtension ext;
            ext.id = "com.example.test";
            ext.name = "Test";
            ext.fileExtension = "test";
            ext.executable = juce::File ("C:/fake/tool.exe");

            juce::String error;
            juce::StringArray warnings;
            const bool ok = io::runFormatExtensionImport (ext, project, plugins,
                                                       tempRoot.getChildFile ("bad-input.test"),
                                                       runner, error, warnings);
            expect (! ok);
            expectEquals (error, juce::String ("did not run"));
        }

        tempRoot.deleteRecursively();
    }
};

static FormatExtensionUnitTests formatExtensionUnitTests;

}
