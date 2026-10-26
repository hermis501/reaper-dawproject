# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

`reaper_dawproject`: a REAPER extension that registers `.dawproject` import (File > Open project) and *File > Export project to DAWproject...* (also the action `_DAWPROJECT_EXPORT`), plus the CLIs `dawproject2rpp` and `rpp2dawproject` built from the same core. The AAF extension lives in its own repository (reaper-aaf) and shares no code with this one; some files (`util`, `rpp_reader`, `rpp_sink`, `tempo_map`, `media_info`, `reaper_ext`, `converter`) started as the same code, so a fix here may be worth porting there by hand. User-facing docs: README.md (repository) and packaging/README.md (release zip).


## Commands

```
cmake -S . -B build -A x64                  # configure (Visual Studio generator)
cmake --build build --config Release        # DLL + dawproject2rpp.exe + rpp2dawproject.exe in build/Release
cmake --install build --config Release      # copies the DLL to %APPDATA%\REAPER\UserPlugins (REAPER must be closed)
python tools/make_package.py                # dist/DAWproject-for-REAPER (+ zip); user README in packaging/
python tests/test_converter.py              # import: generates .dawproject files, converts, asserts on the RPP
python tests/test_export.py                 # export: writes an .rpp, exports, asserts on the XML, converts back
build/Release/dawproject2rpp.exe in.dawproject out.rpp --media <dir>
build/Release/rpp2dawproject.exe in.rpp out.dawproject [--external-media]
```

The test scripts take an optional path (exe / bin folder). `test_export.py` also validates against the official XSD when `DAWPROJECT_XSD` (folder with `Project.xsd`, `MetaData.xsd`) and `JAVA` are set (uses `tests/Validate.java`). Official sample files can be generated from https://github.com/bitwig/dawproject (`./gradlew test` with JDK 21 writes them to `target/`); chaining them through both tools and validating is a good regression check.


## Architecture

Sources are in `src/`, CLIs in `cli/`, the bundled JSFX in `jsfx/`. The workflow `.github/workflows/build.yml` builds, tests (with XSD validation), packages, and creates a release for `v*` tags.

- Export commands are actions (a REAPER menu item runs an action) registered by `RegisterExportCommands` (`reaper_ext.cpp`, one copy per plug-in), which also adds them to the default File menu from `hookcustommenu` ("Main file", flag 0), after *Export project MIDI...* (40849), else after *Render...* (40015), else at the end; both DLLs' entries stay together. A user-customized File menu does not get them automatically. Command IDs are unchanged from before the move to the File menu (`_DAWPROJECT_EXPORT`, `_DAWPROJECT_EXPORT_AAF`, `_DAWPROJECT_EXPORT_AAF_EMBED`).
- `src/converter.cpp` does everything between XML and RPP lines. Flow in `Converter::Run`: index all `id` attributes → read transport → build the tempo map (Arrangement `TempoAutomation`/`TimeSignatureAutomation`, or `Points` targeting the transport tempo id) → flatten `Structure` into `tracks_` (folders via `depth`, master separately) → apply channels/devices → routing (non-parent destinations become `AUXRECV` + `MAINSEND 0`, sends become receives on the destination) → walk the arrangement → `Emit`.
- Timeline walking uses a `View`: a mapping from a timeline's local time to absolute time (`offset`, in beats or seconds) plus a visible window in absolute **seconds** (`ws`/`we`) with fades and mute. `Walk` dispatches by element name; `ProcessClip` narrows the window, handles `playStart`/`playStop`, loops (one sub-view per loop pass) and `reference` (alias clips via the id index). Leaves (`AudioLeaf`, `WarpsLeaf`, `NotesLeaf`, `ExpressionLeaf`, `PointsLeaf`) emit REAPER items/envelopes clipped to the window. Each clip collects its notes/MIDI expressions into one `MidiCollector` → one MIDI item.
- DAWproject time-unit rules: a timeline's `timeUnit` applies to itself and nested timelines (default beats); a clip's `time`/`duration` use the parent unit, its `playStart`/`loop*`/content use `contentTimeUnit`. `Rebase` switches a view's domain by converting the anchor through the tempo map.
- All positions written to RPP are seconds computed through `TempoMap` (piecewise constant/linear-in-beats ramps); MIDI events are ticks (960/QN) relative to the item start in beats.
- `src/plugin_state.cpp` turns `.vstpreset` (VST3), `.fxp/.fxb` (VST2) and raw CLAP state into REAPER `<VST`/`<CLAP` blocks. The VST3 layout (header block with id, `0xFEED5EEE`, pin masks, size; data block `[compLen][1][comp][contLen][0][cont]`; footer) and the id hash (FNV-1a of the class id in COM byte order) were verified byte-for-byte against REAPER 7 project files. Returning false means "plug-in unidentifiable, skip it"; a set `error` with true means "loaded without state".
- Import, post-load step: VST3/CLAP parameter envelopes (REAPER addresses parameters by index, DAWproject by the plug-in's parameter id) and VCA groups can only be set up once REAPER has loaded the project. With `ConvertOptions::postLoadFixups` they are returned in `ConvertReport::envelopes/groups`, and `src/reaper_main.cpp` applies them from a one-shot timer via `TrackFX_GetParamIdent`, `GetFXEnvelope` and `GetSetTrackGroupMembership`. VST2 and JSFX automation is written directly as `<PARMENV index min max default` (VST2 index = parameterID, JSFX index = slider index). VCA membership is read as "channel destination = a VCA channel" (the DAWproject spec does not define it further).
- Built-in devices (`src/builtin_fx.cpp`) map to the JSFX in `jsfx/`. CMake embeds those into the generated `builtin_jsfx_data.h`, and the extension writes them to `<resource>/Effects/DAWproject/` at startup. The slider order in the JSFX and the tables in `builtin_fx.cpp` must stay in sync. JSFX sliders use ms where DAWproject uses seconds.
- Export (`src/exporter.cpp`): `ParseRpp` → tempo map from `TEMPOENVEX` (seconds) → tracks (ISBUS nesting becomes nested `<Track>`s; master first) → FX chains (`ReadReaperPlugin` decodes REAPER's base64 blocks back to `.vstpreset`/`.fxp`/CLAP state) → routing → items (active take; audio content in seconds, play rate as `Warps`; MIDI ticks become notes and expression points) → envelopes → markers/tempo → ZIP via miniz. XML element order follows `Project.xsd` (e.g. Channel: Devices, Mute, Pan, Sends, Volume). The File menu command saves a temp copy with `Main_SaveProjectEx(proj, tmp, 0)`; without `&8` the project's file name is untouched. It then exports that text.
- `rpp_sink.h`: converter output goes through `RppSink` (REAPER's `ProjectStateContext` in the DLL, an indented file in the CLI). Lines starting with `<` open a block, `>` closes it.
- Paths are UTF-8 everywhere; `util.cpp` converts to wide APIs on Windows. Numbers are formatted/parsed with `to_chars`/`from_chars` (locale-independent).


## REAPER conventions

Confirmed from real project files:
- Mute envelope: 1 = unmuted, 0 = muted. Default mute envelopes are `PT 0 1 1`.
- Volume envelope points are amplitudes even with `VOLTYPE 1`; the values seen top out at exactly 2.0 (+6 dB).
- VST2 ids are written with upper-case hex (`1920167789<56535472...>`).
- Plug-in state round-trips byte-identical through export + import on three real projects (78 plug-ins).

Not yet confirmed in REAPER itself (check when touching these):
- What `TrackFX_GetParamIdent` returns for VST3/CLAP. Assumed: the decimal parameter id, possibly as `x:id`; `IdentMatches` accepts both.
- The `PARMENV` first field may be `index:ident` in REAPER-written files. The exporter uses the part before `:`.
- JSFX `PARMENV` values are in slider units.
- The bundled JSFX use `:log` slider ranges, which need REAPER 6.74+.
- Pan envelope values are inverted (+1 = left).
- VST plug-ins are written with an empty file name and rely on REAPER resolving them by id.
- Bitwig send levels get +12 dB scaling (taken from ProjectConverter's behaviour).


## Reference material

- DAWproject schema: https://github.com/bitwig/dawproject (`Project.xsd`, Java classes with Javadoc semantics)
- ProjectConverter (LGPL-3.0, Java) `format/reaper/ReaperCreator.java` is the closest existing DAWproject → RPP implementation; use it for reference only, don't copy code
- REAPER SDK `reaper_plugin.h` (`project_import_register_t`) is vendored in `third_party/reaper-sdk`
