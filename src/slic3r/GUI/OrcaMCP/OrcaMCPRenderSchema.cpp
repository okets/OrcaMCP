#include "OrcaMCPRenderRequest.hpp"

// render_plate_view's input schema and description, one builder per part so each option is declared where
// it is added. The registration (OrcaMCPServer.cpp) only calls render_plate_view_schema().

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {

using json = nlohmann::json;

json overlays_schema()
{
    return json{
                    {"type", {"boolean", "object"}},
                    {"description", "Self-locating overlays drawn on the image: plate outline, 10 mm grid, origin with X/Y, numbered object labels, hatched excluded areas and collision volumes (a height-limited one drawn at its height; in a layer plan, dashed when not at that layer). true/omitted = all, false = none, or an object {outline, grid, origin, labels, excluded} of booleans."},
                    {"properties", {
                        {"outline", {{"type", "boolean"}}}, {"grid", {{"type", "boolean"}}}, {"origin", {{"type", "boolean"}}},
                        {"labels", {{"type", "boolean"}}}, {"excluded", {{"type", "boolean"}}}
                    }}
                };
}

json layer_view_schema()
{
    return json{
                    {"type", {"string", "object"}},
                    {"description", "Instead of a 3D render, a top-down plan; ignores views. \"first_layer\": the first layer -- object footprints (or their raft), brim loops, support and wipe tower -- from the sliced plate (source: sliced) or the model footprints when unsliced (source: footprints); the view for 'is the brim wide enough' and 'where do the support feet land'. {layer: N} or {z: mm}: any layer of the sliced G-code, the lines the Preview draws (source: gcode; an unsliced plate is an error). Returns layer {number, of, z}, filaments (printing on it, in order), extruded_mm2 by feature and extruded_mm2_by_filament (line length x width, the whole layer whatever is drawn), object_mm2 vs support_mm2, objects_at_height (per object its object_layer and support_layer printed at this height -- support can have heights of its own, so either can be null -- and overhang: area_mm2 hanging past the layer below and support_below, the first support layer going down, at most 2 mm, whose lines lie under this overhang -- z, gap_mm down to it, and the support_mm2 and interface_mm2 of the overhang they lie under; a gap near the configured top Z distance means that is the contact layer. When none within 2 mm has lines under it, support_below is the nearest support layer below with 0 areas and searched_to_mm 2: nothing touches the overhang. null means no support layer below at all), drawn, legend, and nothing_drawn (+hint) when the filter matches no line."},
                    {"properties", {
                        {"layer", {
                            {"type", "integer"},
                            {"description", "Layer number from 1, as the Preview's layer slider numbers layers: 1..get_print_estimate's printed_layers."}
                        }},
                        {"z", {
                            {"type", "number"},
                            {"description", "A height in mm; the nearest printed layer is drawn (a tie goes to the lower)."}
                        }},
                        {"features", {
                            {"type", "array"},
                            {"items", {{"type", "string"}, {"enum", {"perimeters", "infill", "support", "support_interface", "brim", "skirt", "prime_tower"}}}},
                            {"description", "Draw only these (default all)."}
                        }},
                        {"filaments", {
                            {"type", "array"},
                            {"items", {{"type", "integer"}}},
                            {"description", "Draw only these filament slots, 1-based as get_scene_info's filaments_used (default all)."}
                        }},
                        {"color_by", {
                            {"type", "string"},
                            {"enum", {"feature", "filament"}},
                            {"description", "feature (default): the Preview's feature colours. filament: each slot's colour, as the Preview's Filament view. The legend says which colour is which."}
                        }},
                        {"fit", {
                            {"description", "\"plate\" (default) or {\"object_index\": n} to frame that object with what it prints on this layer: its support lines, and on the first layer its brim and raft."}
                        }}
                    }}
                };
}

// A view's `fit`: the plate, or one object by its object_id (object_index is the same number, kept).
json fit_schema()
{
    return json{
        {"description", "\"plate\" (default) or {\"object_id\": n} to frame one object -- its instances on this plate; an object this plate does not hold is an error naming its plate (object_index is the same number, kept). A closer camera beats more pixels."},
        {"properties", {
            {"object_id", {{"type", "integer"}, {"description", "The object to frame (get_scene_info's object_id)"}}},
            {"object_index", {{"type", "integer"}, {"description", "Same as object_id, kept"}}}
        }}
    };
}

// One view. Strict: a key the schema does not declare (fov, zoom, distance...) is refused instead of ignored.
json view_schema()
{
    return json{
        {"type", "object"},
        {"additionalProperties", false},
        {"properties", {
            {"preset", {
                {"type", "string"},
                {"enum", {"iso", "top", "front", "back", "left", "right", "low"}},
                {"description", "Named camera framing `fit` (default the plate). low = bed level from the front, for first layers and support feet."}
            }},
            {"fit", fit_schema()},
            {"frame", {
                {"type", "string"},
                {"enum", {"bed_mm", "plate_local"}},
                {"description", "Frame of camera_position/target: bed_mm (default, the get_scene_info frame) or plate_local (from this plate's front-left corner)."}
            }},
            {"camera_position", {
                {"type", "array"},
                {"items", {{"type", "number"}}},
                {"description", "[x, y, z] in bed mm, same frame as get_scene_info"}
            }},
            {"target", {
                {"type", "array"},
                {"items", {{"type", "number"}}},
                {"description", "[x, y, z] in bed mm; point it at the plate's objects (get_scene_info positions)"}
            }},
            {"camera", {
                {"type", "object"},
                {"description", "A camera object an earlier render returned, to draw the same view again."}
            }}
        }}
    };
}

json views_schema()
{
    return json{
        {"type", "array"},
        {"description", "Views to render. Omit for the default contact sheet (iso, top, front). Each view: a preset with optional fit, or camera_position + target (bed mm unless frame is plate_local)."},
        {"items", view_schema()}
    };
}

} // namespace

std::string render_plate_view_description()
{
    return "Render a picture of a plate, one object, or a sliced layer's toolpaths (layer_view). Omit views for a contact sheet of iso, top and front fitted to the plate. A view is {preset: iso|top|front|back|left|right|low, fit: \"plate\" | {object_index}} or explicit {camera_position, target} in BED mm (the get_scene_info frame; plate N sits at plates[N].bounding_box) -- or add frame: \"plate_local\" to give them relative to the plate's front-left corner. An explicit camera_position and target set the viewing direction only: the image is always fitted to the plate's drawn objects (20% margin), so a close-up of part of an object is not possible today; fit {object_index} frames one whole object. The requested plate's objects are drawn, including any hanging over its edge, from the 3D scene whichever tab the app shows. Every view returns objects_in_frame, uniform_image (+hint), plate_origin and the camera; overlays (outline, 10 mm grid, origin, labels) are on by default. Use save_to_file=true for PNG paths. layer_view draws a top-down plan instead: \"first_layer\", or any sliced layer as {layer: N} or {z: mm}, filtered by features and filaments, with the layer's height, filaments and extruded areas -- the way to check supports, interfaces and which tool prints what on a given layer.";
}

json render_plate_view_schema()
{
    return json{
        {"type", "object"},
        {"properties", {
            {"plate_index", {
                {"type", "integer"},
                {"description", "Plate index (0-based)"}
            }},
            {"save_to_file", {
                {"type", "boolean"},
                {"description", "Return file paths instead of base64 (default: false)"},
                {"default", false}
            }},
            {"resolution", {
                {"type", "integer"},
                {"description", "Resolution in pixels (default: 512)"},
                {"default", 512}
            }},
            {"overlays", overlays_schema()},
            {"layer_view", layer_view_schema()},
            {"image_format", {
                {"type", "string"},
                {"enum", {"png", "jpeg"}},
                {"description", "png or jpeg. Default: png when save_to_file is true (crisp overlays, alpha kept), jpeg for inline base64 (smaller)."}
            }},
            {"views", views_schema()}
        }},
        {"required", {"plate_index"}}
    };
}

}}} // namespace Slic3r::GUI::OrcaMCP
