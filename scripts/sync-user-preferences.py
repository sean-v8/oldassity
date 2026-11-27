#!/usr/bin/env python3
"""Capture portable Oldassity defaults from a closed local instance."""

import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PROFILE = "src/OldassityPreferenceDefaults.inc"

# wxFileConfig values are kept verbatim, including their escape sequences.
# The allowlist describes settings, not entire files or arbitrary plugin state.
GROUP_KEYS = {
    "": """AudioTimeFormat BandwidthSelectionFormatName DefaultOpenType Exporters
           FrequencySelectionFormatName ImportExportPreferences Importers LibraryPreferences
           MenuBar NumericConverterRegistry Preferences SelectionFormat SelectionToolbarMode
           SendAnonymousUsageInfo SnapFunctions StatusBarFieldRegistry""",
    "AudioIO": """Crossfade CutPreviewAfterLen CutPreviewBeforeLen Duplex EffectsPreviewLen
                  LatencyCorrection LatencyDuration Microfades MixRenderWorker PreRoll
                  RecordChannels SWPlaythrough SeekLongPeriod SeekShortPeriod SilenceLevel
                  SoundActivatedRecord UnpinnedScrubbing""",
    "MidiIO": "SynthLatency",
    "GUI": """AdjustSelectionEdges AudioPasteDialogDefaultMode AudioTrackPastePolicy AutoScroll
              BPM BeepOnCompletion BlendThemes CircularTrackNavigation CollapseToHalfWave
              DefaultViewModeChoiceNew DefaultWaveformScaleChoice DialogForNameNewLabel
              EditClipCanMove EmptyCanBeDirty EnableCutLines EnvdBRange IntroOrderStart
              LowerTimeSig PasteAsNewClips PreferNewTrackRecord RetainLabels RtlWorkaround
              SampleViewChoice ScrollBeyondZero SelectAllOnNone ShowClipping ShowExtraMenus
              ShowRMS ShowSplashScreen ShowTrackNameInWaveform Solo Theme
              TracksFitVerticallyZoomed TypeToCreateLabel UpperTimeSig VerticalZooming
              ZoomPreset1Choice ZoomPreset2Choice""",
    "GUI/TrackNames": "DateStamp RecordingNameCustom TimeStamp TrackNumber",
    "Effects": "GroupBy RealtimeGroupBy SkipEffectsScanAtStartup",
    "Journal": "Enabled",
    "FFmpeg": "Enabled",
    "ExtendedImport": "OverrideExtendedImportByOpenFileDialogChoice",
    "ExportAudioDialog": """ExportRange Format IncludeAudioBeforeFirstLabel OverwriteExisting
                            SkipSilenceAtBeginning SplitMode SplitNamePolicy SampleRate Channels""",
    "Locale": "Language",
    "Preferences": "ImportExport Tracks",
    "Prefs": "Height PrefsCategory Width",
    "Prefs/KeyConfig": "ViewBy",
    "Quality": """DitherAlgorithmChoice HQDitherAlgorithmChoice
                  LibsoxrHQSampleRateConverterChoice LibsoxrSampleRateConverterChoice""",
    "QuickPlay": "ScrubbingEnabled ToolTips",
    "SamplingRate": "DefaultProjectSampleFormatChoice DefaultProjectSampleRate",
    "Snap": "Mode To",
    "Spectrum": """Algorithm ColorScheme EnableSpectralSelection FFTSize FrequencyGain Gain
                   MaxFreq MinFreq Range ScaleType WindowType ZeroPaddingFactor""",
    "FrequencyPlotDialog": "AlgChoice AxisChoice DrawGrid FuncChoice SizeChoice",
    "LabelEditor": "Height Width",
    "TagsEditorDialog": "height width",
    "Version": "Major Minor Micro",
    "Warnings": "DropoutDetected FirstProjectSave MissingExtension MixMono MixStereo MixUnknownChannels",
    "WaveTrackMenu/SubViews": "Extra",
    "Window": "Maximized",
}

IDENT = r"[A-Za-z0-9_. -]+"
SCALAR = re.compile(r"[\w .,:;+%#@*()'!|=<>~?{}&\-\\\[\]\"\t]*")
FILE_SETTING = re.compile(r"path|directory|filename|folder|url|token|password|secret|credential|account", re.I)
FAVORITE = re.compile(
    r"Effect_[^_\\/:]*_[^_\\/:]*_[^_\\/:]*_"
    r"(?:Builtin Effect: [\w .+-]+|Built-in Effect: [\w .+-]+|"
    r"https?://lsp-plug\.in/(?:plugins/)?lv2/[A-Za-z0-9_.-]+)"
)


def allowed(group, key, value):
    if group == "GUI" and key == "RealtimeEffectFavorites":
        return not value or all(FAVORITE.fullmatch(item) for item in value.split(r"\n"))
    if group.startswith("GUI/ToolBars/"):
        if key == "Path":
            return bool(re.fullmatch(r"(?:\d+(?:,\d+)*)?", value))
        return key in {"Dock", "DockV2", "H", "Show", "W", "X", "Y", "MultiToolActive"} and bool(
            re.fullmatch(r"-?\d+", value)
        )
    if not SCALAR.fullmatch(value) or "\\" in value and key not in {"MenuBar"} and group != "NewKeys":
        return False
    if FILE_SETTING.search(key):
        return False
    if group == "NewKeys":
        return bool(re.fullmatch(r"(?:[^\\]|\\t)*", value))
    if group == "Version":
        return key in GROUP_KEYS[group].split() and bool(re.fullmatch(r"\d+", value))
    if group == "FileFormats" or group.startswith("FileFormats/"):
        return key not in {"FFmpegPreset", "FFmpegTag"}
    if group.startswith("MenuBar/") or group in {"MenuBar", "Effects/NoiseReduction"}:
        return True
    return key in GROUP_KEYS.get(group, "").split()


def collect(config):
    groups = {}
    group = ""
    for line in config.read_text(encoding="utf-8-sig").splitlines():
        if line.startswith("[") and line.endswith("]"):
            group = line[1:-1]
            continue
        key, separator, value = line.partition("=")
        if separator and re.fullmatch(IDENT, key) and (
            not group or all(re.fullmatch(IDENT, part) for part in group.split("/"))
        ) and allowed(group, key, value):
            groups.setdefault(group, {})[key] = value
    if set(groups.get("Version", {})) != {"Major", "Minor", "Micro"}:
        raise SystemExit("Expected a saved Oldassity config with version information.")
    lines = []
    for group, entries in sorted(groups.items()):
        if group:
            lines.append(f"[{group}]")
        lines.extend(f"{key}={value}" for key, value in sorted(entries.items()))
    return "\n".join(lines) + "\n"


def git(*args):
    return subprocess.check_output(["git", "-C", str(ROOT), *args], text=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    config_home = Path(os.environ.get("XDG_CONFIG_HOME") or Path.home() / ".config")
    parser.add_argument("--config", type=Path, default=config_home / "oldassity/audacity.cfg")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--preview", action="store_true")
    mode.add_argument("--commit", action="store_true")
    args = parser.parse_args()
    profile = collect(args.config)
    if args.preview:
        print(profile, end="")
        return

    target = ROOT / PROFILE
    if git("status", "--porcelain", "--", PROFILE):
        raise SystemExit("The defaults profile already has uncommitted changes.")
    contents = f'R"Oldassity({profile})Oldassity"\n'
    if ')Oldassity"' in profile:
        raise SystemExit("The profile contains the C++ raw string delimiter.")
    if target.read_text(encoding="utf-8") == contents:
        print("Preferences already match the defaults profile.")
        return
    with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=target.parent, delete=False) as output:
        temporary = Path(output.name)
        output.write(contents)
    try:
        temporary.chmod(target.stat().st_mode & 0o777)
        temporary.replace(target)
    finally:
        temporary.unlink(missing_ok=True)
    if args.commit:
        print(git("commit", "-m", "Synchronize user preferences", "--", PROFILE), end="")


if __name__ == "__main__":
    main()
