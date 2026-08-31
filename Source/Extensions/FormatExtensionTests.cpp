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

        beginTest ("parseFormatExtensionManifest parses additionalInputs of both kinds");
        {
            auto folder = tempRoot.getChildFile ("additional-inputs");
            folder.createDirectory();
            folder.getChildFile ("tool.exe").replaceWithText ("");
            folder.getChildFile ("manifest.json").replaceWithText (R"JSON({
                "id": "com.example.video-mux",
                "name": "Video Mux",
                "fileExtension": "mp4",
                "direction": "export",
                "executable": "tool.exe",
                "additionalInputs": [
                    {
                        "kind": "userFile",
                        "envVar": "KANADE_DAW_VIDEO_FILE",
                        "prompt": "Video file to combine with",
                        "fileFilter": "*.mp4"
                    },
                    {
                        "kind": "mixdownRender",
                        "envVar": "KANADE_DAW_MIXDOWN_WAV"
                    }
                ]
            })JSON");

            FormatExtension ext;
            juce::String warning;
            expect (parseFormatExtensionManifest (folder, ext, warning), warning);
            expectEquals ((int) ext.additionalInputs.size(), 2);

            expect (ext.additionalInputs[0].kind == AdditionalInputKind::userFile);
            expectEquals (ext.additionalInputs[0].envVar, juce::String ("KANADE_DAW_VIDEO_FILE"));
            expectEquals (ext.additionalInputs[0].prompt, juce::String ("Video file to combine with"));
            expectEquals (ext.additionalInputs[0].fileFilter, juce::String ("*.mp4"));

            expect (ext.additionalInputs[1].kind == AdditionalInputKind::mixdownRender);
            expectEquals (ext.additionalInputs[1].envVar, juce::String ("KANADE_DAW_MIXDOWN_WAV"));
        }

        beginTest ("parseFormatExtensionManifest defaults additionalInputs to empty when absent");
        {
            auto folder = tempRoot.getChildFile ("no-additional-inputs");
            folder.createDirectory();
            folder.getChildFile ("tool.exe").replaceWithText ("");
            folder.getChildFile ("manifest.json").replaceWithText (R"JSON({
                "id": "com.example.plain",
                "name": "Plain",
                "fileExtension": "xyz",
                "executable": "tool.exe"
            })JSON");

            FormatExtension ext;
            juce::String warning;
            expect (parseFormatExtensionManifest (folder, ext, warning), warning);
            expect (ext.additionalInputs.empty());
        }

        beginTest ("parseFormatExtensionManifest rejects an additionalInputs entry with an unknown kind");
        {
            auto folder = tempRoot.getChildFile ("bad-kind");
            folder.createDirectory();
            folder.getChildFile ("tool.exe").replaceWithText ("");
            folder.getChildFile ("manifest.json").replaceWithText (R"JSON({
                "id": "com.example.bad-kind",
                "name": "Bad Kind",
                "fileExtension": "xyz",
                "executable": "tool.exe",
                "additionalInputs": [
                    { "kind": "somethingElse", "envVar": "X" }
                ]
            })JSON");

            FormatExtension ext;
            juce::String warning;
            expect (! parseFormatExtensionManifest (folder, ext, warning));
            expect (warning.isNotEmpty());
        }

        beginTest ("parseFormatExtensionManifest rejects an additionalInputs entry missing envVar");
        {
            auto folder = tempRoot.getChildFile ("missing-envvar");
            folder.createDirectory();
            folder.getChildFile ("tool.exe").replaceWithText ("");
            folder.getChildFile ("manifest.json").replaceWithText (R"JSON({
                "id": "com.example.missing-envvar",
                "name": "Missing EnvVar",
                "fileExtension": "xyz",
                "executable": "tool.exe",
                "additionalInputs": [
                    { "kind": "mixdownRender" }
                ]
            })JSON");

            FormatExtension ext;
            juce::String warning;
            expect (! parseFormatExtensionManifest (folder, ext, warning));
            expect (warning.isNotEmpty());
        }

        beginTest ("parseFormatExtensionManifest parses settings of all three types");
        {
            auto folder = tempRoot.getChildFile ("settings-all-types");
            folder.createDirectory();
            folder.getChildFile ("tool.exe").replaceWithText ("");
            folder.getChildFile ("manifest.json").replaceWithText (R"JSON({
                "id": "com.example.settings",
                "name": "Settings Test",
                "fileExtension": "xyz",
                "executable": "tool.exe",
                "settings": [
                    { "id": "balance", "label": "Balance", "type": "slider",
                      "envVar": "BALANCE", "min": 0.0, "max": 1.0, "default": 0.5 },
                    { "id": "matchLength", "label": "Match length", "type": "checkbox",
                      "envVar": "MATCH_LENGTH", "default": true },
                    { "id": "codec", "label": "Codec", "type": "dropdown",
                      "envVar": "CODEC", "options": ["aac", "mp3"], "default": "aac" }
                ]
            })JSON");

            FormatExtension ext;
            juce::String warning;
            expect (parseFormatExtensionManifest (folder, ext, warning), warning);
            expectEquals ((int) ext.settings.size(), 3);

            expect (ext.settings[0].type == ExtensionSettingType::slider);
            expectEquals (ext.settings[0].envVar, juce::String ("BALANCE"));
            expectWithinAbsoluteError (ext.settings[0].sliderMin, 0.0, 0.0001);
            expectWithinAbsoluteError (ext.settings[0].sliderMax, 1.0, 0.0001);
            expectWithinAbsoluteError (ext.settings[0].sliderDefault, 0.5, 0.0001);

            expect (ext.settings[1].type == ExtensionSettingType::checkbox);
            expectEquals (ext.settings[1].envVar, juce::String ("MATCH_LENGTH"));
            expect (ext.settings[1].checkboxDefault);

            expect (ext.settings[2].type == ExtensionSettingType::dropdown);
            expectEquals (ext.settings[2].envVar, juce::String ("CODEC"));
            expectEquals (ext.settings[2].dropdownOptions.size(), 2);
            expectEquals (ext.settings[2].dropdownOptions[0], juce::String ("aac"));
            expectEquals (ext.settings[2].dropdownDefault, juce::String ("aac"));
        }

        beginTest ("parseFormatExtensionManifest defaults settings to empty and customUI to false when absent");
        {
            auto folder = tempRoot.getChildFile ("no-settings");
            folder.createDirectory();
            folder.getChildFile ("tool.exe").replaceWithText ("");
            folder.getChildFile ("manifest.json").replaceWithText (R"JSON({
                "id": "com.example.plain2",
                "name": "Plain2",
                "fileExtension": "xyz",
                "executable": "tool.exe"
            })JSON");

            FormatExtension ext;
            juce::String warning;
            expect (parseFormatExtensionManifest (folder, ext, warning), warning);
            expect (ext.settings.empty());
            expect (! ext.customUI);
        }

        beginTest ("parseFormatExtensionManifest parses customUI: true");
        {
            auto folder = tempRoot.getChildFile ("custom-ui");
            folder.createDirectory();
            folder.getChildFile ("tool.exe").replaceWithText ("");
            folder.getChildFile ("manifest.json").replaceWithText (R"JSON({
                "id": "com.example.customui",
                "name": "Custom UI",
                "fileExtension": "xyz",
                "executable": "tool.exe",
                "customUI": true
            })JSON");

            FormatExtension ext;
            juce::String warning;
            expect (parseFormatExtensionManifest (folder, ext, warning), warning);
            expect (ext.customUI);
            expect (ext.settings.empty());
        }

        beginTest ("parseFormatExtensionManifest rejects customUI: true combined with a non-empty settings array");
        {
            auto folder = tempRoot.getChildFile ("custom-ui-and-settings");
            folder.createDirectory();
            folder.getChildFile ("tool.exe").replaceWithText ("");
            folder.getChildFile ("manifest.json").replaceWithText (R"JSON({
                "id": "com.example.conflict",
                "name": "Conflict",
                "fileExtension": "xyz",
                "executable": "tool.exe",
                "customUI": true,
                "settings": [
                    { "id": "x", "label": "X", "type": "checkbox", "envVar": "X", "default": false }
                ]
            })JSON");

            FormatExtension ext;
            juce::String warning;
            expect (! parseFormatExtensionManifest (folder, ext, warning));
            expect (warning.isNotEmpty());
        }

        beginTest ("parseFormatExtensionManifest rejects a settings entry with an unknown type");
        {
            auto folder = tempRoot.getChildFile ("settings-bad-type");
            folder.createDirectory();
            folder.getChildFile ("tool.exe").replaceWithText ("");
            folder.getChildFile ("manifest.json").replaceWithText (R"JSON({
                "id": "com.example.badtype",
                "name": "Bad Type",
                "fileExtension": "xyz",
                "executable": "tool.exe",
                "settings": [
                    { "id": "x", "label": "X", "type": "colourPicker", "envVar": "X" }
                ]
            })JSON");

            FormatExtension ext;
            juce::String warning;
            expect (! parseFormatExtensionManifest (folder, ext, warning));
            expect (warning.isNotEmpty());
        }

        beginTest ("parseFormatExtensionManifest rejects a slider settings entry missing min/max/default");
        {
            auto folder = tempRoot.getChildFile ("settings-slider-incomplete");
            folder.createDirectory();
            folder.getChildFile ("tool.exe").replaceWithText ("");
            folder.getChildFile ("manifest.json").replaceWithText (R"JSON({
                "id": "com.example.incomplete",
                "name": "Incomplete",
                "fileExtension": "xyz",
                "executable": "tool.exe",
                "settings": [
                    { "id": "x", "label": "X", "type": "slider", "envVar": "X", "min": 0.0 }
                ]
            })JSON");

            FormatExtension ext;
            juce::String warning;
            expect (! parseFormatExtensionManifest (folder, ext, warning));
            expect (warning.isNotEmpty());
        }

        beginTest ("parseFormatExtensionManifest rejects a dropdown settings entry whose default is not one of its options");
        {
            auto folder = tempRoot.getChildFile ("settings-dropdown-bad-default");
            folder.createDirectory();
            folder.getChildFile ("tool.exe").replaceWithText ("");
            folder.getChildFile ("manifest.json").replaceWithText (R"JSON({
                "id": "com.example.baddefault",
                "name": "Bad Default",
                "fileExtension": "xyz",
                "executable": "tool.exe",
                "settings": [
                    { "id": "x", "label": "X", "type": "dropdown", "envVar": "X",
                      "options": ["a", "b"], "default": "c" }
                ]
            })JSON");

            FormatExtension ext;
            juce::String warning;
            expect (! parseFormatExtensionManifest (folder, ext, warning));
            expect (warning.isNotEmpty());
        }

        beginTest ("parseFormatExtensionManifest rejects settings entries with duplicate ids");
        {
            auto folder = tempRoot.getChildFile ("settings-duplicate-id");
            folder.createDirectory();
            folder.getChildFile ("tool.exe").replaceWithText ("");
            folder.getChildFile ("manifest.json").replaceWithText (R"JSON({
                "id": "com.example.dupid",
                "name": "Dup Id",
                "fileExtension": "xyz",
                "executable": "tool.exe",
                "settings": [
                    { "id": "x", "label": "X1", "type": "checkbox", "envVar": "X1", "default": false },
                    { "id": "x", "label": "X2", "type": "checkbox", "envVar": "X2", "default": false }
                ]
            })JSON");

            FormatExtension ext;
            juce::String warning;
            expect (! parseFormatExtensionManifest (folder, ext, warning));
            expect (warning.isNotEmpty());
        }

        beginTest ("parseFormatExtensionManifest rejects two settings entries sharing an envVar");
        {
            auto folder = tempRoot.getChildFile ("settings-duplicate-envvar");
            folder.createDirectory();
            folder.getChildFile ("tool.exe").replaceWithText ("");
            folder.getChildFile ("manifest.json").replaceWithText (R"JSON({
                "id": "com.example.dupenv",
                "name": "Dup EnvVar",
                "fileExtension": "xyz",
                "executable": "tool.exe",
                "settings": [
                    { "id": "a", "label": "A", "type": "checkbox", "envVar": "SHARED", "default": false },
                    { "id": "b", "label": "B", "type": "checkbox", "envVar": "SHARED", "default": false }
                ]
            })JSON");

            FormatExtension ext;
            juce::String warning;
            expect (! parseFormatExtensionManifest (folder, ext, warning));
            expect (warning.isNotEmpty());
        }

        beginTest ("parseFormatExtensionManifest rejects a settings entry reusing an additionalInputs envVar");
        {
            auto folder = tempRoot.getChildFile ("settings-vs-inputs-envvar");
            folder.createDirectory();
            folder.getChildFile ("tool.exe").replaceWithText ("");
            folder.getChildFile ("manifest.json").replaceWithText (R"JSON({
                "id": "com.example.crossenv",
                "name": "Cross EnvVar",
                "fileExtension": "xyz",
                "executable": "tool.exe",
                "additionalInputs": [
                    { "kind": "mixdownRender", "envVar": "SHARED" }
                ],
                "settings": [
                    { "id": "a", "label": "A", "type": "checkbox", "envVar": "SHARED", "default": false }
                ]
            })JSON");

            FormatExtension ext;
            juce::String warning;
            expect (! parseFormatExtensionManifest (folder, ext, warning));
            expect (warning.isNotEmpty());
        }

        beginTest ("parseProgressLine recognises a well-formed PROGRESS: line");
        {
            float value = -1.0f;
            expect (parseProgressLine ("PROGRESS:42", value));
            expectWithinAbsoluteError (value, 0.42f, 0.0001f);
        }

        beginTest ("parseProgressLine clamps a value above 100 to 1.0");
        {
            float value = -1.0f;
            expect (parseProgressLine ("PROGRESS:150", value));
            expectWithinAbsoluteError (value, 1.0f, 0.0001f);
        }

        beginTest ("parseProgressLine accepts a fractional percentage");
        {
            float value = -1.0f;
            expect (parseProgressLine ("PROGRESS:33.5", value));
            expectWithinAbsoluteError (value, 0.335f, 0.0001f);
        }

        beginTest ("parseProgressLine rejects a line without the PROGRESS: prefix");
        {
            float value = -1.0f;
            expect (! parseProgressLine ("frame=120 fps=30 time=00:00:04.00", value));
        }

        beginTest ("parseProgressLine rejects a PROGRESS: line with a non-numeric value");
        {
            float value = -1.0f;
            expect (! parseProgressLine ("PROGRESS:abc", value));
        }

        beginTest ("parseProgressLine rejects an empty value");
        {
            float value = -1.0f;
            expect (! parseProgressLine ("PROGRESS:", value));
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
                          int, std::function<void (float)>, std::function<bool()>,
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
            const bool ok = io::runFormatExtensionExport (ext, project, plugins, outputFile, {}, runner, error, warnings);

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

        beginTest ("runFormatExtensionExport passes no timeout (-1) when the extension has customUI: true");
        {
            struct TimeoutCapturingRunner final : public FormatExtensionRunner
            {
                int lastTimeoutMs = -999;

                bool run (const juce::StringArray&, const juce::File& expectedOutput,
                          int timeoutMs, std::function<void (float)>, std::function<bool()>,
                          juce::String&) override
                {
                    lastTimeoutMs = timeoutMs;
                    expectedOutput.create();
                    return true;
                }
            } capturingRunner;   // not "runner": juce::UnitTest has a member of that name

            Settings settings;
            PluginManager plugins (settings);
            Project project;

            FormatExtension ext;
            ext.id = "com.example.test";
            ext.name = "Test";
            ext.fileExtension = "test";
            ext.executable = juce::File ("C:/fake/tool.exe");
            ext.customUI = true;

            const auto outputFile = tempRoot.getChildFile ("customui-timeout.test");
            outputFile.deleteFile();

            juce::String error;
            juce::StringArray warnings;
            const bool ok = io::runFormatExtensionExport (ext, project, plugins, outputFile, {},
                                                           capturingRunner, error, warnings);

            expect (ok, error);
            expectEquals (capturingRunner.lastTimeoutMs, -1);
        }

        beginTest ("runFormatExtensionExport passes the default 120000ms timeout when customUI is false");
        {
            struct TimeoutCapturingRunner final : public FormatExtensionRunner
            {
                int lastTimeoutMs = -999;

                bool run (const juce::StringArray&, const juce::File& expectedOutput,
                          int timeoutMs, std::function<void (float)>, std::function<bool()>,
                          juce::String&) override
                {
                    lastTimeoutMs = timeoutMs;
                    expectedOutput.create();
                    return true;
                }
            } capturingRunner;   // not "runner": juce::UnitTest has a member of that name

            Settings settings;
            PluginManager plugins (settings);
            Project project;

            FormatExtension ext;
            ext.id = "com.example.test";
            ext.name = "Test";
            ext.fileExtension = "test";
            ext.executable = juce::File ("C:/fake/tool.exe");
            // customUI left at its default (false)

            const auto outputFile = tempRoot.getChildFile ("customui-timeout-default.test");
            outputFile.deleteFile();

            juce::String error;
            juce::StringArray warnings;
            const bool ok = io::runFormatExtensionExport (ext, project, plugins, outputFile, {},
                                                           capturingRunner, error, warnings);

            expect (ok, error);
            expectEquals (capturingRunner.lastTimeoutMs, 120000);
        }

        beginTest ("runFormatExtensionExport sets additionalInputs as environment variables during the run, and clears them after");
        {
            struct EnvCheckingRunner final : public FormatExtensionRunner
            {
                juce::String observedDuringRun;

                bool run (const juce::StringArray&, const juce::File& expectedOutput,
                          int, std::function<void (float)>, std::function<bool()>,
                          juce::String&) override
                {
                    observedDuringRun = juce::SystemStats::getEnvironmentVariable ("KANADE_DAW_TEST_VAR", "<unset>");
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

            const auto outputFile = tempRoot.getChildFile ("export-envvar-out.test");
            outputFile.deleteFile();

            const auto probeFile = tempRoot.getChildFile ("probe-value.txt");
            const std::map<juce::String, juce::String> additionalInputs { { "KANADE_DAW_TEST_VAR", probeFile.getFullPathName() } };

            juce::String error;
            juce::StringArray warnings;
            const bool ok = io::runFormatExtensionExport (ext, project, plugins, outputFile, additionalInputs,
                                                           runner, error, warnings);

            expect (ok, error);
            expectEquals (runner.observedDuringRun, probeFile.getFullPathName());

            // Cleared once the export call has returned - must not leak into
            // whatever KANADE DAW (or the next extension invocation) does next.
            expectEquals (juce::SystemStats::getEnvironmentVariable ("KANADE_DAW_TEST_VAR", "<unset>"),
                          juce::String ("<unset>"));
        }

        beginTest ("runFormatExtensionExport fails and returns the runner's error when the runner reports failure");
        {
            struct FailingRunner final : public FormatExtensionRunner
            {
                bool run (const juce::StringArray&, const juce::File&,
                          int, std::function<void (float)>, std::function<bool()>,
                          juce::String& errorOut) override
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
                                                       tempRoot.getChildFile ("wont-be-made.test"), {},
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
                          int, std::function<void (float)>, std::function<bool()>,
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
                bool run (const juce::StringArray&, const juce::File&,
                          int, std::function<void (float)>, std::function<bool()>,
                          juce::String& errorOut) override
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
