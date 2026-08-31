using System.IO.Compression;
using System.Xml.Linq;

// KANADE DAW format extension: dumps a .dawproject file to a human-readable text summary.
// Usage: dawproject-dump.exe --export <input.dawproject> <output.txt>
try
{
    if (args.Length != 3 || args[0] != "--export")
    {
        Console.Error.WriteLine("Usage: dawproject-dump --export <input.dawproject> <output.txt>");
        return 1;
    }

    string inputPath = args[1];
    string outputPath = args[2];

    string projectXml = ReadProjectXmlFromZip(inputPath);
    XDocument doc = XDocument.Parse(projectXml);
    XElement? project = doc.Element("Project");
    if (project is null)
    {
        Console.Error.WriteLine("project.xml has no <Project> root element.");
        return 1;
    }

    string summary = BuildSummary(project);
    File.WriteAllText(outputPath, summary);
    return 0;
}
catch (Exception ex)
{
    Console.Error.WriteLine($"dawproject-dump failed: {ex.Message}");
    return 1;
}

static string ReadProjectXmlFromZip(string dawprojectPath)
{
    using ZipArchive zip = ZipFile.OpenRead(dawprojectPath);
    ZipArchiveEntry? entry = zip.Entries.FirstOrDefault(e =>
        e.FullName.Equals("project.xml", StringComparison.OrdinalIgnoreCase) ||
        e.FullName.EndsWith("/project.xml", StringComparison.OrdinalIgnoreCase));
    if (entry is null)
        throw new InvalidDataException("project.xml not found inside the .dawproject archive.");

    using Stream stream = entry.Open();
    using StreamReader reader = new(stream);
    return reader.ReadToEnd();
}

static string BuildSummary(XElement project)
{
    var lines = new List<string> { "KANADE DAW project summary" };

    // Tolerant lookup: Tempo/TimeSignature may not sit directly under Transport.
    XElement? tempo = project.Descendants("Tempo").FirstOrDefault();
    XElement? timeSig = project.Descendants("TimeSignature").FirstOrDefault();
    string tempoStr = tempo?.Attribute("value")?.Value ?? "?";
    string tsStr = (timeSig?.Attribute("numerator")?.Value ?? "?") + "/" + (timeSig?.Attribute("denominator")?.Value ?? "?");
    lines.Add($"Tempo: {tempoStr} bpm ({tsStr})");

    // Clip counts per track id, from Arrangement/.../Clips[@track=id]/Clip (tolerant of nesting depth).
    var clipCounts = project.Descendants("Clips")
        .Where(c => c.Attribute("track") != null)
        .GroupBy(c => c.Attribute("track")!.Value)
        .ToDictionary(g => g.Key, g => g.Sum(c => c.Descendants("Clip").Count()));

    var tracks = project.Descendants("Structure").FirstOrDefault()?.Descendants("Track").ToList()
                 ?? new List<XElement>();

    var regularTracks = new List<(string Name, string ContentType, int Clips)>();
    int busCount = 0;
    foreach (XElement track in tracks)
    {
        string id = track.Attribute("id")?.Value ?? "";
        string name = track.Attribute("name")?.Value ?? id;
        string contentType = track.Attribute("contentType")?.Value ?? "unknown";
        string role = track.Element("Channel")?.Attribute("role")?.Value ?? "regular";

        if (role is "submix" or "master")
        {
            busCount++;
            continue;
        }

        int clipCount = clipCounts.TryGetValue(id, out int c) ? c : 0;
        regularTracks.Add((name, contentType, clipCount));
    }

    lines.Add($"Tracks: {regularTracks.Count}" + (busCount > 0 ? $" (+{busCount} bus/master)" : ""));
    foreach (var t in regularTracks)
        lines.Add($"  - {t.Name} ({t.ContentType}) - {t.Clips} clip{(t.Clips == 1 ? "" : "s")}");

    return string.Join(Environment.NewLine, lines) + Environment.NewLine;
}
