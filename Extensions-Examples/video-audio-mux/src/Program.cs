using System.Globalization;
using System.Diagnostics;

// KANADE DAW format extension: existing video + KANADE DAW's own mixdown -> muxed .mp4 (export only).
// Invocation: video-audio-mux.exe --export <input.dawproject> <output.mp4>
//
// The .dawproject argument is never opened - this plugin is pure audio/video
// muxing, not project-structure conversion. Output path is taken as the last
// argv element rather than a hardcoded index: reaper-bridge (this project's
// sibling extension, and .NET's own Main(string[] args) convention generally)
// observes args[0] == "--export" (the program name is not included in args[]),
// but FormatExtensionRunner.cpp's ChildProcess::start() call places the exe's
// own path as the first StringArray element too - taking the last element
// sidesteps that ambiguity rather than betting on one specific index.
//
// Two extra inputs arrive as environment variables (KANADE DAW's
// `additionalInputs` manifest mechanism - see manifest.json in this folder):
//   KANADE_DAW_VIDEO_FILE   - the user-picked video to combine with
//   KANADE_DAW_MIXDOWN_WAV  - KANADE DAW's own rendered project mixdown

try
{
    if (args.Length < 3 || Array.IndexOf(args, "--export") < 0)
    {
        Console.Error.WriteLine("usage: video-audio-mux.exe --export <input.dawproject> <output.mp4>");
        return 1;
    }

    string outputPath = args[^1];

    string? videoPath = Environment.GetEnvironmentVariable("KANADE_DAW_VIDEO_FILE");
    if (string.IsNullOrEmpty(videoPath) || !File.Exists(videoPath))
    {
        Console.Error.WriteLine(
            "KANADE_DAW_VIDEO_FILE is not set or does not exist - this plugin must be invoked by " +
            "KANADE DAW's Export menu, not run standalone without it.");
        return 1;
    }

    string? mixdownPath = Environment.GetEnvironmentVariable("KANADE_DAW_MIXDOWN_WAV");
    if (string.IsNullOrEmpty(mixdownPath) || !File.Exists(mixdownPath))
    {
        Console.Error.WriteLine(
            "KANADE_DAW_MIXDOWN_WAV is not set or does not exist - this plugin must be invoked by " +
            "KANADE DAW's Export menu, not run standalone without it.");
        return 1;
    }

    double mixBalance = 0.5;
    string? mixBalanceText = Environment.GetEnvironmentVariable("MUX_MIX_BALANCE");
    // double.TryParse's out parameter is documented to be set to zero on
    // failure (not left at mixBalance's prior value) - so the parsed value
    // is only assigned into mixBalance when TryParse actually succeeds,
    // otherwise the 0.5 default above would silently become 0.0 for any
    // unparsable (but non-empty) MUX_MIX_BALANCE value.
    if (!string.IsNullOrEmpty(mixBalanceText) &&
        double.TryParse(mixBalanceText, NumberStyles.Float, CultureInfo.InvariantCulture, out double parsedMixBalance))
        mixBalance = parsedMixBalance;
    mixBalance = Math.Clamp(mixBalance, 0.0, 1.0);

    string ffmpegPath = Path.Combine(AppContext.BaseDirectory, "ffmpeg.exe");
    if (!File.Exists(ffmpegPath))
    {
        Console.Error.WriteLine(
            "ffmpeg.exe was not found next to video-audio-mux.exe - place a Windows ffmpeg.exe build " +
            "(e.g. from https://github.com/BtbN/FFmpeg-Builds, an LGPL release) in this folder.");
        return 1;
    }

    string ffprobePath = Path.Combine(AppContext.BaseDirectory, "ffprobe.exe");
    if (!File.Exists(ffprobePath))
    {
        Console.Error.WriteLine(
            "ffprobe.exe was not found next to video-audio-mux.exe - place a Windows ffprobe.exe build " +
            "(e.g. from https://github.com/BtbN/FFmpeg-Builds, an LGPL release) in this folder.");
        return 1;
    }

    double duration = ProbeDuration(ffprobePath, videoPath);
    bool videoHasAudio = ProbeHasAudioStream(ffprobePath, videoPath);
    string durationArg = duration.ToString("0.###", CultureInfo.InvariantCulture);

    // Pad whichever audio input is shorter than the video with silence, trim
    // whichever is longer, so the muxed audio is always exactly the video's
    // own duration - then amix the two together (or just the mixdown, if the
    // input video has no audio of its own).
    // amix's own default normalization already scales each input by 1/inputs
    // (i.e. 0.5 each for two inputs, which is exactly what this plugin
    // produced before mixBalance existed) - to make mixBalance a genuine
    // 0..1 weighting rather than stacking a second halving on top of that
    // default, disable amix's normalization (normalize=0) and apply the
    // balance explicitly instead. At mixBalance 0.5 this reproduces the
    // exact same 0.5/0.5 mix as before, so today's default behaviour is
    // unchanged.
    string videoVolumeArg = (1.0 - mixBalance).ToString("0.###", CultureInfo.InvariantCulture);
    string mixdownVolumeArg = mixBalance.ToString("0.###", CultureInfo.InvariantCulture);

    string filterComplex = videoHasAudio
        ? $"[0:a]apad,atrim=0:{durationArg},volume={videoVolumeArg}[a0];" +
          $"[1:a]apad,atrim=0:{durationArg},volume={mixdownVolumeArg}[a1];" +
          "[a0][a1]amix=inputs=2:duration=first:dropout_transition=0:normalize=0[aout]"
        : $"[1:a]apad,atrim=0:{durationArg}[aout]";

    var ffmpegArgs = new List<string>
    {
        "-y",
        "-i", videoPath,
        "-i", mixdownPath,
        "-filter_complex", filterComplex,
        "-map", "0:v",
        "-map", "[aout]",
        "-c:v", "copy",
        "-c:a", "aac",
        "-t", durationArg,
        "-nostats",
        "-progress", "pipe:1",
        outputPath
    };

    var (exitCode, stderrText) = RunFfmpegWithProgress(ffmpegPath, ffmpegArgs, duration);

    if (exitCode != 0)
    {
        Console.Error.WriteLine(stderrText.Trim().Length > 0 ? stderrText.Trim() : $"ffmpeg exited with code {exitCode}.");
        return 1;
    }

    if (!File.Exists(outputPath))
    {
        Console.Error.WriteLine("ffmpeg exited successfully but did not produce \"" + outputPath + "\".");
        return 1;
    }

    return 0;
}
catch (Exception ex)
{
    Console.Error.WriteLine($"error: {ex.Message}");
    return 1;
}

static double ProbeDuration(string ffprobePath, string filePath)
{
    var (exitCode, stdout, stderrText) = RunProcess(ffprobePath,
        new List<string> { "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", filePath });

    if (exitCode != 0 || !double.TryParse(stdout.Trim(), NumberStyles.Float, CultureInfo.InvariantCulture, out double duration))
        throw new InvalidOperationException($"could not probe duration of \"{filePath}\": {stderrText.Trim()}");

    return duration;
}

static bool ProbeHasAudioStream(string ffprobePath, string filePath)
{
    var (exitCode, stdout, stderrText) = RunProcess(ffprobePath,
        new List<string> { "-v", "error", "-select_streams", "a", "-show_entries", "stream=index", "-of", "csv=p=0", filePath });

    if (exitCode != 0)
        throw new InvalidOperationException($"could not probe streams of \"{filePath}\": {stderrText.Trim()}");

    return stdout.Trim().Length > 0;
}

static (int ExitCode, string Stdout, string Stderr) RunProcess(string exePath, List<string> arguments)
{
    var startInfo = new ProcessStartInfo(exePath)
    {
        RedirectStandardOutput = true,
        RedirectStandardError = true,
        UseShellExecute = false,
    };
    foreach (var arg in arguments)
        startInfo.ArgumentList.Add(arg);

    using var process = Process.Start(startInfo)
        ?? throw new InvalidOperationException($"could not start \"{exePath}\"");

    // Read both streams concurrently (not sequential ReadToEnd calls) so a
    // chatty stderr (ffmpeg's progress output) can't fill its pipe buffer and
    // deadlock while stdout is being drained, or vice versa.
    var stdoutTask = process.StandardOutput.ReadToEndAsync();
    var stderrTask = process.StandardError.ReadToEndAsync();
    Task.WaitAll(stdoutTask, stderrTask);
    process.WaitForExit();

    return (process.ExitCode, stdoutTask.Result, stderrTask.Result);
}

// Like RunProcess, but streams ffmpeg's own stdout (populated by
// "-progress pipe:1" as key=value lines) while ffmpeg runs, converting
// each out_time= line into a KANADE_DAW "PROGRESS:<0-100>" line printed
// to THIS process's own stdout - which is what FormatExtensionRunner.cpp's
// reader thread drains into KANADE DAW's TaskPanel progress bar. ffmpeg's
// stderr is still captured in full for the failure-path error message,
// exactly as RunProcess already does.
static (int ExitCode, string Stderr) RunFfmpegWithProgress(string ffmpegPath, List<string> arguments,
    double totalDurationSeconds)
{
    var startInfo = new ProcessStartInfo(ffmpegPath)
    {
        RedirectStandardOutput = true,
        RedirectStandardError = true,
        UseShellExecute = false,
    };
    foreach (var arg in arguments)
        startInfo.ArgumentList.Add(arg);

    using var process = Process.Start(startInfo)
        ?? throw new InvalidOperationException($"could not start \"{ffmpegPath}\"");

    var stderrTask = process.StandardError.ReadToEndAsync();

    var progressTask = Task.Run(async () =>
    {
        string? line;
        while ((line = await process.StandardOutput.ReadLineAsync()) != null)
        {
            if (!line.StartsWith("out_time="))
                continue;

            if (!TimeSpan.TryParse(line.Substring("out_time=".Length), CultureInfo.InvariantCulture,
                    out TimeSpan elapsed))
                continue;

            int percent = totalDurationSeconds > 0
                ? (int)Math.Clamp(elapsed.TotalSeconds / totalDurationSeconds * 100.0, 0, 100)
                : 0;

            Console.WriteLine($"PROGRESS:{percent}");
            Console.Out.Flush();
        }
    });

    Task.WaitAll(stderrTask, progressTask);
    process.WaitForExit();

    return (process.ExitCode, stderrTask.Result);
}
