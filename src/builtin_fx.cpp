#include "builtin_fx.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "builtin_jsfx_data.h"  // generated from jsfx/*.jsfx by CMake
#include "util.h"

namespace dawp {
namespace {

enum Conversion { kDb, kMs, kHz, kLinear, kRatio, kRange, kBool, kEnum, kOrder };

struct SliderSpec
{
  const char* element;  // DAWproject element name (band elements for the EQ)
  Conversion conversion;
  double min, max, def;
};

// Slider order matches the JSFX files.
const SliderSpec kCompressor[] = {
    {"Threshold", kDb, -60, 0, -20},   {"Ratio", kRatio, 1, 100, 4},      {"Attack", kMs, 0.01, 500, 10},
    {"Release", kMs, 1, 5000, 100},    {"InputGain", kDb, -24, 24, 0},    {"OutputGain", kDb, -24, 24, 0},
    {"AutoMakeup", kBool, 0, 1, 0}};
const SliderSpec kGate[] = {{"Threshold", kDb, -80, 0, -40},
                            {"Range", kRange, -100, 0, -80},
                            {"Ratio", kRatio, 1, 100, 10},
                            {"Attack", kMs, 0.01, 500, 1},
                            {"Release", kMs, 1, 5000, 100}};
const SliderSpec kLimiter[] = {{"Threshold", kDb, -30, 0, -1},
                               {"InputGain", kDb, -24, 24, 0},
                               {"OutputGain", kDb, -24, 24, 0},
                               {"Attack", kMs, 0.01, 100, 1},
                               {"Release", kMs, 1, 2000, 50}};
// Element order required by the DAWproject schema (alphabetical), as slider indices.
const int kCompressorOrder[] = {2, 6, 4, 5, 1, 3, 0};
const int kGateOrder[] = {3, 1, 2, 4, 0};
const int kLimiterOrder[] = {3, 1, 2, 4, 0};

const int kEqBands = 8;
const int kEqSliders = 2 + kEqBands * 6;
const SliderSpec kEqGain[] = {{"InputGain", kDb, -24, 24, 0}, {"OutputGain", kDb, -24, 24, 0}};
const SliderSpec kEqBand[] = {{"Enabled", kBool, 0, 1, 0},  {"type", kEnum, 0, 6, 0},       {"Freq", kHz, 20, 20000, 1000},
                              {"Gain", kDb, -24, 24, 0},     {"Q", kLinear, 0.1, 18, 0.707}, {"order", kOrder, 1, 8, 2}};
const double kEqDefaultFreq[kEqBands] = {60, 150, 400, 1000, 2500, 6000, 12000, 16000};
const char* kEqTypes[] = {"bell", "lowShelf", "highShelf", "lowPass", "highPass", "bandPass", "notch"};

const SliderSpec* Specs(BuiltinKind kind, int& count)
{
  switch (kind)
  {
    case BuiltinKind::Compressor: count = 7; return kCompressor;
    case BuiltinKind::NoiseGate: count = 5; return kGate;
    case BuiltinKind::Limiter: count = 5; return kLimiter;
    default: count = 0; return nullptr;
  }
}

const SliderSpec& Spec(BuiltinKind kind, int slider)
{
  if (kind == BuiltinKind::Equalizer)
    return slider < 2 ? kEqGain[slider] : kEqBand[(slider - 2) % 6];
  int count;
  return Specs(kind, count)[slider];
}

bool GetNum(pugi::xml_node n, const char* name, double& out)
{
  pugi::xml_attribute a = n.attribute(name);
  return a && ParseDouble(a.value(), out);
}

// A DAWproject value (in 'unit') to the slider's unit.
double ToSlider(const SliderSpec& spec, pugi::xml_node param, double v, std::string unit)
{
  std::string paramUnit = param.attribute("unit").value();
  if (unit.empty()) unit = paramUnit;
  if (spec.conversion == kBool || strcmp(param.name(), "BoolParameter") == 0 || strcmp(param.name(), "Enabled") == 0 ||
      strcmp(param.name(), "AutoMakeup") == 0)
    return v > 0.5 ? 1 : 0;

  double mn, mx;
  if ((unit == "normalized" || unit == "percent") && paramUnit != unit && GetNum(param, "min", mn) && GetNum(param, "max", mx))
  {
    if (unit == "percent") v /= 100.0;
    v = mn + v * (mx - mn);
    unit = paramUnit;
  }
  if (unit == "normalized" || (unit == "percent" && spec.conversion != kRatio))
  {
    if (unit == "percent") v /= 100.0;
    v = spec.min + v * (spec.max - spec.min);
  }
  else
  {
    switch (spec.conversion)
    {
      case kDb:
        if (unit == "linear") v = 20.0 * std::log10(std::max(v, 1e-10));
        break;
      case kRange:
        if (unit == "linear") v = 20.0 * std::log10(std::max(v, 1e-10));
        if (v > 0) v = -v;  // a positive range means the amount of attenuation
        break;
      case kMs:
        if (unit != "linear") v *= 1000.0;  // seconds unless stated otherwise
        break;
      case kRatio:
        if (unit == "percent") v = 1.0 / std::max(0.0001, 1.0 - v / 100.0);  // 100 % = infinite ratio
        break;
      default: break;
    }
  }
  return std::max(spec.min, std::min(spec.max, v));
}

void ReadParam(BuiltinDevice& out, pugi::xml_node param, int slider)
{
  if (!param) return;
  const SliderSpec& spec = Spec(out.kind, slider);
  double v;
  if (spec.conversion == kBool)
  {
    pugi::xml_attribute a = param.attribute("value");
    if (a) out.sliders[slider] = (strcmp(a.value(), "true") == 0 || strcmp(a.value(), "1") == 0) ? 1 : 0;
  }
  else if (GetNum(param, "value", v))
    out.sliders[slider] = ToSlider(spec, param, v, std::string());
  if (param.attribute("id")) out.params.push_back({param, slider, (int)spec.conversion});
}

}  // namespace

const std::vector<JsfxFile>& BundledJsfx()
{
  static const std::vector<JsfxFile> files(std::begin(kBundledJsfx), std::end(kBundledJsfx));
  return files;
}

std::string JsfxPath(BuiltinKind kind)
{
  switch (kind)
  {
    case BuiltinKind::Equalizer: return "DAWproject/dawproject_eq.jsfx";
    case BuiltinKind::Compressor: return "DAWproject/dawproject_compressor.jsfx";
    case BuiltinKind::NoiseGate: return "DAWproject/dawproject_gate.jsfx";
    case BuiltinKind::Limiter: return "DAWproject/dawproject_limiter.jsfx";
    default: return std::string();
  }
}

BuiltinKind JsfxKind(const std::string& path)
{
  std::string p = ToLower(path);
  for (char& c : p)
    if (c == '\\') c = '/';
  for (BuiltinKind k : {BuiltinKind::Equalizer, BuiltinKind::Compressor, BuiltinKind::NoiseGate, BuiltinKind::Limiter})
  {
    std::string ours = ToLower(JsfxPath(k));
    std::string bare = ours.substr(0, ours.size() - 5);  // REAPER may drop the extension
    if (p == ours || p == bare) return k;
  }
  return BuiltinKind::None;
}

int JsfxSliderCount(BuiltinKind kind)
{
  if (kind == BuiltinKind::Equalizer) return kEqSliders;
  int count;
  Specs(kind, count);
  return count;
}

void JsfxSliderRange(BuiltinKind kind, int slider, double& min, double& max)
{
  const SliderSpec& s = Spec(kind, slider);
  min = s.min;
  max = s.max;
}

bool ReadBuiltinDevice(pugi::xml_node device, BuiltinDevice& out, std::vector<std::string>& warnings)
{
  std::string name = device.name();
  if (name == "Equalizer") out.kind = BuiltinKind::Equalizer;
  else if (name == "Compressor") out.kind = BuiltinKind::Compressor;
  else if (name == "NoiseGate") out.kind = BuiltinKind::NoiseGate;
  else if (name == "Limiter") out.kind = BuiltinKind::Limiter;
  else return false;

  int n = JsfxSliderCount(out.kind);
  out.sliders.resize(n);
  for (int i = 0; i < n; i++) out.sliders[i] = Spec(out.kind, i).def;

  if (out.kind != BuiltinKind::Equalizer)
  {
    for (int i = 0; i < n; i++) ReadParam(out, device.child(Spec(out.kind, i).element), i);
    return true;
  }

  for (int b = 0; b < kEqBands; b++) out.sliders[2 + b * 6 + 2] = kEqDefaultFreq[b];
  ReadParam(out, device.child("InputGain"), 0);
  ReadParam(out, device.child("OutputGain"), 1);
  int b = 0;
  for (pugi::xml_node band : device.children("Band"))
  {
    if (b == kEqBands)
    {
      warnings.push_back("Equalizer bands beyond the 8th were dropped");
      break;
    }
    int base = 2 + b * 6;
    out.sliders[base] = 1;  // bands are on unless they say otherwise
    std::string type = band.attribute("type").value();
    for (int t = 0; t < 7; t++)
      if (type == kEqTypes[t]) out.sliders[base + 1] = t;
    double order;
    if (GetNum(band, "order", order)) out.sliders[base + 5] = std::max(1.0, std::min(8.0, std::round(order)));
    ReadParam(out, band.child("Enabled"), base);
    ReadParam(out, band.child("Freq"), base + 2);
    ReadParam(out, band.child("Gain"), base + 3);
    ReadParam(out, band.child("Q"), base + 4);
    b++;
  }
  return true;
}

double BuiltinSliderValue(BuiltinKind kind, const BuiltinParam& param, double value, const std::string& pointsUnit)
{
  return ToSlider(Spec(kind, param.slider), param.node, value, pointsUnit);
}

double BuiltinParamValue(BuiltinKind kind, int slider, double v, std::string& unit)
{
  switch (Spec(kind, slider).conversion)
  {
    case kDb:
    case kRange: unit = "decibel"; return v;
    case kMs: unit = "seconds"; return v / 1000.0;
    case kHz: unit = "hertz"; return v;
    default: unit = "linear"; return v;
  }
}

pugi::xml_node WriteBuiltinDevice(BuiltinKind kind, const std::vector<double>& sliders, bool enabled, bool loaded,
                                  pugi::xml_node devices, const std::function<std::string()>& newId,
                                  std::vector<std::string>& sliderParamIds)
{
  static const char* elements[] = {"", "Equalizer", "Compressor", "NoiseGate", "Limiter"};
  static const char* names[] = {"", "EQ", "Compressor", "Noise Gate", "Limiter"};
  int count = JsfxSliderCount(kind);
  std::vector<double> s(count);
  for (int i = 0; i < count; i++) s[i] = i < (int)sliders.size() ? sliders[i] : Spec(kind, i).def;
  sliderParamIds.assign(count, std::string());

  pugi::xml_node d = devices.append_child(elements[(int)kind]);
  d.append_attribute("deviceName") = names[(int)kind];
  d.append_attribute("deviceRole") = "audioFX";
  d.append_attribute("loaded") = loaded;
  d.append_attribute("id") = newId().c_str();
  pugi::xml_node en = d.append_child("Enabled");
  en.append_attribute("value") = enabled;
  en.append_attribute("id") = newId().c_str();

  auto real = [&](pugi::xml_node parent, const char* name, int slider) {
    const SliderSpec& spec = Spec(kind, slider);
    std::string unit;
    double v = BuiltinParamValue(kind, slider, s[slider], unit);
    std::string u2;
    pugi::xml_node p = parent.append_child(name);
    p.append_attribute("unit") = unit.c_str();
    p.append_attribute("min") = FormatDouble(BuiltinParamValue(kind, slider, spec.min, u2)).c_str();
    p.append_attribute("max") = FormatDouble(BuiltinParamValue(kind, slider, spec.max, u2)).c_str();
    p.append_attribute("value") = FormatDouble(v).c_str();
    sliderParamIds[slider] = newId();
    p.append_attribute("id") = sliderParamIds[slider].c_str();
  };
  auto boolean = [&](pugi::xml_node parent, const char* name, int slider) {
    pugi::xml_node p = parent.append_child(name);
    p.append_attribute("value") = s[slider] > 0.5;
    sliderParamIds[slider] = newId();
    p.append_attribute("id") = sliderParamIds[slider].c_str();
  };

  if (kind == BuiltinKind::Equalizer)
  {
    for (int b = 0; b < kEqBands; b++)
    {
      int base = 2 + b * 6;
      pugi::xml_node band = d.append_child("Band");
      int type = std::max(0, std::min(6, (int)std::lround(s[base + 1])));
      band.append_attribute("type") = kEqTypes[type];
      if (type == 3 || type == 4) band.append_attribute("order") = (int)std::lround(s[base + 5]);
      real(band, "Freq", base + 2);
      real(band, "Gain", base + 3);
      real(band, "Q", base + 4);
      boolean(band, "Enabled", base);
    }
    real(d, "InputGain", 0);
    real(d, "OutputGain", 1);
    return d;
  }

  const int* order = kind == BuiltinKind::Compressor ? kCompressorOrder : kind == BuiltinKind::NoiseGate ? kGateOrder : kLimiterOrder;
  for (int i = 0; i < count; i++)
  {
    int slider = order[i];
    const SliderSpec& spec = Spec(kind, slider);
    if (spec.conversion == kBool) boolean(d, spec.element, slider);
    else real(d, spec.element, slider);
  }
  return d;
}

}  // namespace dawp
