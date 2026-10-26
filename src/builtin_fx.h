// DAWproject built-in devices (Equalizer, Compressor, NoiseGate, Limiter) <-> the bundled JSFX
// in jsfx/, which the REAPER extension installs to <resource path>/Effects/DAWproject/.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "pugixml.hpp"

namespace dawp {

enum class BuiltinKind { None, Equalizer, Compressor, NoiseGate, Limiter };

struct JsfxFile
{
  const char* name;  // file name, e.g. "dawproject_eq.jsfx"
  const char* text;  // file contents
};
// The JSFX sources compiled into the binary.
const std::vector<JsfxFile>& BundledJsfx();

// Path of the effect relative to REAPER's Effects folder, as used in <JS ...> chunks.
std::string JsfxPath(BuiltinKind kind);
// Kind for a <JS ...> path, None if it is not one of ours.
BuiltinKind JsfxKind(const std::string& path);
int JsfxSliderCount(BuiltinKind kind);
void JsfxSliderRange(BuiltinKind kind, int slider, double& min, double& max);

// One automatable DAWproject parameter mapped to a JSFX slider (0-based).
struct BuiltinParam
{
  pugi::xml_node node;  // the DAWproject parameter element
  int slider;
  int conversion;       // internal: how values are converted
};

struct BuiltinDevice
{
  BuiltinKind kind = BuiltinKind::None;
  std::vector<double> sliders;  // all slider values
  std::vector<BuiltinParam> params;
};

// Import: reads a built-in device element. Returns false if the element is not one of the
// four standard devices. 'warnings' receives notes about settings that could not be kept.
bool ReadBuiltinDevice(pugi::xml_node device, BuiltinDevice& out, std::vector<std::string>& warnings);

// Converts an automation value of 'param' (in the points' unit, or the parameter's own unit
// if empty) to the slider's value.
double BuiltinSliderValue(BuiltinKind kind, const BuiltinParam& param, double value, const std::string& pointsUnit);

// Export: creates the DAWproject device element (child of 'devices') for slider values read from a
// <JS ...> chunk. 'newId' returns fresh XML ids. 'sliderParamIds' receives the id of the parameter
// element for each slider ("" if the slider has no parameter element).
pugi::xml_node WriteBuiltinDevice(BuiltinKind kind, const std::vector<double>& sliders, bool enabled, bool loaded,
                                  pugi::xml_node devices, const std::function<std::string()>& newId,
                                  std::vector<std::string>& sliderParamIds);

// Export: converts a slider value to the unit of the parameter written for it, and names that unit.
double BuiltinParamValue(BuiltinKind kind, int slider, double sliderValue, std::string& unit);

}  // namespace dawp
