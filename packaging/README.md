# DAWproject for REAPER

A REAPER extension that opens `.dawproject` files as REAPER projects and saves REAPER projects as `.dawproject`.

DAWproject is the open project exchange format supported by Bitwig Studio, Studio One, Cubase 14+, Cubasis, VST Live, n-Track and others.

Version {VERSION}, Windows (64-bit) and macOS (10.15 or later, Apple silicon and Intel), REAPER 6.74 or later (REAPER 7 recommended).

## Contents

- `reaper_dawproject-x64.dll` (Windows) or `reaper_dawproject.dylib` (macOS): the REAPER extension
- `Effects/DAWproject/*.jsfx`: the four effects that stand in for DAWproject's built-in devices (the extension installs them by itself; the copies are here for manual installs)
- `command-line/dawproject2rpp` and `command-line/rpp2dawproject` (`.exe` on Windows): the same conversions without REAPER (optional)
- `THIRD-PARTY-NOTICES.txt`

## Install

1. Close REAPER.
2. Copy the extension into REAPER's `UserPlugins` folder:
   - Windows: `reaper_dawproject-x64.dll` into `%APPDATA%\REAPER\UserPlugins`
   - macOS: `reaper_dawproject.dylib` into `~/Library/Application Support/REAPER/UserPlugins` (in Finder: *Go > Go to Folder...*)

   In REAPER, *Options > Show REAPER resource path in explorer/finder* opens the resource folder that contains `UserPlugins`. For a portable install, use the `UserPlugins` folder next to the REAPER program.
3. Start REAPER.

**macOS:** the extension is not notarized by Apple. If REAPER does not load it (the File menu entries are missing, or macOS says the file can't be verified), remove the download quarantine in Terminal and restart REAPER:

```
xattr -dr com.apple.quarantine ~/Library/Application\ Support/REAPER/UserPlugins/reaper_dawproject.dylib
```

On start, the extension writes four JSFX effects to `Effects/DAWproject/` in the resource folder. They stand in for the DAWproject built-in devices: equalizer, compressor, noise gate and limiter.

If the effects folder can't be written (for example a read-only resource folder), copy the `Effects/DAWproject` folder from this package into REAPER's `Effects` folder yourself.

To uninstall, delete the extension file. The `Effects/DAWproject` folder can be deleted too.

The extension can be installed together with *AAF for REAPER* (https://github.com/hermis501/reaper-aaf).

## Opening a DAWproject file

Use *File > Open project* and choose *DAWproject files* as the file type, or drag a `.dawproject` file onto REAPER. The project opens as an unsaved REAPER project. Save it as `.rpp` to keep it.

Audio embedded in the file is extracted to a folder named `<project name>-media` next to the `.dawproject` file. If that folder can't be written, it goes to the temp folder. Anything that could not be converted is listed in the REAPER console (ReaScript console window) after loading.

What is converted:

- **Tracks:** tracks, folder/group tracks, colors, volume, pan, mute, solo, channel count.
- **Routing:**
  - outputs to buses, as sends
  - sends, with pre/post, level, pan and enabled state
  - VCA channels, as REAPER VCA groups
- **Tempo and time signature:** including ramps and changes.
- **Audio clips:**
  - positions, offsets, fades, cross-fades, muted clips
  - warped clips: each constant-rate segment becomes one item with a play rate
  - looped clips, nested clips, linked/alias clips
- **MIDI clips:** notes, CC, pitch bend, pressure and program changes.
- **Automation:** volume, pan, mute, send and plug-in parameters.
- **Plug-ins:**
  - VST3, VST2 and CLAP plug-ins, with their saved state
  - the built-in devices, as the bundled JSFX
- **Project information:**
  - markers
  - title, artist, comment and other metadata, into the project notes and render metadata
  - clip launcher scenes: when the arrangement is empty, they are laid out one after another as regions

VST3 and CLAP parameter automation and VCA groups are applied right after the project has loaded. If a plug-in is not installed, the console says which automation could not be applied.

Not converted:

- Audio Unit plug-ins
- per-note expressions other than pressure
- sends to or from the master channel
- on/off automation of devices

## File menu

The extension adds **Export project to DAWproject...** to REAPER's *File* menu, next to *Export project MIDI...*. Opening DAWproject files is part of *File > Open project*.

The same command is in the Actions list as **DAWproject: Export project to DAWproject...** (command ID `_DAWPROJECT_EXPORT`). Use it there to assign a shortcut or a toolbar button. Scripts can call it with `reaper.Main_OnCommand(reaper.NamedCommandLookup("_DAWPROJECT_EXPORT"), 0)`.

If you have customized your File menu (*Options > Customize menus/toolbars*), REAPER keeps your layout and the entry is not added automatically. Add the action to the menu there, or reset the File menu to its default.

## Saving a REAPER project as DAWproject

Choose *File > Export project to DAWproject...*. A save dialog asks for the file name. When the export is done, the REAPER console shows the status: the saved file, the number of tracks, clips, plug-ins and media files, and anything that could not be exported. Your project and its file name are not changed.

What is exported:

- **Tracks and master:**
  - tracks, folders (as group tracks), colors, volume, pan, mute, solo, channel count
  - master volume, pan and FX
- **Routing:** parent/master outputs, sends and receives. See *Buses* below.
- **Tempo and markers:**
  - the tempo map with ramps, and time signature changes
  - markers; regions become markers at their start
- **Audio items:** position, length, offset, fades, mute, play rate, looping, sections, the active take. Audio files are embedded in the `.dawproject`.
- **MIDI items:** notes, CC, pitch bend, pressure, program changes, looping, pooled MIDI.
- **Automation:** track volume, pan and mute; send volume, pan and mute; plug-in parameters.
- **Plug-ins:**
  - VST3, VST2 and CLAP plug-ins with their state
  - the bundled JSFX, which become DAWproject built-in devices again
- **Project information:** notes and render metadata (title, artist, album and so on).

### Buses

DAWproject gives every track one main output, plus any number of sends. REAPER has no such distinction, so the export decides like this:

- **Folders:** a child track outputs to its folder, and the folder becomes a group track.
- **Bus by sends:** a track whose *Master/parent send* is off outputs to the track it sends to. That send must be post-fader, at 0 dB, centred, unmuted, without envelopes, and go from channels 1/2 into channels 1/2. The receiving track becomes a bus (submix channel), so its FX stay on it in the other DAW.
- **Several sends of that kind:** the output is the bus the other destinations lead to. For example, if a vocal sends to a doubler and to a vox bus, and the doubler also goes to the vox bus, the output is the vox bus. Otherwise it is the track that the most tracks output to. If vocals also send at 0 dB to a reverb or delay, the bus still wins: effect returns usually also get sends at other levels or from tracks that go to the master, and names like reverb, delay, room, hall or doubler count as effects. The other sends stay sends, and every track keeps its own FX. When the choice is unclear, the console says which track was taken.
- **Effect returns:** a track that only receives sends, and whose sources still go to the master, becomes an effect channel.
- **Multi-output instruments:** sends from channels 3/4 and up become ordinary sends, since DAWproject sends can't pick source channels.
- **No output:** a track with its master send off and no such send gets no main output. The console names it.

Not exported:

- item/take volume (DAWproject clips have no gain)
- take FX, reversed items, other takes
- other JSFX and other plug-in formats (AU, DX, LV2, video), FX containers
- pre-FX volume/pan and width envelopes, automation items, SysEx
- curved envelope shapes, which become linear

## Command-line tools (optional)

Open a command prompt (Windows) or Terminal (macOS) in the `command-line` folder. On macOS, start the tools with `./` (for example `./dawproject2rpp`); if macOS blocks them, run `xattr -dr com.apple.quarantine .` in that folder first.

```
dawproject2rpp song.dawproject [song.rpp] [--media <folder>]
rpp2dawproject song.rpp [song.dawproject] [--external-media]
```

`rpp2dawproject` embeds the audio files unless `--external-media` is given. With that option, files are referenced by their full path. Messages and warnings are printed to the console.

## Notes

- The built-in device JSFX reproduce the original settings, not the exact sound of the original DAW's devices.
- Plug-ins have to be installed in REAPER to load. Plug-in state is passed through unchanged.
- The macOS version is built and tested automatically (conversion tests and command-line tools), but it has not yet been tried inside REAPER on a Mac. Please report any problem.
