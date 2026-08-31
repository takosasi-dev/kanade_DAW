using System.Globalization;
using System.IO.Compression;
using System.Text;
using System.Xml.Linq;

// KANADE DAW format extension: .dawproject -> Reaper .rpp (export only).
// Invocation: reaper-bridge.exe --export <input.dawproject> <output.rpp>
// (Main's args[] excludes the exe name, so args[0] is "--export".)

try
{
    if (args.Length < 3 || args[0] != "--export")
    {
        Console.Error.WriteLine("usage: reaper-bridge.exe --export <input.dawproject> <output.rpp>");
        return 1;
    }

    ExportDawprojectToRpp(args[1], args[2]);
    return 0;
}
catch (Exception ex)
{
    Console.Error.WriteLine($"error: {ex.Message}");
    return 1;
}

static void ExportDawprojectToRpp(string inputPath, string outputPath)
{
    using var zip = ZipFile.OpenRead(inputPath);
    var projectEntry = zip.GetEntry("project.xml")
        ?? throw new InvalidDataException("project.xml not found inside .dawproject archive");

    XDocument doc;
    using (var stream = projectEntry.Open())
        doc = XDocument.Load(stream);

    var root = doc.Root ?? throw new InvalidDataException("project.xml has no root element");

    // --- Tempo / time signature (constant-tempo assumption: a v1 limitation,
    // real dawproject files can contain tempo changes mid-timeline) ---
    var tempoEl = root.Descendants("Tempo").FirstOrDefault();
    double bpm = ParseDouble(tempoEl?.Attribute("value")?.Value) ?? 120.0;

    var tsEl = root.Descendants("TimeSignature").FirstOrDefault();
    double tsNum = ParseDouble(tsEl?.Attribute("numerator")?.Value) ?? 4.0;
    double tsDen = ParseDouble(tsEl?.Attribute("denominator")?.Value) ?? 4.0;

    // --- Regular (non-bus, non-master) tracks, in document order ---
    var structureEl = root.Element("Structure");
    var trackOrder = new List<string>();
    var trackNames = new Dictionary<string, string>();

    if (structureEl != null)
    {
        foreach (var trackEl in structureEl.Elements("Track"))
        {
            var id = trackEl.Attribute("id")?.Value;
            if (id == null) continue;

            var channelEl = trackEl.Element("Channel");
            var role = channelEl?.Attribute("role")?.Value ?? "regular";
            if (role == "submix" || role == "master") continue; // skip buses/master

            trackOrder.Add(id);
            trackNames[id] = trackEl.Attribute("name")?.Value ?? id;
        }
    }

    var trackItems = trackOrder.ToDictionary(id => id, _ => new List<RppItem>());

    // --- Media extraction bookkeeping ---
    string mediaDir = Path.Combine(Path.GetDirectoryName(Path.GetFullPath(outputPath)) ?? ".", "media");
    Directory.CreateDirectory(mediaDir);
    var archiveToLocal = new Dictionary<string, string>();
    var usedNames = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

    // --- Walk every <Clips track="..."> group anywhere under <Arrangement> ---
    foreach (var clipsGroup in root.Descendants("Clips"))
    {
        var trackId = clipsGroup.Attribute("track")?.Value;
        if (trackId == null || !trackItems.TryGetValue(trackId, out var items))
            continue; // clips on a bus/master/unknown track: not our concern

        foreach (var clipEl in clipsGroup.Elements("Clip"))
        {
            double? time = ParseDouble(clipEl.Attribute("time")?.Value);
            double? duration = ParseDouble(clipEl.Attribute("duration")?.Value);
            if (time == null || duration == null)
            {
                Console.Error.WriteLine(
                    $"warning: skipped clip with missing time/duration on track \"{trackNames[trackId]}\"");
                continue;
            }

            if (clipEl.Element("Notes") != null)
            {
                Console.Error.WriteLine(
                    $"warning: skipped MIDI clip on track \"{trackNames[trackId]}\" at beat " +
                    $"{FormatNum(time.Value)} (MIDI export not yet supported)");
                continue;
            }

            // <Audio> can sit directly under <Clip>, or nested under <Clip><Lanes><Warps>.
            var audioEl = clipEl.Descendants("Audio").FirstOrDefault();
            if (audioEl == null)
            {
                Console.Error.WriteLine(
                    $"warning: skipped clip with unrecognized content on track \"{trackNames[trackId]}\" " +
                    $"at beat {FormatNum(time.Value)}");
                continue;
            }

            var archivePath = audioEl.Element("File")?.Attribute("path")?.Value;
            if (string.IsNullOrEmpty(archivePath))
            {
                Console.Error.WriteLine(
                    $"warning: skipped audio clip with no file reference on track \"{trackNames[trackId]}\" " +
                    $"at beat {FormatNum(time.Value)}");
                continue;
            }

            string localFile;
            try
            {
                localFile = ExtractAudioFile(zip, archivePath, mediaDir, archiveToLocal, usedNames);
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine(
                    $"warning: skipped audio clip on track \"{trackNames[trackId]}\" " +
                    $"at beat {FormatNum(time.Value)}: {ex.Message}");
                continue;
            }

            items.Add(new RppItem(
                PositionSeconds: time.Value * 60.0 / bpm,
                LengthSeconds: duration.Value * 60.0 / bpm,
                Name: Path.GetFileNameWithoutExtension(localFile),
                RelativeMediaPath: "media/" + localFile));
        }
    }

    // --- Render the .rpp text ---
    var sb = new StringBuilder();
    sb.Append("<REAPER_PROJECT 0.1 \"6.0\" 0\n");
    sb.Append($"  TEMPO {FormatNum(bpm)} {FormatNum(tsNum)} {FormatNum(tsDen)}\n");

    foreach (var trackId in trackOrder)
    {
        sb.Append("  <TRACK\n");
        sb.Append($"    NAME \"{EscapeRppString(trackNames[trackId])}\"\n");
        foreach (var item in trackItems[trackId])
        {
            sb.Append("    <ITEM\n");
            sb.Append($"      POSITION {FormatNum(item.PositionSeconds)}\n");
            sb.Append($"      LENGTH {FormatNum(item.LengthSeconds)}\n");
            sb.Append($"      NAME \"{EscapeRppString(item.Name)}\"\n");
            sb.Append("      <SOURCE WAVE\n");
            sb.Append($"        FILE \"{item.RelativeMediaPath}\"\n");
            sb.Append("      >\n");
            sb.Append("    >\n");
        }
        sb.Append("  >\n");
    }
    sb.Append(">\n");

    File.WriteAllText(outputPath, sb.ToString());

    if (!File.Exists(outputPath))
        throw new IOException("output .rpp was not created");
}

// Extracts one audio entry from the .dawproject zip into mediaDir, reusing the
// same local filename if the same archive path was already extracted, and
// disambiguating with a numeric suffix if two different archive paths share
// a basename.
static string ExtractAudioFile(
    ZipArchive zip, string archivePath, string mediaDir,
    Dictionary<string, string> archiveToLocal, HashSet<string> usedNames)
{
    if (archiveToLocal.TryGetValue(archivePath, out var already))
        return already;

    var normalized = archivePath.Replace('\\', '/');
    var entry = zip.GetEntry(normalized) ?? zip.GetEntry(archivePath)
        ?? throw new FileNotFoundException($"audio file not found in archive: {archivePath}");

    string baseName = Path.GetFileName(normalized);
    string candidate = baseName;
    int counter = 2;
    while (usedNames.Contains(candidate))
    {
        candidate = $"{Path.GetFileNameWithoutExtension(baseName)}_{counter}{Path.GetExtension(baseName)}";
        counter++;
    }
    usedNames.Add(candidate);

    entry.ExtractToFile(Path.Combine(mediaDir, candidate), overwrite: true);
    archiveToLocal[archivePath] = candidate;
    return candidate;
}

static double? ParseDouble(string? s) =>
    s != null && double.TryParse(s, NumberStyles.Float, CultureInfo.InvariantCulture, out var v) ? v : null;

static string FormatNum(double v) => v.ToString("0.######", CultureInfo.InvariantCulture);

static string EscapeRppString(string s) => s.Replace("\"", "'");

internal record RppItem(double PositionSeconds, double LengthSeconds, string Name, string RelativeMediaPath);
