// src/slic3r/GUI/OrcaMCP/OrcaMCPTextureBakeTools.hpp
//
// What the bake tools and the render's texture preview share on the app side (Plater and the preset bundle), so the
// preview is computed with the very settings the bake would use.
#ifndef slic3r_GUI_OrcaMCPTextureBakeTools_hpp_
#define slic3r_GUI_OrcaMCPTextureBakeTools_hpp_

#include <vector>

#include "libslic3r/TextureDisplacement.hpp"

namespace Slic3r { namespace GUI {

class Plater;

namespace OrcaMCP {

// The colour settings a bake of `layers` with `options` captures (the project's filaments and mixed slots, the palette
// ranking): empty when no layer is colouring. Main thread.
TextureColorSettings bake_color_settings(Plater& plater, const std::vector<TextureDisplacementLayer>& layers,
                                         const TextureDisplacementOptions& options);

// The texture displacement panel is open on this object (it works on the selected one). Main thread.
bool texture_panel_open_on(Plater& plater, int object_id);

// The plate holding the object's first instance, -1 when it stands on none. Main thread.
int object_plate_index(Plater& plater, int object_id);

}}} // namespace Slic3r::GUI::OrcaMCP

#endif
