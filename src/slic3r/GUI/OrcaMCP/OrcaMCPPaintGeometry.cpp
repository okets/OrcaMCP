// src/slic3r/GUI/OrcaMCP/OrcaMCPPaintGeometry.cpp
#include "OrcaMCPPaintGeometry.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

bool parse_paint_axis(const std::string& name, PaintAxis& out)
{
    if (name.size() != 1)
        return false;
    switch (std::tolower(static_cast<unsigned char>(name[0]))) {
    case 'x': out = PaintAxis::X; return true;
    case 'y': out = PaintAxis::Y; return true;
    case 'z': out = PaintAxis::Z; return true;
    default:  return false;
    }
}

const char* paint_axis_name(PaintAxis axis)
{
    switch (axis) {
    case PaintAxis::X: return "x";
    case PaintAxis::Y: return "y";
    case PaintAxis::Z: return "z";
    }
    return "z";
}

std::vector<PaintBand> make_even_bands(const std::vector<int>& states, double axis_min, double axis_max)
{
    std::vector<PaintBand> bands;
    if (states.empty() || !(axis_max > axis_min))
        return bands;

    const double span = axis_max - axis_min;
    const double n    = double(states.size());
    bands.reserve(states.size());
    for (std::size_t i = 0; i < states.size(); ++i) {
        PaintBand band;
        band.state = states[i];
        // Interpolate each boundary from the ends rather than accumulating a width, so
        // band[i].to and band[i+1].from are bit-identical and the last `to` is exactly axis_max.
        band.from = i == 0 ? axis_min : axis_min + span * (double(i) / n);
        band.to   = i + 1 == states.size() ? axis_max : axis_min + span * (double(i + 1) / n);
        bands.push_back(band);
    }
    return bands;
}

int band_index_for_value(const std::vector<PaintBand>& bands, double value)
{
    for (std::size_t i = 0; i < bands.size(); ++i)
        if (value >= bands[i].from && value < bands[i].to)
            return int(i);

    // Nothing matched. The far end of the painted range is the one closed boundary in the set,
    // so a facet centroid sitting exactly on it belongs to the band that ends there.
    int    best_idx = -1;
    double best_to  = 0.0;
    for (std::size_t i = 0; i < bands.size(); ++i)
        if (best_idx < 0 || bands[i].to > best_to) {
            best_idx = int(i);
            best_to  = bands[i].to;
        }
    if (best_idx >= 0 && std::abs(value - best_to) <= 1e-9)
        return best_idx;
    return -1;
}

}}} // namespace Slic3r::GUI::OrcaMCP
