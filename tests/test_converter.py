"""Generates DAWproject files covering the converter's features, converts them with
dawproject2rpp and checks the resulting RPP.

    python tests/test_converter.py [path/to/dawproject2rpp.exe]
"""

import base64
import math
import os
import struct
import subprocess
import sys
import tempfile
import wave
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# build/Release of this folder's own build, else of the repository's top-level build
DEFAULT_BIN = next((d for d in (os.path.join(ROOT, "build", "Release"), os.path.join(os.path.dirname(ROOT), "build", "Release"))
                    if os.path.isdir(d)), os.path.join(ROOT, "build", "Release"))
DEFAULT_EXE = os.path.join(DEFAULT_BIN, "dawproject2rpp.exe")

PROL_ID = "AFD92F729A0447B7B5E8D1D568DEA985"  # FabFilter Pro-L 2; REAPER hashes it to 1938458649


def make_wav(path, seconds=2.0, rate=44100):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        n = int(seconds * rate)
        w.writeframes(b"".join(struct.pack("<h", int(8000 * math.sin(i * 0.05))) for i in range(n)))


def make_vstpreset(class_id, comp, cont):
    header = b"VST3" + struct.pack("<i", 1) + class_id.encode() + struct.pack("<q", 48 + len(comp) + len(cont))
    entries = b"Comp" + struct.pack("<qq", 48, len(comp)) + b"Cont" + struct.pack("<qq", 48 + len(comp), len(cont))
    return header + comp + cont + b"List" + struct.pack("<i", 2) + entries


def make_fxp_chunk(fx_id, chunk):
    body = b"FPCh" + struct.pack(">iiii", 1, fx_id, 1, 1) + b"Prog".ljust(28, b"\0") + struct.pack(">i", len(chunk)) + chunk
    return b"CcnK" + struct.pack(">i", len(body)) + body


FEATURES_XML = """<?xml version="1.0" encoding="UTF-8"?>
<Project version="1.0">
  <Application name="Bitwig Studio" version="5.2"/>
  <Transport>
    <Tempo unit="bpm" value="120" min="20" max="666" id="tempo"/>
    <TimeSignature numerator="4" denominator="4" id="sig"/>
  </Transport>
  <Structure>
    <Track contentType="audio notes" id="master" name="Master">
      <Channel audioChannels="2" role="master" id="masterCh">
        <Devices>
          <Vst3Plugin deviceID="{PROL}" deviceName="Pro-L 2" deviceVendor="FabFilter" deviceRole="audioFX" loaded="true" id="d1">
            <Parameters><RealParameter parameterID="1234" unit="normalized" min="0" max="1" value="0" id="pl1"/></Parameters>
            <Enabled value="true" id="d1en"/>
            <State path="plugins/prol2.vstpreset"/>
          </Vst3Plugin>
        </Devices>
        <Pan unit="normalized" value="0.5" id="mPan"/>
        <Volume unit="linear" min="0" max="2" value="0.5" id="mVol"/>
      </Channel>
    </Track>
    <Track contentType="tracks" id="drums" name="Drums" color="#ff0000">
      <Channel audioChannels="2" role="submix" destination="masterCh" id="drumsCh">
        <Volume unit="linear" value="1" id="drumsVol"/>
        <Pan unit="normalized" value="0.5" id="drumsPan"/>
      </Channel>
      <Track contentType="audio" id="kick" name="Kick">
        <Channel audioChannels="1" role="regular" destination="drumsCh" id="kickCh">
          <Volume unit="linear" value="0.25" id="kickVol"/>
          <Pan unit="normalized" value="0.25" id="kickPan"/>
          <Mute value="false" id="kickMute"/>
          <Sends>
            <Send destination="fxCh" type="pre" id="s1">
              <Volume unit="linear" min="0" max="1" value="0.25" id="s1Vol"/>
              <Pan unit="normalized" value="0.5" id="s1Pan"/>
              <Enable value="true" id="s1En"/>
            </Send>
          </Sends>
        </Channel>
      </Track>
      <Track contentType="audio" id="snare" name="Snare 'quoted'">
        <Channel audioChannels="2" role="regular" destination="drumsCh" id="snareCh" solo="true">
          <Volume unit="decibel" value="-6.0206" id="snareVol"/>
        </Channel>
      </Track>
    </Track>
    <Track contentType="notes" id="synth" name="Synth">
      <Channel audioChannels="2" role="regular" destination="drumsCh" id="synthCh">
        <Devices>
          <ClapPlugin deviceID="org.surge-synth-team.surge-xt" deviceName="Surge XT" deviceVendor="Surge Synth Team" deviceRole="instrument" id="d2">
            <State path="plugins/surge.clap-preset"/>
          </ClapPlugin>
          <Vst2Plugin deviceID="1919250280" deviceName="ReaComp" deviceRole="audioFX" id="d3">
            <Parameters><RealParameter parameterID="3" unit="normalized" min="0" max="1" value="0.5" id="rc3"/></Parameters>
            <Enabled value="false" id="d3en"/>
            <State path="plugins/reacomp.fxp"/>
          </Vst2Plugin>
          <Equalizer deviceName="EQ" deviceRole="audioFX" id="d4">
            <Band type="highPass" order="4"><Freq unit="hertz" value="80" id="eqf1"/></Band>
            <Band type="bell"><Freq unit="hertz" value="1000" id="eqf2"/><Gain unit="decibel" value="-3" id="eqg2"/><Q unit="linear" value="2" id="eqq2"/></Band>
            <OutputGain unit="linear" value="0.5" id="eqout"/>
          </Equalizer>
          <Compressor deviceName="Comp" deviceRole="audioFX" id="d6">
            <Attack unit="seconds" value="0.005" id="ca"/>
            <Ratio unit="linear" value="4" id="cr"/>
            <Threshold unit="decibel" value="-18" id="ct"/>
          </Compressor>
          <Vst3Plugin deviceName="Broken" deviceRole="audioFX" id="d5"/>
        </Devices>
        <Volume unit="linear" value="1" id="synthVol"/>
        <Pan unit="normalized" value="0.5" id="synthPan"/>
      </Channel>
    </Track>
    <Track contentType="audio" id="fx" name="Reverb">
      <Channel audioChannels="2" role="effect" destination="masterCh" id="fxCh">
        <Volume unit="linear" value="1" id="fxVol"/>
      </Channel>
    </Track>
    <Track contentType="notes" id="warped" name="Warped MIDI">
      <Channel audioChannels="2" role="regular" destination="vcaCh" id="warpedCh"/>
    </Track>
    <Track contentType="automation" id="vca" name="VCA 1">
      <Channel role="vca" id="vcaCh"><Volume unit="linear" value="1" id="vcaVol"/></Channel>
    </Track>
  </Structure>
  <Arrangement id="arr">
    <Lanes timeUnit="beats" id="root">
      <Lanes track="kick" id="kickLanes">
        <Clips id="kc">
          <!-- Bitwig style: an arrangement clip containing a clip timeline, looped every 4 beats -->
          <Clip time="0" duration="16" playStart="0" loopStart="0" loopEnd="4" name="Kick loop">
            <Clips id="kci">
              <Clip time="0" duration="4" contentTimeUnit="beats" playStart="0">
                <Warps contentTimeUnit="seconds" timeUnit="beats" id="kw">
                  <Audio channels="1" duration="2.0" sampleRate="44100" id="ka"><File path="audio/kick.wav"/></Audio>
                  <Warp time="0" contentTime="0"/>
                  <Warp time="4" contentTime="2.0"/>
                </Warps>
              </Clip>
            </Clips>
          </Clip>
        </Clips>
        <Points id="kmute">
          <Target parameter="kickMute"/>
          <BoolPoint time="0" value="false"/>
          <BoolPoint time="8" value="true"/>
        </Points>
        <Points id="ksend" unit="linear">
          <Target parameter="s1Vol"/>
          <RealPoint time="0" value="0.25" interpolation="linear"/>
          <RealPoint time="4" value="0" interpolation="hold"/>
        </Points>
      </Lanes>
      <Lanes track="snare" id="snareLanes">
        <Clips id="sc">
          <Clip time="4" duration="2" contentTimeUnit="seconds" playStart="0.5" fadeTimeUnit="seconds" fadeInTime="-0.25" fadeOutTime="0.1" name="Snare">
            <Audio channels="1" duration="2.0" sampleRate="44100" id="sa"><File path="audio/kick.wav"/></Audio>
          </Clip>
          <Clip time="12" duration="2" enable="false" name="Muted">
            <Audio channels="1" duration="2.0" sampleRate="44100" algorithm="repitch" id="sa2"><File path="audio/kick.wav"/></Audio>
          </Clip>
        </Clips>
      </Lanes>
      <Lanes track="synth" id="synthLanes">
        <Clips id="nc">
          <Clip time="0" duration="8" playStart="0" loopStart="0" loopEnd="2" name="Arp">
            <Lanes id="nl">
              <Notes id="notes">
                <Note time="0" duration="0.5" channel="0" key="60" vel="1.0" rel="0.5">
                  <Points unit="normalized" id="np"><Target expression="pressure"/>
                    <RealPoint time="0" value="0" interpolation="hold"/><RealPoint time="0.25" value="1" interpolation="hold"/></Points>
                </Note>
                <Note time="1" duration="2" channel="1" key="64" vel="0.5"/>
                <Note time="2.5" duration="0.5" channel="0" key="99" vel="0.5"/>
              </Notes>
              <Points id="cc" unit="normalized">
                <Target expression="channelController" controller="74" channel="0"/>
                <RealPoint time="0" value="0" interpolation="hold"/>
                <RealPoint time="1" value="1" interpolation="hold"/>
              </Points>
            </Lanes>
          </Clip>
        </Clips>
        <Points id="svol" unit="linear">
          <Target parameter="synthVol"/>
          <RealPoint time="0" value="1" interpolation="linear"/>
          <RealPoint time="16" value="0.5" interpolation="linear"/>
        </Points>
        <Points id="span" unit="normalized">
          <Target parameter="synthPan"/>
          <RealPoint time="0" value="0" interpolation="hold"/>
        </Points>
        <Points id="dev" unit="normalized">
          <Target parameter="d3en"/>
          <BoolPoint time="0" value="true"/>
        </Points>
        <Points id="rcauto" unit="normalized">
          <Target parameter="rc3"/>
          <RealPoint time="0" value="0.25" interpolation="linear"/>
          <RealPoint time="4" value="0.75" interpolation="linear"/>
        </Points>
        <Points id="eqauto" unit="decibel">
          <Target parameter="eqg2"/>
          <RealPoint time="0" value="-3" interpolation="linear"/>
          <RealPoint time="4" value="6" interpolation="hold"/>
        </Points>
        <Points id="cauto" unit="seconds">
          <Target parameter="ca"/>
          <RealPoint time="0" value="0.01" interpolation="hold"/>
        </Points>
      </Lanes>
      <Lanes track="warped" id="warpedLanes">
        <Clips id="wc">
          <Clip time="8" duration="4" name="Fast">
            <Warps contentTimeUnit="beats" id="ww">
              <Notes id="wn">
                <Note time="0" duration="1" channel="0" key="48" vel="0.8"/>
                <Note time="1" duration="1" channel="0" key="50" vel="0.8"/>
              </Notes>
              <Warp time="0" contentTime="0"/>
              <Warp time="4" contentTime="2"/>
            </Warps>
          </Clip>
        </Clips>
      </Lanes>
      <Lanes track="master" id="masterLanes">
        <Points id="plauto" unit="normalized">
          <Target parameter="pl1"/>
          <RealPoint time="0" value="0" interpolation="linear"/>
          <RealPoint time="8" value="1" interpolation="linear"/>
        </Points>
      </Lanes>
    </Lanes>
    <Markers id="mk">
      <Marker time="0" name="Intro" color="#00ff00"/>
      <Marker time="32" name="Verse"/>
    </Markers>
    <TempoAutomation id="ta" unit="bpm">
      <Target parameter="tempo"/>
      <RealPoint time="0" value="120" interpolation="hold"/>
      <RealPoint time="16" value="120" interpolation="linear"/>
      <RealPoint time="32" value="140" interpolation="hold"/>
    </TempoAutomation>
    <TimeSignatureAutomation id="tsa">
      <Target parameter="sig"/>
      <TimeSignaturePoint time="0" numerator="4" denominator="4"/>
      <TimeSignaturePoint time="40" numerator="3" denominator="4"/>
    </TimeSignatureAutomation>
  </Arrangement>
  <Scenes/>
</Project>
""".replace("{PROL}", PROL_ID)

METADATA_XML = """<?xml version="1.0" encoding="UTF-8"?>
<MetaData><Title>Feature Test</Title><Artist>Tester</Artist><Comment>line one
line two</Comment></MetaData>
"""

SCENES_XML = """<?xml version="1.0" encoding="UTF-8"?>
<Project version="1.0">
  <Application name="Test" version="1"/>
  <Transport><Tempo unit="bpm" value="60" id="t"/></Transport>
  <Structure>
    <Channel role="master" id="m" name="Master"/>
    <Track contentType="notes" id="a" name="A"><Channel role="regular" destination="m" id="ac"/></Track>
  </Structure>
  <Arrangement id="arr"><Lanes timeUnit="beats" id="l"/></Arrangement>
  <Scenes>
    <Scene name="One" id="s1">
      <Lanes id="s1l">
        <ClipSlot track="a" id="cs1">
          <Clip time="0" duration="4" loopStart="0" loopEnd="1">
            <Notes id="n1"><Note time="0" duration="0.5" channel="0" key="60" vel="0.8"/></Notes>
          </Clip>
        </ClipSlot>
      </Lanes>
    </Scene>
    <Scene name="Two" id="s2">
      <Lanes id="s2l">
        <ClipSlot track="a" id="cs2">
          <Clip time="0" duration="2"><Notes id="n2"><Note time="1" duration="1" channel="0" key="62" vel="0.8"/></Notes></Clip>
        </ClipSlot>
      </Lanes>
    </Scene>
  </Scenes>
</Project>
"""


# --- RPP parsing ---------------------------------------------------------------------------

def parse_rpp(text):
    """Returns a tree of (tokens, children) nodes."""
    root = (["ROOT"], [])
    stack = [root]
    for raw in text.splitlines():
        line = raw.strip()
        if not line:
            continue
        if line == ">":
            stack.pop()
            continue
        if line.startswith("<"):
            node = (tokenize(line[1:]), [])
            stack[-1][1].append(node)
            stack.append(node)
        else:
            stack[-1][1].append((tokenize(line), None))
    assert len(stack) == 1, "unbalanced RPP blocks"
    return root[1][0]


def tokenize(line):
    out, i = [], 0
    while i < len(line):
        c = line[i]
        if c == " ":
            i += 1
        elif c in "\"'`":
            j = line.index(c, i + 1)
            out.append(line[i + 1:j])
            i = j + 1
        else:
            j = line.find(" ", i)
            j = len(line) if j < 0 else j
            out.append(line[i:j])
            i = j
    return out


def children(node, name):
    return [c for c in node[1] if c[0][0] == name]


def first(node, name):
    found = children(node, name)
    assert found, f"{name} not found in {node[0]}"
    return found[0]


def value(node, name, index=1):
    return first(node, name)[0][index]


def close(a, b, eps=1e-4):
    return abs(float(a) - float(b)) < eps


# --- checks --------------------------------------------------------------------------------

failures = []


def check(cond, msg):
    if not cond:
        failures.append(msg)
        print("FAIL:", msg)


def beats_to_sec(b):
    # tempo map of FEATURES_XML: 120 until beat 16, linear ramp to 140 at beat 32, then 140
    if b <= 16:
        return b * 0.5
    if b <= 32:
        k = (140 - 120) / 16
        return 8 + 60 / k * math.log((120 + k * (b - 16)) / 120)
    return beats_to_sec(32) + (b - 32) * 60 / 140


def check_features(proj, media_dir):
    check(value(proj, "TEMPO") == "120", "base tempo")
    tempo = first(proj, "TEMPOENVEX")
    pts = [c[0] for c in tempo[1] if c[0][0] == "PT"]
    check(len(pts) == 4, f"tempo points: {pts}")
    check(close(pts[1][1], 8) and pts[1][3] == "0", "ramp starts at beat 16 = 8 s, linear")
    check(close(pts[2][1], beats_to_sec(32)) and close(pts[2][2], 140), "ramp end position")
    check(close(pts[3][1], beats_to_sec(40)) and pts[3][4] == str((4 << 16) | 3), "3/4 at beat 40")

    markers = children(proj, "MARKER")
    check(len(markers) == 2 and markers[0][0][3] == "Intro", "markers")
    check(markers[0][0][5] == str(0x00ff00 | 0x1000000), "marker color (green)")
    check(close(markers[1][0][2], beats_to_sec(32)), "marker after tempo ramp")

    check(close(value(proj, "MASTER_VOLUME"), 0.5), "master volume")
    mfx = first(proj, "MASTERFXLIST")
    vst = first(mfx, "VST")
    check(vst[0][5] == "1938458649{" + PROL_ID + "}", f"VST3 id: {vst[0][5]}")
    lines = [c[0][0] for c in vst[1]]
    data = base64.b64decode("".join(lines[1:-1]))
    comp = bytes(range(100))
    check(data == struct.pack("<ii", len(comp), 1) + comp + struct.pack("<ii", 4, 0) + b"ctrl", "VST3 state data block")
    header = base64.b64decode(lines[0])
    check(struct.unpack("<I", header[:4])[0] == 1938458649 and header[4:8] == bytes.fromhex("ee5eedfe"), "VST3 header")
    check(struct.unpack("<I", header[-12:-8])[0] == len(data), "VST3 header data size")

    tracks = children(proj, "TRACK")
    names = [value(t, "NAME") for t in tracks]
    check(names == ["Drums", "Kick", "Snare 'quoted'", "Synth", "Reverb", "Warped MIDI", "VCA 1"], f"track order {names}")
    by = dict(zip(names, tracks))
    check(first(by["Drums"], "ISBUS")[0][1:] == ["1", "1"], "folder start")
    check(first(by["Snare 'quoted'"], "ISBUS")[0][1:] == ["2", "-1"], "folder end")
    check(value(by["Drums"], "PEAKCOL") == str(0xff | 0x1000000), "track color red")
    check(close(value(by["Kick"], "VOLPAN"), 0.25) and close(value(by["Kick"], "VOLPAN", 2), -0.5), "kick vol/pan")
    check(close(value(by["Snare 'quoted'"], "VOLPAN"), 0.5), "decibel volume")
    check(value(by["Snare 'quoted'"], "MUTESOLO", 2) == "2", "solo")
    check(value(by["Synth"], "MAINSEND") == "0", "synth routed to non-parent bus")
    recv = [c[0] for c in by["Drums"][1] if c[0][0] == "AUXRECV"]
    check(len(recv) == 1 and recv[0][1] == "3", f"drums receives synth: {recv}")

    rev = by["Reverb"]
    recv = [c[0] for c in rev[1] if c[0][0] == "AUXRECV"]
    check(len(recv) == 1 and recv[0][1] == "1" and recv[0][2] == "3", "pre-fader send kick->reverb")
    check(close(recv[0][3], 0.25 * 10 ** (12 / 20)), "Bitwig send scaling +12 dB")
    auxenv = first(rev, "AUXVOLENV")
    check(len([c for c in auxenv[1] if c[0][0] == "PT"]) == 2, "send automation")

    kick_items = children(by["Kick"], "ITEM")
    check(len(kick_items) == 4, f"looped nested clip -> 4 items, got {len(kick_items)}")
    for i, it in enumerate(kick_items):
        check(close(value(it, "POSITION"), i * 2.0) and close(value(it, "LENGTH"), 2.0), f"kick item {i} position")
        check(close(value(it, "PLAYRATE"), 1.0), "kick warp rate")
        src = first(it, "SOURCE")
        f = value(src, "FILE")
        check(os.path.isfile(f) and f.startswith(media_dir), f"extracted media {f}")
    mute = first(by["Kick"], "MUTEENV")
    mpts = [c[0] for c in mute[1] if c[0][0] == "PT"]
    check(mpts[0][2] == "1" and mpts[1][2] == "0" and close(mpts[1][1], 4), "mute envelope (1 = unmuted)")

    snare = children(by["Snare 'quoted'"], "ITEM")
    check(len(snare) == 2, "snare items")
    s0 = snare[0]
    check(close(value(s0, "POSITION"), 1.75) and close(value(s0, "LENGTH"), 1.25), "crossfade moves start earlier")
    check(close(value(s0, "SOFFS"), 0.25) and close(value(s0, "FADEIN", 2), 0.25), "crossfade offset and fade")
    check(close(value(s0, "FADEOUT", 2), 0.1), "fade out")
    check(value(snare[1], "MUTE") == "1" and value(snare[1], "PLAYRATE", 2) == "0", "muted, repitch")

    synth = by["Synth"]
    fx = first(synth, "FXCHAIN")
    clap = first(fx, "CLAP")
    check(clap[0][1] == "CLAPi: Surge XT (Surge Synth Team)" and clap[0][2] == "org.surge-synth-team.surge-xt", "CLAP")
    check(base64.b64decode("".join(c[0][0] for c in first(clap, "STATE")[1])) == b"surge-state", "CLAP state")
    vst2 = first(fx, "VST")
    check(vst2[0][5].startswith("1919250280<56535472"), f"VST2 id {vst2[0][5]}")
    v2data = base64.b64decode(vst2[1][1][0][0])
    check(v2data == b"opaque-chunk", "VST2 chunk data")
    byp = [c[0] for c in fx[1] if c[0][0] == "BYPASS"]
    check(byp[1][1] == "1", "disabled VST2 bypassed")
    check(len(children(fx, "VST")) + len(children(fx, "CLAP")) == 2, "id-less plug-in skipped")
    js = children(fx, "JS")
    check([j[0][1] for j in js] == ["DAWproject/dawproject_eq.jsfx", "DAWproject/dawproject_compressor.jsfx"], f"built-ins as JSFX {js}")
    eq = [float(x) if x != "-" else None for x in js[0][1][0][0]]
    check(eq[0] == 0 and close(eq[1], -6.0206), f"EQ gains {eq[:2]}")
    check(eq[2:8] == [1, 4, 80, 0, 0.707, 4], f"EQ band 1 high pass order 4: {eq[2:8]}")
    check(eq[8:14] == [1, 0, 1000, -3, 2, 2], f"EQ band 2 bell: {eq[8:14]}")
    check(eq[14] == 0 and len(eq) == 64 and eq[50] is None, "unused bands off, padded to 64")
    comp = [float(x) for x in js[1][1][0][0][:7]]
    check(comp == [-18, 4, 5, 100, 0, 0, 0], f"compressor sliders {comp}")
    parmenv = {}
    for c in fx[1]:
        if c[0][0] == "PARMENV":
            parmenv[c[0][1]] = (c[0], [x[0] for x in c[1] if x[0][0] == "PT"])
    check("3" in parmenv and close(parmenv["3"][1][0][2], 0.25) and close(parmenv["3"][1][1][2], 0.75), f"VST2 param automation {parmenv.get('3')}")
    check("11" in parmenv and parmenv["11"][0][2:4] == ["-24", "24"] and close(parmenv["11"][1][1][2], 6), "EQ band gain automation (slider 12)")
    check("2" in parmenv and close(parmenv["2"][1][0][2], 10), "compressor attack automation in ms")
    order = [c[0][0] for c in fx[1]]
    check(order.index("PARMENV") > order.index("VST") and order[-1] == "WAK", "PARMENV inside the FX entry")

    items = children(synth, "ITEM")
    check(len(items) == 1 and close(value(items[0], "LENGTH"), 4.0), "looped MIDI clip is one item")
    events = [c[0] for c in first(items[0], "SOURCE")[1] if c[0][0] == "E"]
    ticks, t = [], 0
    for e in events:
        t += int(e[1])
        ticks.append((t, e[2], e[3], e[4]))
    note_ons = [x for x in ticks if x[1].startswith("9")]
    check(len(note_ons) == 8, f"4 loop repeats x 2 notes, got {len(note_ons)}")
    check(note_ons[0] == (0, "90", "3c", "7f"), f"first note {note_ons[0]}")
    check(any(x[1] == "91" and x[2] == "40" for x in note_ons), "channel 2 note")
    offs_64 = [x for x in ticks if x[1] == "81" and x[2] == "40"]
    check(offs_64 and offs_64[0][0] == 1920, "note crossing loop end is cut at the loop end")
    ccs = [x for x in ticks if x[1] == "b0" and x[2] == "4a"]
    check(len(ccs) == 8 and ccs[1] == (960, "b0", "4a", "7f"), f"CC74 per loop: {ccs[:3]}")
    check(ticks[-1][0] == 8 * 960, "MIDI source length 8 beats")
    pressure = [x for x in ticks if x[1] == "a0"]
    check(len(pressure) == 8 and pressure[0][2] == "3c" and pressure[1][3] == "7f", f"per-note pressure -> poly aftertouch {pressure[:2]}")
    check(pressure[1][0] - pressure[0][0] == 240, "pressure timed from the note start")
    venv = first(synth, "VOLENV2")
    vpts = [c[0] for c in venv[1] if c[0][0] == "PT"]
    check(close(vpts[1][1], 8) and close(vpts[1][2], 0.5), "volume envelope")
    penv = first(synth, "PANENV2")
    check(close([c[0] for c in penv[1] if c[0][0] == "PT"][0][2], 1.0), "pan envelope hard left = +1")

    warped = by["Warped MIDI"]
    witems = children(warped, "ITEM")
    check(len(witems) == 1 and close(value(witems[0], "POSITION"), 4.0), "warped MIDI item")
    wev = [c[0] for c in first(witems[0], "SOURCE")[1] if c[0][0] == "E"]
    t, won = 0, []
    for e in wev:
        t += int(e[1])
        if e[2] == "90": won.append((t, e[3]))
    check(won == [(0, "30"), (1920, "32")], f"warped MIDI notes doubled in time: {won}")
    check(value(warped, "MAINSEND") == "1" and not children(by["VCA 1"], "AUXRECV"), "VCA member keeps its output")

    notes = first(proj, "NOTES")
    check([c[0][0] for c in notes[1]] == ["|line", "|line"] or len(notes[1]) == 2, "notes")
    meta = first(proj, "RENDER_METADATA")
    check(any(c[0][1] == "ID3:TIT2" and c[0][2] == "Feature Test" for c in meta[1]), "title metadata")


def check_scenes(proj):
    tracks = children(proj, "TRACK")
    items = children(tracks[0], "ITEM")
    check(len(items) == 2, f"scene items {len(items)}")
    check(close(value(items[0], "LENGTH"), 4) and close(value(items[1], "POSITION"), 4), "scenes laid out in sequence")
    regions = [m[0] for m in children(proj, "MARKER")]
    check(len(regions) == 4 and regions[0][3] == "One" and regions[0][4] == "1", f"scene regions {regions}")


def convert(exe, src, dst, media):
    res = subprocess.run([exe, src, dst, "--media", media], capture_output=True, text=True, encoding="utf-8")
    print(res.stderr.strip())
    check(res.returncode == 0, f"conversion of {os.path.basename(src)} failed")
    with open(dst, encoding="utf-8") as f:
        return parse_rpp(f.read())


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_EXE
    with tempfile.TemporaryDirectory() as tmp:
        wav = os.path.join(tmp, "kick.wav")
        make_wav(wav)
        features = os.path.join(tmp, "features.dawproject")
        with zipfile.ZipFile(features, "w") as z:
            z.writestr("project.xml", FEATURES_XML)
            z.writestr("metadata.xml", METADATA_XML)
            z.write(wav, "audio/kick.wav")
            z.writestr("plugins/prol2.vstpreset", make_vstpreset(PROL_ID, bytes(range(100)), b"ctrl"))
            z.writestr("plugins/surge.clap-preset", b"surge-state")
            z.writestr("plugins/reacomp.fxp", make_fxp_chunk(1919250280, b"opaque-chunk"))
        scenes = os.path.join(tmp, "scenes.dawproject")
        with zipfile.ZipFile(scenes, "w") as z:
            z.writestr("project.xml", SCENES_XML)

        media = os.path.join(tmp, "media")
        check_features(convert(exe, features, os.path.join(tmp, "features.rpp"), media), media)
        check_scenes(convert(exe, scenes, os.path.join(tmp, "scenes.rpp"), media))

    print(f"{len(failures)} failure(s)" if failures else "all checks passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
