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
            {"compare_with", {
                {"type", "string"},
                {"description", "The file_path of an earlier render_plate_view picture of this session (a PNG, saved with save_to_file, of this plate): the view is drawn with that picture's camera and size, and the answer's picture is three columns of that size -- before, now, and the difference (now in grey, the pixels whose colour changed by more than 24 of 255 in red). comparison gives changed_fraction, changed_box [x0, y0, x1, y1] in a column's pixels, and the columns' x_offsets; the picture is one for the eye and these numbers, not one to compare with later. Send the same overlays, light, section and paint in both pictures, or those differences count as changes. Takes no preset, fit or camera of its own (only the earlier picture's own camera, unchanged), one view per call, and not with layer_view. A path that is not one of this server's pictures, one not rendered in this session, a JPEG, another plate or another resolution is refused. Nothing is undone or changed to make the earlier state: to see a change, render before it with save_to_file, make it, then render with compare_with."}
            }},
            {"camera", {
                {"type", "object"},
                {"description", "A camera object an earlier render returned (views[i].camera), unchanged, to draw exactly that view again at its resolution. Takes no other key in the view, except compare_with."}
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

json section_schema()
{
    return json{
        {"type", {"string", "object"}},
        {"additionalProperties", false},
        {"description", "Cut the objects open with a plane and draw the cut faces dark grey, as the 3D view's section view does; the side the normal points to is removed. \"app\": the cut the 3D view shows now (an error when its section view is off). {normal, point}: the plane through point with that normal. {normal, ratio}: the section view's own sweep, ratio above 0 to 1, from the end of the bounding sphere of the plate's instances the normal points to (0, nothing cut) to the opposite end (1, everything). {plane: x|y|z, at_mm}: square to an axis, removing the larger coordinates (remove: below removes the smaller). Points and at_mm are bed mm, or plate-local with frame: plate_local. The answer's section gives the plane used (normal, point) and per object section_area_mm2, islands and effect (cut, keeps_all or removes_all); empty with a note when the plane cuts no object, and a note for an object with open edges, whose cut face may be short (slicing drops an open outline). The cut is limited to z >= 0 (a sunk part's cut below the bed is neither drawn nor counted). It needs the render shader (an error when this driver could not build it), works with any camera and light, and paint tints are cut with the surface. 3D views only."},
        {"properties", {
            {"normal", vec3_schema("Unit or not: the side it points to is removed")},
            {"point", vec3_schema("A point on the plane")},
            {"ratio", {{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 1}, {"description", "With normal: the 3D view's ratio, 0 (nothing cut) to 1 (everything)"}}},
            {"plane", {{"type", "string"}, {"enum", {"x", "y", "z"}}, {"description", "The axis the plane is square to"}}},
            {"at_mm", {{"type", "number"}, {"description", "With plane: where it cuts along that axis"}}},
            {"remove", {{"type", "string"}, {"enum", {"above", "below"}}, {"description", "With plane: above (default) removes the larger coordinates"}}},
            {"frame", {{"type", "string"}, {"enum", {"bed_mm", "plate_local"}}, {"description", "Frame of point and of at_mm for x and y (default bed_mm)"}}},
            {"source", {{"type", "string"}, {"description", "Accepted back from an answer; ignored"}}},
            {"removes", {{"type", "string"}, {"description", "Accepted back from an answer; ignored"}}},
            {"ratio_used", {{"type", "number"}, {"description", "Accepted back from an answer; ignored"}}},
            {"cap_color", {{"type", "string"}, {"description", "Accepted back from an answer; ignored"}}},
            {"applied", {{"type", "boolean"}, {"description", "Accepted back from an answer; ignored"}}},
            {"empty", {{"type", "boolean"}, {"description", "Accepted back from an answer; ignored"}}},
            {"objects", {{"type", "array"}, {"description", "Accepted back from an answer; ignored"}}},
            {"notes", {{"type", "array"}, {"description", "Accepted back from an answer; ignored"}}},
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
    return "Render a picture of a plate, one object, a region or a sliced layer's toolpaths (layer_view). Omit views for a contact sheet of iso, top and front fitted to the plate. A view is a preset (iso|top|front|back|left|right|low) or {camera_position, target} in BED mm (the get_scene_info frame; plate N sits at plates[N].bounding_box), or add frame: \"plate_local\" to give them relative to the plate's front-left corner. By default (camera_mode fit) a custom camera gives only the direction: the picture is zoomed to the view's fit -- the plate, {object_id}, a box {min, max} or a point with radius_mm -- and the eye may be moved back (camera.moved says so). camera_mode exact honours the eye, with fov_deg, or projection orthographic with ortho_height_mm. A close-up is a small fit box, not more pixels: camera.mm_per_pixel says the scale. Pass a camera a render returned as views[i].camera to draw exactly that view again. The requested plate's objects are drawn, including any hanging over its edge, from the 3D scene whichever tab the app shows. Every view returns objects_in_frame, uniform_image (+hint), plate_origin and the camera; overlays (outline, 10 mm grid, origin, labels) are on by default. paint draws support, seam, fuzzy_skin and texture-layer paint as the paint panels colour it (texture: one colour per layer, texture_layers to choose); colour paint is always drawn in its filaments' colours; texture_preview draws painted texture layers as the bake would make them, before baking; color_by filament draws every part in its filament's colour instead of one colour per object; color_by relief colours the depth of a texture's relief and color_by detail the facet angle of any mesh (pattern, aliasing, triangle-size bands). A view with compare_with draws the same camera as an earlier saved picture and sets it beside that picture and a mask of what changed (before/after a bake or an edit). section cuts the objects open with a plane and draws the cut faces, reporting their areas. Use save_to_file=true for PNG paths. layer_view draws a top-down plan instead: \"first_layer\", or any sliced layer as {layer: N} or {z: mm}, filtered by features and filaments, with the layer's height, filaments and extruded areas -- the way to check supports, interfaces and which tool prints what on a given layer.";
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
            {"section", section_schema()},
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
                {"enum", {"object", "filament", "relief", "detail", "triangle_size"}},
                {"description", "object (default): one colour per object, so objects tell apart. filament: each part in its filament's colour, as the 3D view. With paint, unpainted surfaces are neutral grey, so object and filament only matter without it. objects_in_frame[].color_source says where each part's colour came from: palette, filament, paint (colour paint, always its filaments' colours), neutral (paint tints asked for), relief or detail. relief: colour each facet by how far it lies above (red) or below (blue) the surface the part had before its texture, white at it: with texture_preview the unbaked surface is the reference (what the texture adds); after a bake_texture_displacement of this session the mesh the bake displaced is (kept in memory until the project is closed, 8 parts, and used only while the part still has the baked mesh -- an undo, repair or new bake drops it); for any other part (a mesh baked in the texture panel, a model loaded already textured, an earlier session's bake) there is no reference and the part keeps its colour, listed in relief.not_coloured with the reason. The distance is to the nearest point of the reference surface, not along the displacement, so steep relief reads shallower. Values are in bed mm, the instance's scale included. The scale spans the 99th percentile of |distance| (range_source measured_p99; the true extremes are in min_mm and max_mm, the facets beyond it in beyond_scale); a second bake of a part measures that bake's own increment. The answer's relief gives the scale (from, to, bins, step in mm, range_source), a legend of each colour step's range, per object and part the reference, min_mm, max_mm and mean_abs_mm, and notes; the colours are shaded by the lights, so read the legend by hue and rake the light for relief. detail: needs no reference and works on any mesh: each facet is coloured by its largest angle to the facets it shares an edge with (pale = flat or smooth, dark = sharp), which shows a texture's pattern, the stair-stepping of one sampled too coarsely and bands where the mesh's triangles change size (but a band of large flat triangles is pale, its angles being 0: use triangle_size for density); the answer's detail gives the scale in degrees, per part the angle statistics, open_edges, edge_mm (triangle edge length min, median, max in mm) and slabs. triangle_size: each facet coloured by its longest edge in mm on a log scale (blue fine, red coarse), which shows aliasing bands and changes of density that facet angles cannot; the answer's triangle_size gives the legend in mm and per part longest_edge_mm statistics, beyond_scale and slabs: the median longest edge per 2 mm slab of bed Z (at most 64 slabs), so a band shows in numbers. With texture_preview these three colour the unbudgeted preview, which cannot predict the bands the bake's own triangle budget makes: bake, then colour the baked mesh. A degenerate facet is skipped as a neighbour. With texture_preview all colour the displaced preview; colour-painted parts coloured this way lose their filament colours. A part over 1 million triangles is not coloured (too_large), one picture colours at most 2 million triangles, and the work must finish within the call's wait. 3D views only."}
            }},
            {"relief_range_mm", {
                {"type", "number"},
                {"exclusiveMinimum", 0},
                {"description", "With color_by relief: the colour scale's half range in mm (the colours run from minus this to plus this, 15 steps); facets beyond it take the end colours (counted in beyond_scale). Default: the 99th percentile of the measured |distance|, so a few spikes do not wash the rest out."}
            }},
            {"detail_range_deg", {
                {"type", "number"},
                {"exclusiveMinimum", 0},
                {"description", "With color_by detail: the facet angle in degrees that takes the darkest colour (12 steps from 0). Default: the measured 98th percentile, rounded up to a whole degree."}
            }},
            {"texture_preview", {
                {"type", "boolean"},
                {"description", "Draw each textured object as its bake would make it -- the true displaced surface and the texture's colours, with the part's own colour paint kept, computed as the bake computes it (a classic part: after its preparation) but without simplification, as the texture panel's preview -- instead of the flat unbaked mesh. Nothing is baked or changed. It takes time on large meshes: when every view fits an object (fit {object_id}, or a box or point that only some reach) only those objects are previewed, otherwise all of them. The answer's texture_preview says which objects were drawn, how long each took, whether it was kept from an earlier call (cached: the last 4 objects or 2 million triangles are kept until the object's layers, paint, mesh or palette change), and which were left out and why. An object whose preview does not finish within the call's wait is drawn unbaked and listed timed_out (not_reached when the wait ran out before it was started); a part over 1 million triangles is too_large. Not with paint texture, a section or a layer_view. Default false."},
                {"default", false}
            }},
            {"views", views_schema()}
        }},
        {"required", {"plate_index"}}
    };
}

}}} // namespace Slic3r::GUI::OrcaMCP
