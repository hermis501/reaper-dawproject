"""Writes a REAPER project covering the exporter's features, exports it with rpp2dawproject,
checks the DAWproject, converts it back with dawproject2rpp and compares.

    python tests/test_export.py [build/Release folder]

Set DAWPROJECT_XSD to a folder holding Project.xsd and MetaData.xsd (from the bitwig/dawproject
repository) and JAVA to a java executable to also validate against the official schema.
"""

import base64
import os
import struct
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_converter import PROL_ID, children, close, first, make_wav, parse_rpp, value  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# build/Release of this folder's own build, else of the repository's top-level build
DEFAULT_BIN = next((d for d in (os.path.join(ROOT, "build", "Release"), os.path.join(os.path.dirname(ROOT), "build", "Release"))
                    if os.path.isdir(d)), os.path.join(ROOT, "build", "Release"))

failures = []


def check(cond, msg):
    if not cond:
        failures.append(msg)
        print("FAIL:", msg)


def b64lines(data):
    s = base64.b64encode(data).decode()
    return [s[i:i + 128] for i in range(0, len(s), 128)]


def vst_block(header_line, fx_id, magic, data):
    header = struct.pack("<II", fx_id, magic) + struct.pack("<I", 2) + struct.pack("<QQ", 1, 2) + struct.pack("<I", 2) + \
        struct.pack("<QQ", 1, 2) + struct.pack("<III", len(data), 1, 0)
    lines = [header_line] + b64lines(header) + b64lines(data) + b64lines(b"\0\0\x10\0\0\0") + [">"]
    return "\n".join(lines)


VST3_DATA = struct.pack("<II", 5, 1) + b"comp!" + struct.pack("<II", 4, 0) + b"cont"
VST2_CHUNK = b"reacomp-opaque-state"

MIDI_EVENTS = [
    # delta, status, d1, d2
    (0, 0x90, 60, 100), (0, 0xB0, 1, 64), (480, 0x80, 60, 0), (0, 0xE0, 0, 0x60),
    (480, 0x90, 64, 90), (0, 0xA0, 64, 30), (960, 0x80, 64, 10), (0, 0xC0, 5, 0),
]


def midi_source():
    lines = ["<SOURCE MIDI", "HASDATA 1 960 QN", "CCINTERP 32", "POOLEDEVTS {11111111-2222-3333-4444-555555555555}"]
    for d, s, a, b in MIDI_EVENTS:
        lines.append(f"E {d} {s:02x} {a:02x} {b:02x}")
    lines.append("E 1920 b0 7b 00")  # source length: 4 beats
    lines.append(">")
    return "\n".join(lines)


def build_rpp(wav):
    comp_js = "-18 4 5 100 0 0 1 " + " ".join(["-"] * 57)
    return f"""<REAPER_PROJECT 0.1 "7.22/win64" 1700000000
  <NOTES 0 2
    |first line
    |second line
  >
  TEMPO 120 4 4
  <RENDER_METADATA
    TAG ID3:TIT2 "Export Test"
    TAG ID3:TPE1 "Someone"
  >
  MASTER_VOLUME 0.5 0 -1 -1 1
  MASTER_NCH 2 2
  <MASTERFXLIST
    SHOW 0
    LASTSEL 0
    DOCKED 0
    BYPASS 0 0 0
{vst_block('<VST "VST3: Pro-L 2 (FabFilter)" "FabFilter Pro-L 2.vst3" 0 "" 1938458649{' + PROL_ID + '} ""', 1938458649, 0xFEED5EEE, VST3_DATA)}
    WAK 0 0
  >
  <TEMPOENVEX
    ACT 1 -1
    PT 0 120 1
    PT 8 120 0
    PT 15.5 140 1 196612
  >
  MARKER 1 0 Intro 0 16777471
  MARKER 2 4 "Verse A" 1 0
  MARKER 2 6 "" 1
  <TRACK
    NAME Drums
    PEAKCOL 16777471
    VOLPAN 1 0 -1 -1 1
    MUTESOLO 0 0 0
    ISBUS 1 1
    NCHAN 2
    MAINSEND 1 0
  >
  <TRACK
    NAME "Kick 'A'"
    VOLPAN 0.25 -0.5 -1 -1 1
    MUTESOLO 1 0 0
    ISBUS 2 -1
    NCHAN 2
    MAINSEND 1 0
    <VOLENV2
      ACT 1 -1
      PT 0 1 0
      PT 2 0.5 1
    >
    <PANENV2
      ACT 1 -1
      PT 0 1 1
    >
    <MUTEENV
      ACT 1 -1
      PT 0 1 1
      PT 3 0 1
    >
    <ITEM
      POSITION 1
      LENGTH 3
      LOOP 1
      FADEIN 1 0.1 0 1 0 0 0
      FADEOUT 1 0.2 0 1 0 0 0
      MUTE 0 0
      NAME kick-take
      SOFFS 0.5
      PLAYRATE 2 1 0 -1 0 0.0025
      <SOURCE WAVE
        FILE "{wav}"
      >
    >
    <ITEM
      POSITION 5
      LENGTH 1
      MUTE 1 0
      NAME other
      SOFFS 0
      PLAYRATE 1 1 0 -1 0 0.0025
      <SOURCE WAVE
        FILE "kick.wav"
      >
      TAKE SEL
      NAME second-take
      SOFFS 0.25
      <SOURCE WAVE
        FILE "kick.wav"
      >
    >
  >
  <TRACK
    NAME Synth
    VOLPAN 1 0 -1 -1 1
    ISBUS 0 0
    MAINSEND 0 0
    <FXCHAIN
      SHOW 0
      LASTSEL 0
      DOCKED 0
      BYPASS 0 0 0
      <CLAP "CLAPi: Surge XT (Surge Synth Team)" org.surge-synth-team.surge-xt ""
        <STATE
          {base64.b64encode(b"surge-state").decode()}
        >
      >
      FXID {{AAAAAAAA-0000-0000-0000-000000000001}}
      WAK 0 0
      BYPASS 1 0 0
{vst_block('<VST "VST: ReaComp (Cockos)" reacomp.dll 0 "" 1919250280<56535472636D7072656163636F6D7000> ""', 1919250280, 0xFEED5EEE, VST2_CHUNK)}
      <PARMENV 3:3 0 1 0.5
        ACT 1 -1
        PT 0 0.25 0
        PT 2 0.75 0
      >
      WAK 0 0
      BYPASS 0 0 0
      <JS DAWproject/dawproject_compressor.jsfx ""
        {comp_js}
      >
      <PARMENV 2 0.01 500 5
        ACT 1 -1
        PT 0 10 1
      >
      WAK 0 0
      BYPASS 0 0 0
      <JS utility/volume ""
        0 - - -
      >
      WAK 0 0
    >
    <ITEM
      POSITION 0
      LENGTH 4
      LOOP 1
      NAME notes
      <SOURCE MIDI
{midi_source()[13:]}
    >
    <ITEM
      POSITION 6
      LENGTH 2
      LOOP 0
      NAME pooled
      <SOURCE MIDIPOOL
        POOLEDEVTS {{11111111-2222-3333-4444-555555555555}}
      >
    >
  >
  <TRACK
    NAME Bus
    ISBUS 0 0
    MAINSEND 1 0
    AUXRECV 2 0 1 0 0 0 0 0 0 -1:U 0 -1 ''
  >
  <TRACK
    NAME Reverb
    ISBUS 0 0
    MAINSEND 1 0
    AUXRECV 1 3 0.5 0.25 0 0 0 0 0 -1:U 0 -1 ''
    <AUXVOLENV
      ACT 1 -1
      PT 0 0.5 0
      PT 4 1 0
    >
  >
>
"""


def run(cmd):
    res = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8")
    print(res.stderr.strip())
    check(res.returncode == 0, f"{os.path.basename(cmd[0])} failed")
    return res


def check_xml(path):
    z = zipfile.ZipFile(path)
    names = z.namelist()
    root = ET.fromstring(z.read("project.xml"))
    meta = ET.fromstring(z.read("metadata.xml"))
    check(meta.findtext("Title") == "Export Test" and meta.findtext("Artist") == "Someone", "metadata tags")
    check(meta.findtext("Comment") == "first line\nsecond line", f"notes as comment: {meta.findtext('Comment')!r}")
    check(root.find("Application").get("version") == "7.22", "application version")
    check(root.find("Transport/Tempo").get("value") == "120", "tempo")

    ids = {e.get("id"): e for e in root.iter() if e.get("id")}
    tracks = root.findall("Structure/Track")
    check([t.get("name") for t in tracks] == ["Master", "Drums", "Synth", "Bus", "Reverb"], f"top level {[t.get('name') for t in tracks]}")
    master, drums, synth, bus, reverb = tracks
    mch = master.find("Channel")
    check(mch.get("role") == "master" and mch.find("Volume").get("value") == "0.5", "master channel")
    pro = mch.find("Devices/Vst3Plugin")
    check(pro is not None and pro.get("deviceID") == PROL_ID and pro.get("deviceVendor") == "FabFilter", "VST3 device")
    preset = z.read(pro.find("State").get("path"))
    check(preset[:4] == b"VST3" and preset[8:40] == PROL_ID.encode() and b"comp!" in preset and b"cont" in preset, "vstpreset")

    check(drums.get("contentType").startswith("tracks") and drums.get("color") == "#ff0000", "folder track")
    kick = drums.find("Track")
    check(kick is not None and kick.get("name") == "Kick 'A'", "child track nested in folder")
    kch = kick.find("Channel")
    check(kch.get("destination") == drums.find("Channel").get("id"), "child routed to folder")
    check(close(kch.find("Pan").get("value"), 0.25) and kch.find("Mute").get("value") == "true", "pan/mute")
    check(drums.find("Channel").get("destination") == mch.get("id"), "folder to master")

    sch = synth.find("Channel")
    check(sch.get("destination") == bus.find("Channel").get("id"), "MAINSEND 0 + one plain receive -> destination")
    check(bus.find("Channel/Sends") is None and synth.find("Channel/Sends") is None, "consumed receive is not also a send")
    check(bus.find("Channel").get("role") == "submix", "a channel that tracks output to is a submix, not an effect channel")
    devs = list(sch.find("Devices"))
    check([d.tag for d in devs] == ["ClapPlugin", "Vst2Plugin", "Compressor"], f"synth devices {[d.tag for d in devs]}")
    clap, vst2, comp = devs
    check(z.read(clap.find("State").get("path")) == b"surge-state" and clap.get("deviceRole") == "instrument", "CLAP state")
    check(vst2.find("Enabled").get("value") == "false", "bypassed VST2")
    fxp = z.read(vst2.find("State").get("path"))
    check(fxp[:4] == b"CcnK" and fxp[8:12] == b"FPCh" and fxp[60:] == VST2_CHUNK, "fxp chunk")
    p3 = vst2.find("Parameters/RealParameter")
    check(p3 is not None and p3.get("parameterID") == "3", "automated VST2 parameter declared")
    check(close(comp.find("Threshold").get("value"), -18) and close(comp.find("Attack").get("value"), 0.005) and
          comp.find("AutoMakeup").get("value") == "true", "compressor from JSFX")

    rch = reverb.find("Channel")
    check(rch.get("role") == "effect", "receive-only track is an effect channel")
    send = kch.find("Sends/Send")
    check(send is not None and send.get("destination") == rch.get("id") and send.get("type") == "pre", "pre-fader send")
    check(close(send.find("Volume").get("value"), 0.5) and close(send.find("Pan").get("value"), 0.625), "send level/pan")
    order = [c.tag for c in kch]
    check(order == ["Mute", "Pan", "Sends", "Volume"], f"channel element order {order}")

    lanes = {l.get("track"): l for l in root.findall("Arrangement/Lanes/Lanes")}
    kl = lanes[kick.get("id")]
    clips = kl.findall("Clips/Clip")
    check(len(clips) == 2, "kick clips")
    c0 = clips[0]
    check(close(c0.get("time"), 2) and close(c0.get("duration"), 6), "clip position in beats")
    check(c0.get("contentTimeUnit") == "seconds" and close(c0.get("playStart"), 0.25), "playStart in clip time (rate 2)")
    check(close(c0.get("loopEnd"), 1.0) and close(c0.get("fadeInTime"), 0.1), f"loop and fades {c0.attrib}")
    warps = c0.find("Warps")
    check(warps is not None and close(warps.findall("Warp")[1].get("time"), 1.0) and
          close(warps.findall("Warp")[1].get("contentTime"), 2.0), "playrate 2 as warps")
    audio = warps.find("Audio")
    check(audio.get("sampleRate") == "44100" and audio.get("channels") == "1" and close(audio.get("duration"), 2.0), "audio info")
    check(audio.find("File").get("path") in names, "audio embedded")
    c1 = clips[1]
    check(c1.get("name") == "second-take" and c1.get("enable") == "false" and close(c1.get("playStart"), 0.25), "active take, muted")
    pts = {ids[p.find("Target").get("parameter")].tag: p for p in kl.findall("Points")}
    vp = pts["Volume"].findall("RealPoint")
    check(close(vp[1].get("time"), 4) and close(vp[1].get("value"), 0.5) and vp[1].get("interpolation") == "hold", "volume points")
    check(close(pts["Pan"].find("RealPoint").get("value"), 0.0), "pan envelope +1 = hard left = 0")
    mp = pts["Mute"].findall("BoolPoint")
    check([p.get("value") for p in mp] == ["false", "true"], "mute envelope")
    check("Volume" in pts and any(ids[p.find("Target").get("parameter")].tag == "Volume" and p is not pts["Volume"]
                                  for p in kl.findall("Points")), "send volume automation on the source track")

    sl = lanes[synth.get("id")]
    sclips = sl.findall("Clips/Clip")
    check(len(sclips) == 2, "MIDI clips")
    m0 = sclips[0]
    check(m0.get("contentTimeUnit") == "beats" and close(m0.get("loopEnd"), 4) and close(m0.get("duration"), 8),
          "MIDI item twice its source length loops")
    check(sclips[1].get("loopEnd") is None, "unlooped MIDI item")
    content = m0.find("Lanes")
    notes = content.find("Notes").findall("Note")
    check([(n.get("key"), n.get("time"), n.get("duration")) for n in notes] == [("60", "0", "0.5"), ("64", "1", "1")], "notes")
    check(close(notes[0].get("vel"), 100 / 127) and close(notes[1].get("rel"), 10 / 127), "velocities")
    exprs = {p.find("Target").get("expression"): p for p in content.findall("Points")}
    check(set(exprs) == {"channelController", "pitchBend", "polyPressure", "programChange"}, f"expressions {set(exprs)}")
    check(exprs["channelController"].find("Target").get("controller") == "1", "CC number")
    check(close(exprs["pitchBend"].find("RealPoint").get("value"), (0x60 << 7) / 16383), "pitch bend value")
    check(exprs["polyPressure"].find("Target").get("key") == "64", "poly pressure key")
    pooled = sclips[1].find("Lanes/Notes")
    check(pooled is not None and len(pooled) == 2, "pooled MIDI copy resolved")
    spts = [p for p in sl.findall("Points")]
    targets = [ids[p.find("Target").get("parameter")] for p in spts]
    check(any(t.get("parameterID") == "3" for t in targets), "VST2 parameter automation")
    att = [p for p, t in zip(spts, targets) if t.tag == "Attack"]
    check(att and att[0].get("unit") == "seconds" and close(att[0].find("RealPoint").get("value"), 0.01), "JSFX automation in seconds")

    markers = root.findall("Arrangement/Markers/Marker")
    check([m.get("name") for m in markers] == ["Intro", "Verse A"] and markers[0].get("color") == "#ff0000", "markers")
    check(close(markers[1].get("time"), 8), "marker time in beats")
    tempo = root.findall("Arrangement/TempoAutomation/RealPoint")
    check(len(tempo) == 3 and tempo[1].get("interpolation") == "linear" and close(tempo[1].get("time"), 16), "tempo ramp")
    # ramp 120 -> 140 over 7.5 s: beats = dsec*(t1-t0)/(60*ln(t1/t0))
    import math
    check(close(tempo[2].get("time"), 16 + 7.5 * 20 / (60 * math.log(140 / 120))), "tempo point after ramp")
    sig = root.findall("Arrangement/TimeSignatureAutomation/TimeSignaturePoint")
    check([(s.get("numerator"), s.get("denominator")) for s in sig] == [("4", "4"), ("4", "3")] or
          [(s.get("numerator"), s.get("denominator")) for s in sig] == [("4", "4"), ("3", "4")], f"signatures {[s.attrib for s in sig]}")
    return root


def check_roundtrip(rpp):
    proj = parse_rpp(rpp)
    tracks = children(proj, "TRACK")
    names = [value(t, "NAME") for t in tracks]
    check(names == ["Drums", "Kick 'A'", "Synth", "Bus", "Reverb"], f"round trip tracks {names}")
    by = dict(zip(names, tracks))
    check(first(by["Drums"], "ISBUS")[0][1:] == ["1", "1"] and first(by["Kick 'A'"], "ISBUS")[0][1:] == ["2", "-1"], "folders")
    check(close(value(by["Kick 'A'"], "VOLPAN"), 0.25) and close(value(by["Kick 'A'"], "VOLPAN", 2), -0.5), "vol/pan")
    items = children(by["Kick 'A'"], "ITEM")
    check(close(value(items[0], "POSITION"), 1) and close(value(items[0], "PLAYRATE"), 2) and close(value(items[0], "SOFFS"), 0.5),
          "audio item round trip")
    fx = first(by["Synth"], "FXCHAIN")
    check([c[0][0] for c in fx[1] if c[1] is not None and c[0][0] != "PARMENV"] == ["CLAP", "VST", "JS"], "FX chain round trip")
    check(value(by["Synth"], "MAINSEND") == "0", "synth output to bus")
    recv = children(by["Bus"], "AUXRECV")
    check(len(recv) == 1 and recv[0][0][1] == "2", "bus receives synth")
    tempo = [c[0] for c in first(proj, "TEMPOENVEX")[1] if c[0][0] == "PT"]
    check(len(tempo) == 3 and close(tempo[2][1], 15.5) and close(tempo[2][2], 140), f"tempo round trip {tempo}")


BUS_RPP = """<REAPER_PROJECT 0.1 "7.22/win64" 0
  TEMPO 120 4 4
  <TRACK
    NAME Vocal
    MAINSEND 0 0
  >
  <TRACK
    NAME Doubler
    MAINSEND 0 0
    AUXRECV 0 0 1 0 0 0 0 0 0 -1:U 0 -1 ''
  >
  <TRACK
    NAME Ducked
    AUXRECV 0 0 1 0 0 0 0 0 2 -1:U 0 -1 ''
  >
  <TRACK
    NAME "Vox Bus"
    AUXRECV 0 0 1 0 0 0 0 0 0 -1:U 0 -1 ''
    AUXRECV 1 0 1 0 0 0 0 0 0 -1:U 0 -1 ''
  >
  <TRACK
    NAME Plate
    AUXRECV 0 0 1 0 0 0 0 0 0 -1:U 0 -1 ''
  >
>
"""


TIE_RPP = """<REAPER_PROJECT 0.1 "7.22/win64" 0
  TEMPO 120 4 4
  <TRACK
    NAME Vocal1
    MAINSEND 0 0
  >
  <TRACK
    NAME Vocal2
    MAINSEND 0 0
  >
  <TRACK
    NAME Guitar
  >
  <TRACK
    NAME Space
    AUXRECV 0 0 1 0 0 0 0 0 0 -1:U 0 -1 ''
    AUXRECV 1 0 1 0 0 0 0 0 0 -1:U 0 -1 ''
    AUXRECV 2 0 0.5 0 0 0 0 0 0 -1:U 0 -1 ''
  >
  <TRACK
    NAME Delay
    AUXRECV 0 0 1 0 0 0 0 0 0 -1:U 0 -1 ''
    AUXRECV 1 0 1 0 0 0 0 0 0 -1:U 0 -1 ''
  >
  <TRACK
    NAME Group
    AUXRECV 0 0 1 0 0 0 0 0 0 -1:U 0 -1 ''
    AUXRECV 1 0 1 0 0 0 0 0 0 -1:U 0 -1 ''
  >
>
"""


def check_tie(path):
    # both vocals send at 0 dB to a reverb (which also gets the guitar), a delay and a bus, all
    # listed before the bus: the bus is still the output
    root = ET.fromstring(zipfile.ZipFile(path).read("project.xml"))
    ch = {t.get("name"): t.find("Channel") for t in root.iter("Track")}
    bus = ch["Group"].get("id")
    check(ch["Vocal1"].get("destination") == bus and ch["Vocal2"].get("destination") == bus, "0 dB sends to effects: bus wins")
    check([ch[n].get("role") for n in ("Group", "Space", "Delay")] == ["submix", "effect", "effect"], "tie roles")


def check_bus_choice(path):
    # Vocal (master send off) sends plainly to a doubler, a side-chain input, a vox bus and a reverb:
    # the vox bus (which the doubler also outputs to) is its output, the rest stay sends
    root = ET.fromstring(zipfile.ZipFile(path).read("project.xml"))
    ch = {t.get("name"): t.find("Channel") for t in root.iter("Track")}
    bus = ch["Vox Bus"].get("id")
    check(ch["Vocal"].get("destination") == bus and ch["Doubler"].get("destination") == bus, "output = the bus others lead to")
    check(ch["Vox Bus"].get("role") == "submix" and ch["Plate"].get("role") == "effect", "bus/effect roles")
    sends = {s.get("destination") for s in ch["Vocal"].iter("Send")}
    check(sends == {ch[n].get("id") for n in ("Doubler", "Ducked", "Plate")}, "other receives stay sends")


def validate(path):
    xsd, java = os.environ.get("DAWPROJECT_XSD"), os.environ.get("JAVA")
    if not xsd or not java:
        print("(schema validation skipped: set DAWPROJECT_XSD and JAVA)")
        return
    validator = os.path.join(os.path.dirname(os.path.abspath(__file__)), "Validate.java")
    res = subprocess.run([java, validator, os.path.join(xsd, "Project.xsd"), os.path.join(xsd, "MetaData.xsd"), path],
                         capture_output=True, text=True)
    print(res.stdout.strip())
    check(res.returncode == 0, "schema validation")


def main():
    bindir = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_BIN
    exe = lambda name: os.path.join(bindir, name + (".exe" if os.name == "nt" else ""))
    with tempfile.TemporaryDirectory() as tmp:
        wav = os.path.join(tmp, "kick.wav")
        make_wav(wav)
        rpp = os.path.join(tmp, "export test.rpp")
        with open(rpp, "w", encoding="utf-8") as f:
            f.write(build_rpp(wav))
        out = os.path.join(tmp, "export test.dawproject")
        run([exe("rpp2dawproject"), rpp, out])
        validate(out)
        check_xml(out)
        back = os.path.join(tmp, "back.rpp")
        run([exe("dawproject2rpp"), out, back, "--media", os.path.join(tmp, "media")])
        with open(back, encoding="utf-8") as f:
            check_roundtrip(f.read())
        bus_rpp = os.path.join(tmp, "buses.rpp")
        with open(bus_rpp, "w", encoding="utf-8") as f:
            f.write(BUS_RPP)
        bus_out = os.path.join(tmp, "buses.dawproject")
        run([exe("rpp2dawproject"), bus_rpp, bus_out])
        validate(bus_out)
        check_bus_choice(bus_out)
        tie_rpp = os.path.join(tmp, "tie.rpp")
        with open(tie_rpp, "w", encoding="utf-8") as f:
            f.write(TIE_RPP)
        tie_out = os.path.join(tmp, "tie.dawproject")
        run([exe("rpp2dawproject"), tie_rpp, tie_out])
        check_tie(tie_out)
    print(f"{len(failures)} failure(s)" if failures else "all checks passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
