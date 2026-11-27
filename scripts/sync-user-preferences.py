#!/usr/bin/env python3
"""Capture portable Oldassity defaults from a closed local instance."""

import argparse
import base64
import binascii
import html
import sys
from urllib.parse import unquote
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
    "Batch": "ActiveMacro",
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
    "GUI/Shortcuts": "FullDefaults",
    "GUI/TrackNames": "DateStamp RecodingTrackName RecordingNameCustom TimeStamp TrackNumber",
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
# LV2 IDs can contain an HTTP namespace. Strip only known identifier syntax
# before checking for filesystem paths, including paths inside encoded XML.
LV2_ID = re.compile(r"https?://lsp-plug\.in/(?:plugins/)?lv2/[A-Za-z0-9_.-]+")
PRIVATE = re.compile(
    r"[/\\]|(?:^|\W)~[A-Za-z0-9_.-]+[/\\]|\b(?:file|smb|ssh):|"
    r"\b(?:path|filename|directory|folder|token|password|secret|credential|account)\s*=",
    re.I,
)
CHAIN = re.compile(r"GUI/Realtime(?:Master|Track)Presets(?:/[0-9]+)?")
MACRO_NAME = re.compile(r"[\w .()+-]+\.txt")


def portable(value, *, chain=False, lv2=False):
    value = html.unescape(unquote(value))
    if lv2:
        if chain:
            value = re.sub(r'(\bid=\\?")([^"]+)', lambda match: match[1] + LV2_ID.sub("", match[2]), value)
        else:
            value = LV2_ID.sub("", value)
    if re.search(r"[A-Za-z]:[/\\]|\\\\", value):
        return False
    if chain:
        value = re.sub(r"</([A-Za-z][A-Za-z0-9_-]*)>|/>", r"\1", value)
    value = re.sub(r'\\(?:n|r|t|"|\x27)', "", value)
    return not PRIVATE.search(value)


def portable_favorite(favorite):
    favorite = re.sub(r"\\{1,2}_", "_", favorite)
    if not favorite.startswith("Effect_"):
        return False
    if portable(favorite, lv2=True):
        return True

    # An LV2 ID ends in the plugin URI. LSP supplies a public URI, while
    # the preceding vendor and display name can contain a slash (e.g. n/a).
    prefix, separator, uri = favorite.rpartition("_http")
    if not (separator and prefix.startswith("Effect_LV2_") and
            LV2_ID.fullmatch("http" + uri) and ".." not in uri):
        return False
    # Permit slashes within display labels, but reject rooted paths,
    # traversal, URLs, and Windows separators in the metadata.
    metadata = html.unescape(unquote(prefix))
    return bool(re.fullmatch(r"Effect_LV2_[\w .,+()&'!#%/-]+", metadata)
                and ".." not in metadata
                and not re.search(r"(^|[_\s])/|//|/(?:home|Users|tmp|mnt|share)/", metadata))


def collect_favorites(value):
    # wxFileConfig serializes the newline between IDs as a backslash escape.
    quoted = len(value) >= 2 and value.startswith('"') and value.endswith('"')
    if quoted:
        value = value[1:-1]
    favorites = re.split(r"\\n", value)
    retained = [
        favorite for favorite in favorites
        if portable_favorite(favorite)
    ]
    skipped = sum(bool(favorite) for favorite in favorites) - len(retained)
    print(f"Captured {len(retained)} realtime favorites. Skipped {skipped} nonportable IDs.", file=sys.stderr)
    result = r"\n".join(retained)
    return f'"{result}"' if quoted else result


def allowed(group, key, value):
    if group == "NewKeys":
        return True
    if CHAIN.fullmatch(group):
        return key in {"Count", "Name", "Chain"} and portable(
            value, chain=key == "Chain", lv2=key == "Chain"
        )
    if group.startswith("GUI/ToolBars/"):
        if key == "Path":
            return bool(re.fullmatch(r"(?:\d+(?:,\d+)*)?", value))
        return key in {"Dock", "DockV2", "H", "Show", "W", "X", "Y", "MultiToolActive"} and bool(
            re.fullmatch(r"-?\d+", value)
        )
    if not SCALAR.fullmatch(value) or ("\\" in value and key != "MenuBar"):
        return False
    if FILE_SETTING.search(key):
        return False
    if group == "Version":
        return key in GROUP_KEYS[group].split() and bool(re.fullmatch(r"\d+", value))
    if group == "FileFormats" or group.startswith("FileFormats/"):
        return key not in {"FFmpegPreset", "FFmpegTag"}
    if group.startswith(("MenuBar/", "Effects/")) or group == "MenuBar":
        return True
    return key in GROUP_KEYS.get(group, "").split()


def sections(config):
    groups = {}
    group = ""
    for line in config.read_text(encoding="utf-8-sig").splitlines():
        if line.startswith("[") and line.endswith("]"):
            group = line[1:-1]
            continue
        key, separator, value = line.partition("=")
        if separator and key and not key.startswith(("#", ";")):
            groups.setdefault(group, {})[key] = value
    return groups


def render(groups):
    lines = []
    for group, entries in sorted(groups.items()):
        if not entries:
            continue
        if group:
            lines.append(f"[{group}]")
        lines.extend(f"{key}={value}" for key, value in sorted(entries.items()))
    return "\n".join(lines) + "\n" if lines else ""


def collect(config):
    groups = {}
    source = sections(config)
    for group, entries in source.items():
        if group and not all(re.fullmatch(IDENT, part) for part in group.split("/")):
            continue
        groups[group] = {
            key: value for key, value in entries.items()
            if (group == "NewKeys" or re.fullmatch(IDENT, key))
            and allowed(group, key, value)
        }
    favorites = source.get("GUI", {}).get("RealtimeEffectFavorites")
    if favorites is not None:
        groups.setdefault("GUI", {})["RealtimeEffectFavorites"] = collect_favorites(favorites)

    # This toolbar defaults to visible when there is no stored visibility.
    groups.setdefault("GUI/ToolBars/Audio Setup", {}).setdefault("Show", "0")

    if set(groups.get("Version", {})) != {"Major", "Minor", "Micro"}:
        raise SystemExit("Expected a saved Oldassity config with version information.")
    for kind in ("Track", "Master"):
        parent = f"GUI/Realtime{kind}Presets"
        if parent not in groups:
            continue
        children = sorted(
            (group for group in groups if group.startswith(parent + "/")),
            key=lambda group: int(group.rsplit("/", 1)[1])
            if CHAIN.fullmatch(group) else -1,
        )
        retained = []
        for group in children:
            entry = groups.pop(group)
            if CHAIN.fullmatch(group) and entry.get("Name") and entry.get("Chain"):
                retained.append(entry)
            else:
                print(f"Skipped a {kind.lower()} stack preset with nonportable data.", file=sys.stderr)
        groups[parent]["Count"] = str(len(retained))
        for index, entry in enumerate(retained):
            groups[f"{parent}/{index}"] = entry
    return render(groups)


def portable_effect_settings_id(identifier):
    if not identifier.startswith("Effect_"):
        return False
    if portable(identifier):
        return True

    fields = identifier.split("_", 3)
    if len(fields) != 4 or fields[1] != "LV2":
        return False
    vendor, name = fields[2:]
    # LV2 display labels can contain a slash, as in "Stereo Mid/Side EQ".
    return (
        portable(vendor)
        and ".." not in name
        and bool(re.fullmatch(r"[\w .()+-]+ [\w.+()-]+/[\w.+()-]+ [\w .()+-]+", name))
    )


def plugin_settings(config):
    if not config.is_file():
        return ""
    groups = {}
    omitted = 0
    for group, entries in sections(config).items():
        if not group:
            groups[group] = {
                key: value for key, value in entries.items()
                if key == "pluginsettingsversion" and re.fullmatch(r"[0-9.]+", value)
            }
            continue
        match = re.fullmatch(
            r"pluginsettings/base64\\?:((?:[A-Za-z0-9]|\\?[+/])+(?:\\?=){0,2})/(shared|private)(/.*)?",
            group,
        )
        if not match:
            omitted += 1
            continue
        try:
            identifier = base64.b64decode(re.sub(r"\\([+/=])", r"\1", match[1]), validate=True).decode("utf-8")
        except (UnicodeError, binascii.Error):
            omitted += 1
            continue
        subgroup = (match[3] or "").lstrip("/").replace(r"\ ", " ")
        if (not portable_effect_settings_id(identifier)
                or subgroup and not all(re.fullmatch(IDENT, part) for part in subgroup.split("/"))):
            omitted += 1
            continue
        if all(not FILE_SETTING.search(key) and portable(key) and portable(value)
               for key, value in entries.items()):
            groups[group] = entries
        else:
            omitted += 1
    captured = sum(bool(group and entries) for group, entries in groups.items())
    print(f"Captured {captured} plugin settings sections.", file=sys.stderr)
    if omitted:
        print(f"Skipped {omitted} plugin settings sections with unrecognized or nonportable data.", file=sys.stderr)
    return render(groups)


def collect_macros(directory):
    macros = []
    omitted = 0
    for macro in sorted(directory.glob("*.txt")):
        if (macro.is_symlink() or not macro.is_file()
                or not MACRO_NAME.fullmatch(macro.name)):
            omitted += 1
            continue
        contents = macro.read_text(encoding="utf-8-sig")
        if all(portable(line) for line in contents.splitlines()):
            macros.append((macro.name, contents))
        else:
            omitted += 1
    if omitted:
        print(f"Skipped {omitted} macros with nonportable names or commands.", file=sys.stderr)
    return macros


def literal(value):
    # ASCII source keeps non-ASCII macro names and preferences independent
    # of the compiler's source encoding. Fixed-width octal escapes cannot
    # consume digits from the next byte.
    def escape(byte):
        if 32 <= byte < 127 and byte not in (34, 92):
            return chr(byte)
        return f"\\{byte:03o}"

    lines = ['"' + "".join(map(escape, line)) + '"'
             for line in value.encode("utf-8").splitlines(keepends=True)]
    return "\n".join(lines) or '""'


def generate(preferences, plugins, macros):
    bundle = (
        f"constexpr char shippedPreferences[] =\n{literal(preferences)};\n"
        f"constexpr char shippedPluginSettings[] =\n{literal(plugins)};\n"
        "constexpr ShippedMacro shippedMacros[] = {\n"
    )
    for name, contents in macros:
        bundle += f"   {{{literal(name)},\n{literal(contents)}}},\n"
    return bundle + "   {nullptr, nullptr},\n};\n"


def git(*args):
    return subprocess.check_output(["git", "-C", str(ROOT), *args], text=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    config_home = Path(os.environ.get("XDG_CONFIG_HOME") or Path.home() / ".config")
    parser.add_argument("--config", type=Path, default=config_home / "oldassity/audacity.cfg")
    data_home = Path(os.environ.get("XDG_DATA_HOME") or Path.home() / ".local/share")
    parser.add_argument("--macros-dir", type=Path, default=data_home / "oldassity/Macros")
    parser.add_argument("--plugin-settings", type=Path)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--preview", action="store_true")
    mode.add_argument("--commit", action="store_true")
    args = parser.parse_args()
    profile = collect(args.config)
    plugins = plugin_settings(args.plugin_settings or args.config.with_name("pluginsettings.cfg"))
    macros = collect_macros(args.macros_dir)
    contents = generate(profile, plugins, macros)
    if args.preview:
        print(contents, end="")
        return

    target = ROOT / PROFILE
    if git("status", "--porcelain", "--", PROFILE):
        raise SystemExit("The defaults profile already has uncommitted changes.")
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
