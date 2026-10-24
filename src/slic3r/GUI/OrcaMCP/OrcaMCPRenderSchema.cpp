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
                            {"description", "\"plate\" (default) or {\"object_id\": n} to frame that object (object_index is the same number, kept) with what it prints on this layer: its support lines, and on the first layer its brim and raft."}
                        }}
                    }}
                };
}

json vec3_schema(const char* description)
{
    return json{{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 3}, {"description", description}};
}

// A view's `fit`: the plate, one object by its object_id (object_index is the same number, kept), a box or a point
// with a radius.
json fit_schema()
{
    return json{
        {"description", "\"plate\" (default), {\"object_id\": n} to frame one object -- its instances on this plate; an object this plate does not hold is an error naming its plate (object_index is the same number, kept) --, {box: {min: [x,y,z], max: [x,y,z]}} to frame a region, or {point: [x,y,z], radius_mm: r} to frame a sphere; coordinates in bed mm, or plate-local with the view's frame. A close-up is a small fit, not more pixels."},
        {"additionalProperties", false},
        {"properties", {
            {"object_id", {{"type", "integer"}, {"description", "The object to frame (get_scene_info's object_id)"}}},
            {"object_index", {{"type", "integer"}, {"description", "Same as object_id, kept"}}},
            {"box", {
                {"type", "object"},
                {"additionalProperties", false},
                {"description", "A region to frame: its corners in mm."},
                {"properties", {{"min", vec3_schema("[x, y, z] low corner")}, {"max", vec3_schema("[x, y, z] high corner")}}}
            }},
            {"point", vec3_schema("[x, y, z] centre of the sphere to frame (with radius_mm)")},
            {"radius_mm", {{"type", "number"}, {"description", "Radius in mm of the sphere around point (above 0)"}}}
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
                {"description", "Frame of camera_position/target and of a fit box or point: bed_mm (default, the get_scene_info frame) or plate_local (from this plate's front-left corner)."}
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
            {"camera_mode", {
                {"type", "string"},
                {"enum", {"fit", "exact"}},
                {"description", "fit (default): camera_position and target give the direction, the picture is zoomed to `fit` and the eye may be moved back. exact: the eye is at camera_position, looking at target, with fov_deg (perspective) or ortho_height_mm (orthographic); takes no preset or fit."}
            }},
            {"projection", {
                {"type", "string"},
                {"enum", {"perspective", "orthographic"}},
                {"description", "perspective (default) or orthographic."}
            }},
            {"fov_deg", {
                {"type", "number"},
                {"description", "camera_mode exact, perspective: the vertical field of view in degrees, 5 to 120 (default 30)."}
            }},
            {"ortho_height_mm", {
                {"type", "number"},
                {"description", "camera_mode exact, orthographic: the height of the image in mm (required there, above 0)."}
            }},
            {"camera", {
                {"type", "object"},
                {"description", "A camera object an earlier render returned (views[i].camera), unchanged, to draw exactly that view again at its resolution. Takes no other key in the view."}
            }},
            {"view_matrix", {{"type", "array"}, {"description", "A returned camera, sent as the view itself (view_matrix, projection_matrix, viewport and the keys below it carries): drawn exactly, like camera. The keys type, pixel_origin, mm_per_pixel, moved, requested_position, replayed and input_frame are accepted and ignored; camera_mode, projection, fov_deg, camera_position, target, preset and fit are ignored there too."}}},
            {"projection_matrix", {{"type", "array"}, {"description", "See view_matrix."}}},
            {"viewport", {{"type", "array"}, {"description", "See view_matrix: [x, y, width, height], a square."}}},
            {"type", {{"type", "string"}, {"description", "See view_matrix. Ignored."}}},
            {"pixel_origin", {{"type", "string"}, {"description", "See view_matrix. Ignored."}}},
            {"mm_per_pixel", {{"type", "number"}, {"description", "See view_matrix. Ignored."}}},
            {"moved", {{"type", "boolean"}, {"description", "See view_matrix. Ignored."}}},
            {"requested_position", {{"type", "array"}, {"description", "See view_matrix. Ignored."}}},
            {"replayed", {{"type", "boolean"}, {"description", "See view_matrix. Ignored."}}},
            {"input_frame", {{"type", "string"}, {"description", "See view_matrix. Ignored."}}},
            {"plate_index", {{"type", "integer"}, {"description", "See view_matrix: the plate the camera was drawn of; a replay on another plate is refused."}}},
            {"near_mm", {{"type", "number"}, {"description", "See view_matrix. Ignored."}}}
        }}
    };
}

json light_schema()
{
    return json{
        {"type", {"string", "object"}},
        {"additionalProperties", false},
        {"description", "\"default\" (the 3D view's two lights) or a single directional light that rakes across a surface so relief reads. To rake what a view shows, use {from: \"left\"|\"right\"|\"top\"|\"bottom\", elevation_deg}: the light comes from that side of each view's image, elevation_deg (1 to 90, default 20) above the surface the camera sees, so low grazes it and shows relief; {from: \"front\"} lights from the camera. It turns with each view's camera. In bed axes instead: {azimuth_deg, elevation_deg} (azimuth 0 = from +X, counter-clockwise seen from above, elevation 1 to 90 above the horizon) or {direction: [x, y, z]}, the way the light travels (from the light to the scene); these can light a face from behind, giving a dark picture. A face square to the light is drawn at about 0.9 of its colour, one turned away at about 0.28; no highlight. The answer's light gives the bed-axes direction used (direction, azimuth_deg, elevation_deg; bed_elevation_deg with from) and applied: false with a warning when the render shader could not be used (shader_fallback) or nothing was drawn. 3D views only."},
        {"properties", {
            {"from", {{"type", "string"}, {"enum", {"left", "right", "top", "bottom", "front"}}, {"description", "Camera-relative: the side of the image the light comes from"}}},
            {"azimuth_deg", {{"type", "number"}, {"description", "Bed axes. Degrees; 0 = light from +X, counter-clockwise seen from above"}}},
            {"elevation_deg", {{"type", "number"}, {"description", "Degrees, 1 to 90: above the horizon (bed axes) or above the surface the camera sees (from)"}}},
            {"direction", vec3_schema("[x, y, z] the way the light travels, bed axes")},
            {"bed_elevation_deg", {{"type", "number"}, {"description", "Accepted back from an answer; ignored"}}},
            {"applied", {{"type", "boolean"}, {"description", "Accepted back from an answer; ignored"}}},
            {"warning", {{"type", "string"}, {"description", "Accepted back from an answer; ignored"}}}
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
    return "Render a picture of a plate, one object, a region or a sliced layer's toolpaths (layer_view). Omit views for a contact sheet of iso, top and front fitted to the plate. A view is a preset (iso|top|front|back|left|right|low) or {camera_position, target} in BED mm (the get_scene_info frame; plate N sits at plates[N].bounding_box), or add frame: \"plate_local\" to give them relative to the plate's front-left corner. By default (camera_mode fit) a custom camera gives only the direction: the picture is zoomed to the view's fit -- the plate, {object_id}, a box {min, max} or a point with radius_mm -- and the eye may be moved back (camera.moved says so). camera_mode exact honours the eye, with fov_deg, or projection orthographic with ortho_height_mm. A close-up is a small fit box, not more pixels: camera.mm_per_pixel says the scale. Pass a camera a render returned as views[i].camera to draw exactly that view again. The requested plate's objects are drawn, including any hanging over its edge, from the 3D scene whichever tab the app shows. Every view returns objects_in_frame, uniform_image (+hint), plate_origin and the camera; overlays (outline, 10 mm grid, origin, labels) are on by default. paint draws support, seam, fuzzy_skin and texture-layer paint as the paint panels colour it (texture: one colour per layer, texture_layers to choose); colour paint is always drawn in its filaments' colours; color_by filament draws every part in its filament's colour instead of one colour per object. Use save_to_file=true for PNG paths. layer_view draws a top-down plan instead: \"first_layer\", or any sliced layer as {layer: N} or {z: mm}, filtered by features and filaments, with the layer's height, filaments and extruded areas -- the way to check supports, interfaces and which tool prints what on a given layer.";
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
            {"light", light_schema()},
            {"paint", {
                {"type", "array"},
                {"items", {{"type", "string"}, {"enum", {"support", "seam", "fuzzy_skin", "texture"}}}},
                {"description", "Paint to tint on the parts, as the paint panels colour it: support and seam enforcers light green and blockers light red, fuzzy_skin light green, texture one colour per layer (texture_layers chooses). The unpainted surface is then drawn neutral grey so the tints read. Colour paint is always drawn in its filaments' colours, without this. The answer's tint_legend gives each tint's colour and, per object, its facets (get_object_paint's count) and area_mm2 (one instance). A texture mask its mesh has outgrown is not drawn and is marked stale; tint_notes explains an empty case. Ask one channel at a time to tell support from seam in the picture: they share the panels' colours. 3D views only."}
            }},
            {"texture_layers", {
                {"type", "array"},
                {"items", {{"type", "integer"}, {"minimum", 0}, {"maximum", 7}}},
                {"description", "With paint texture: only these layer slots (0 to 7). Default: every slot a texture stack on the plate has a layer in. A slot nothing is painted in is listed with no objects."}
            }},
            {"color_by", {
                {"type", "string"},
                {"enum", {"object", "filament"}},
                {"description", "object (default): one colour per object, so objects tell apart. filament: each part in its filament's colour, as the 3D view. With paint, unpainted surfaces are neutral grey, so color_by only matters without it. objects_in_frame[].color_source says where each part's colour came from: palette, filament, paint (colour paint, always its filaments' colours) or neutral (paint tints asked for)."}
            }},
            {"views", views_schema()}
        }},
        {"required", {"plate_index"}}
    };
}

}}} // namespace Slic3r::GUI::OrcaMCP
