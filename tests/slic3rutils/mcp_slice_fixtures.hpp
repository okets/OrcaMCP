#pragma once

// Small synthetic models the MCP slicing tests slice for real (test_slice_estimate.cpp,
// test_first_layer_plan.cpp, test_layer_plan.cpp). Each says which support it makes, so a test can
// derive its expectations from the shape instead of from a number it happened to see.

#include "libslic3r/TriangleMesh.hpp"

namespace mcp_test {

// A 30 x 30 mm cap on an 8 x 8 mm stem, 12 mm tall: the cap (z 10..12) overhangs the stem on every
// side, and its underside needs support down to the bed.
inline Slic3r::TriangleMesh supported_cap()
{
    Slic3r::TriangleMesh model = Slic3r::make_cube(8, 8, 10);
    model.translate(11, 11, 0);
    Slic3r::TriangleMesh cap = Slic3r::make_cube(30, 30, 2);
    cap.translate(0, 0, 10);
    model.merge(cap);
    return model;
}

// The same cap and stem standing on a 40 x 40 x 2 mm base, 5 mm wider than the cap on every side:
// the support under the cap, even spread a little past its edge, stands on the base's top (z 2), so
// the plate's first layer has no support in it at all.
inline Slic3r::TriangleMesh shelf_over_base()
{
    Slic3r::TriangleMesh model = Slic3r::make_cube(40, 40, 2);
    model.translate(-5, -5, 0);
    Slic3r::TriangleMesh stem  = Slic3r::make_cube(8, 8, 8);
    stem.translate(11, 11, 2);
    model.merge(stem);
    Slic3r::TriangleMesh cap = Slic3r::make_cube(30, 30, 2);
    cap.translate(0, 0, 10);
    model.merge(cap);
    return model;
}

} // namespace mcp_test
