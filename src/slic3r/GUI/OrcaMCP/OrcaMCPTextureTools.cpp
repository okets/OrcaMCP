// src/slic3r/GUI/OrcaMCP/OrcaMCPTextureTools.cpp
//
// The texture displacement tools: the library of textures a layer can use, and (further tools) the layer stack,
// the unwrap, the bake. Their decisions apart from the app are OrcaMCPTextureDisplacement.cpp's; the owner of an
// object's stack, its parts and the bake frame are the texture panel's own functions over the model
// (TextureDisplacementObject.hpp), so a tool and the panel act on the same data.
#include "OrcaMCPServer.hpp"
#include "OrcaMCPCommon.hpp"
#include "OrcaMCPNextSteps.hpp"
#include "OrcaMCPPaintModel.hpp"
#include "OrcaMCPPaintSelect.hpp"
#include "OrcaMCPPartEdits.hpp"
#include "OrcaMCPPartTools.hpp"
#include "OrcaMCPTextureBakeTools.hpp"
#include "OrcaMCPTextureDisplacement.hpp"
#include "OrcaMCPUiJob.hpp"
#include "OrcaMCPUserView.hpp"

#include "slic3r/Utils/FixModelByCgal.hpp"
#include "slic3r/Utils/ThreadCancel.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoTextureDisplacement.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/TextureDisplacementObject.hpp"
#include "slic3r/GUI/TextureLibrary.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <wx/log.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

using namespace Slic3r::GUI;
using namespace Slic3r::GUI::OrcaMCP;
using Slic3r::DecodedHeightTexture;

namespace {

// The library as the picker lists it, copied on the main thread: texture_library() is a cache the panel and
// the importers rewrite there. No rescan, as the picker does not rescan.
struct LibraryCopy
{
    std::vector<Slic3r::GUI::TextureLibraryEntry> entries;
    std::vector<std::string>                      ids;
    std::string                                   user_folder;
};

LibraryCopy library_copy()
{
    LibraryCopy copy;
    copy.entries     = texture_library();
    copy.ids         = texture_ids(copy.entries);
    copy.user_folder = user_texture_dir();
    return copy;
}

nlohmann::json get_texture_library(const nlohmann::json& params)
{
    std::string                error;
    const std::optional<std::string> source = read_text(params, "source", error);
    if (!error.empty())
        return error_response(error);

    LibraryCopy library;
    run_on_main_thread([&library]() -> nlohmann::json {
        library = library_copy();
        return {{"status", "success"}};
    });

    // Each file's colour is read from its header, off the main thread.
    std::vector<std::optional<bool>> colors;
    colors.reserve(library.entries.size());
    for (const Slic3r::GUI::TextureLibraryEntry& entry : library.entries)
        colors.push_back(png_color_from_file(entry.path));

    nlohmann::json answer = library_list_json(library.entries, library.ids, colors, source, error);
    if (!error.empty())
        return error_response(error);
    answer["status"]      = "success";
    answer["user_folder"] = library.user_folder;
    return answer;
}

// Every layer, on any object, whose image is the file at `path`: what keeps working after the file is deleted.
nlohmann::json layers_using(const Slic3r::Model& model, const std::string& path)
{
    nlohmann::json users = nlohmann::json::array();
    for (size_t i = 0; i < model.objects.size(); ++i) {
        const Slic3r::ModelVolume* owner = texture_stack_owner(*model.objects[i]);
        if (owner == nullptr)
            continue;
        for (const Slic3r::TextureDisplacementLayer& layer : owner->texture_displacement_layers)
            if (layer.path == path)
                users.push_back({{"object_id", i}, {"layer", layer.slot}});
    }
    return users;
}

nlohmann::json delete_user_texture(const nlohmann::json& params)
{
    const std::string query = params.at("texture").is_string() ? params.at("texture").get<std::string>() : std::string();
    if (!params.at("texture").is_string() || query.empty())
        return error_response("texture must be a texture id from get_texture_library");

    return run_on_main_thread([&query]() -> nlohmann::json {
        McpDialogSuppressionGuard guard;
        const LibraryCopy         library = library_copy();
        const TextureLookup       found   = find_texture(library.entries, library.ids, query);
        if (found.index < 0)
            return error_response(found.error);
        const Slic3r::GUI::TextureLibraryEntry& entry = library.entries[size_t(found.index)];
        const std::string&                      id    = library.ids[size_t(found.index)];
        if (const auto refusal = user_texture_delete_refusal(entry, id))
            return error_response(*refusal);

        const nlohmann::json still_used_by = layers_using(wxGetApp().plater()->model(), entry.path);
        std::string          error;
        if (!remove_user_texture(entry.path, error))
            return guard.fail_on_errors(error_response(error));
        bump_texture_library_generation(); // the panel's cache of images by path forgets the deleted one

        nlohmann::json answer = {{"status", "success"},
                                 {"deleted", {{"texture", id}, {"name", entry.name}, {"path", entry.path}}},
                                 {"still_used_by", still_used_by}};
        if (!still_used_by.empty())
            answer["note"] = "Those layers keep the image: they hold it themselves, and a saved project stores it. The texture is gone "
                             "from the library, so a layer given another texture cannot go back to it";
        return guard.report(std::move(answer));
    });
}


// ---- Images: reading, and reusing what the library already holds ----

struct ImageRead
{
    std::shared_ptr<std::vector<unsigned char>> data;
    DecodedHeightTexture                        decoded;
};

// The image at `path` as a layer would hold it (converted to a PNG when it is another format), decoded.
std::optional<ImageRead> read_image(const std::string& path, std::string& error)
{
    ImageRead out;
    out.data = load_texture_image_data(path, error);
    if (!out.data)
        return std::nullopt;
    if (const auto size = png_dimensions(out.data->data(), out.data->size()); size && std::max(size->first, size->second) > max_texture_side_px) {
        error = path + " is " + std::to_string(size->first) + " x " + std::to_string(size->second) + " px; a texture layer takes images up to " +
                std::to_string(max_texture_side_px) + " px a side";
        return std::nullopt;
    }
    Slic3r::TextureDisplacementLayer probe;
    probe.image_data = out.data;
    out.decoded      = Slic3r::decode_height_texture(probe, /*keep_decoded=*/false);
    if (out.decoded.empty()) {
        error = path + " is not an image a texture layer can use (a grayscale or RGB picture)";
        return std::nullopt;
    }
    drop_grey_colour(out.decoded); // the library keeps such an image as grayscale
    return out;
}

// The library texture whose decoded image is `wanted` (same size and pixels, same colours or none), with the
// image read from its file. A PNG whose header size differs is not opened.
std::optional<std::pair<Slic3r::GUI::TextureLibraryEntry, ImageRead>> find_identical_texture(const DecodedHeightTexture& wanted)
{
    for (const Slic3r::GUI::TextureLibraryEntry& entry : texture_library()) {
        if (const auto size = png_dimensions_from_file(entry.path); size && (size->first != wanted.width || size->second != wanted.height))
            continue;
        std::string error;
        if (auto read = read_image(entry.path, error); read && same_texture(read->decoded, wanted))
            return std::make_pair(entry, std::move(*read));
    }
    return std::nullopt;
}

struct LibraryImage
{
    Slic3r::GUI::TextureLibraryEntry            entry;
    std::shared_ptr<std::vector<unsigned char>> data;
    bool                                        reused = false;
};

std::optional<LibraryImage> library_image(const Slic3r::GUI::TextureLibraryEntry& entry, bool reused, std::string& error)
{
    LibraryImage out;
    out.entry  = entry;
    out.reused = reused;
    out.data   = load_texture_image_data(entry.path, error);
    if (!out.data)
        return std::nullopt;
    return out;
}

LibraryImage reused_image(std::pair<Slic3r::GUI::TextureLibraryEntry, ImageRead>&& found)
{
    return LibraryImage{std::move(found.first), std::move(found.second.data), true};
}

// An image file given to a layer: the identical texture the library holds, else imported into it.
std::optional<LibraryImage> import_or_reuse(const std::string& path, const DecodedHeightTexture& decoded, std::string& error)
{
    if (auto same = find_identical_texture(decoded))
        return reused_image(std::move(*same));
    const auto entry = import_texture_to_library(path, error);
    if (!entry)
        return std::nullopt;
    return library_image(*entry, false, error);
}

// An edited image (adjust_image): the identical texture the library holds, else saved into it.
std::optional<LibraryImage> save_or_reuse(const DecodedHeightTexture& image, const std::string& stem, std::string& error)
{
    if (auto same = find_identical_texture(image))
        return reused_image(std::move(*same));
    const auto entry = save_texture_to_library(image, stem, error);
    if (!entry)
        return std::nullopt;
    return library_image(*entry, false, error);
}

// ---- Where a view projects from ----

using Axes = std::pair<Slic3r::Vec3d, Slic3r::Vec3d>; // right, up in plate axes

// A named view of the 3D view's cameras, or `current`: the camera now.
Axes view_axes_of(const std::string& name)
{
    Slic3r::GUI::Camera        local;
    const Slic3r::GUI::Camera* camera = &local;
    if (name == "current")
        camera = &wxGetApp().plater()->get_camera();
    else
        local.select_view(camera_named(name)->direction);
    const Slic3r::Matrix3d rotation = camera->get_view_matrix().matrix().block<3, 3>(0, 0);
    return {rotation.row(0).transpose(), rotation.row(1).transpose()};
}

bool nearly_same(const Slic3r::Vec3f& a, const Slic3r::Vec3f& b) { return (a - b).norm() < 1e-5f; }

// ---- One call's edit of a stack ----

// What the edit will do, worked out from the model with nothing changed.
struct EditPlan
{
    Slic3r::ModelObject*                     object = nullptr;
    Slic3r::ModelVolume*                     owner  = nullptr;
    int                                      object_id = -1;
    LayerRequest                             request;
    int                                      slot = -1;                   // the layer edited, or the slot an add takes
    std::optional<Slic3r::TextureDisplacementLayer> before;               // edit, remove, move
    Slic3r::TextureDisplacementLayer         after;                       // the layer as it will be (image placeholders apply later)
    Slic3r::TextureDisplacementOptions       options;                     // the stack's options as they will be
    std::optional<ImageRead>                 picked;                      // texture: the library entry's image
    std::optional<Slic3r::GUI::TextureLibraryEntry> picked_entry;
    std::optional<ImageRead>                 file;                        // file_path: the candidate image
    std::optional<DecodedHeightTexture>      adjusted;                    // adjust_image: the image after it
    std::string                              adjust_stem;
    bool                                     image_changed = false;
    std::optional<VisibleView>               frame_view;                  // a frame set by this call: what paints inside it
    bool                                     changed       = false;
    int                                      move_from = 0, move_to = 0;
    std::string                              snapshot;
};

const Slic3r::TextureDisplacementLayer* layer_in_slot(const std::vector<Slic3r::TextureDisplacementLayer>& stack, int slot)
{
    for (const Slic3r::TextureDisplacementLayer& layer : stack)
        if (layer.slot == slot)
            return &layer;
    return nullptr;
}

// The layer's own image, as the file or library held it (no smoothing, no panel adjustment).
DecodedHeightTexture raw_image_of(Slic3r::TextureDisplacementLayer layer)
{
    layer.smoothing      = 0.f;
    layer.preview_adjust = Slic3r::TextureAdjustments{};
    return Slic3r::decode_height_texture(layer, /*keep_decoded=*/false);
}

// Resolves the call's texture (an id, a file, or for an add the library's first) into `plan`. Why not, or nothing.
std::optional<std::string> resolve_image(const LibraryCopy& library, EditPlan& plan)
{
    const LayerRequest& request = plan.request;
    std::string         error;
    if (request.file_path) {
        const std::filesystem::path file = std::filesystem::u8path(*request.file_path);
        std::error_code             ec;
        if (!std::filesystem::is_regular_file(file, ec))
            return "file_path " + *request.file_path + " is not a file";
        plan.file = read_image(*request.file_path, error);
        if (!plan.file)
            return error;
        return std::nullopt;
    }
    std::string query;
    if (request.texture)
        query = *request.texture;
    else if (request.action == LayerAction::add) {
        if (library.entries.empty())
            return "The texture library is empty, so a new layer has no texture: give file_path an image";
        plan.picked_entry = library.entries.front();
    }
    if (!request.texture && !plan.picked_entry)
        return std::nullopt;
    if (request.texture) {
        const TextureLookup found = find_texture(library.entries, library.ids, query);
        if (found.index < 0)
            return found.error;
        plan.picked_entry = library.entries[size_t(found.index)];
    }
    plan.picked = read_image(plan.picked_entry->path, error);
    if (!plan.picked)
        return "The texture " + plan.picked_entry->name + " could not be read: " + error;
    return std::nullopt;
}

// The image the call gives the layer, if any.
const DecodedHeightTexture* given_image(const EditPlan& plan)
{
    if (plan.file)
        return &plan.file->decoded;
    if (plan.picked)
        return &plan.picked->decoded;
    return nullptr;
}

// Everything the call decides, from the model as it is. Main thread; changes nothing.
std::optional<std::string> plan_edit(const nlohmann::json& params, const LayerRequest& request, EditPlan& plan)
{
    if (Slic3r::cgal_repair_dialog_running())
        return "The app is running its own mesh repair, so no texture layer was changed: call again once it has finished";
    Slic3r::GUI::Plater* plater = wxGetApp().plater();
    std::string          error;
    plan.request = request;
    plan.object  = resolve_object_id(params, plater->model(), plan.object_id, error);
    if (plan.object == nullptr)
        return error;
    if (const auto refusal = edit_job_refusal(!plater->get_ui_job_worker().is_idle(), "set_texture_displacement"))
        return refusal;
    plan.owner = texture_stack_owner(*plan.object);
    if (plan.owner == nullptr)
        return "Object " + std::to_string(plan.object_id) + " has no model part, so it cannot hold texture layers";

    const std::vector<Slic3r::TextureDisplacementLayer>& stack = plan.owner->texture_displacement_layers;
    plan.options = plan.owner->texture_displacement_options;
    if (request.layer)
        if (const auto* found = layer_in_slot(stack, *request.layer))
            plan.before = *found;
    plan.slot = request.layer ? *request.layer : first_free_slot(stack).value_or(0);
    plan.after = plan.before ? *plan.before : new_layer(plan.slot, plan.owner->has_texture_uv());

    const bool edits_layer = request.action == LayerAction::add || request.action == LayerAction::edit;
    LibraryCopy library;
    if (edits_layer && (request.texture || request.file_path || request.action == LayerAction::add)) {
        library = library_copy();
        if (const auto refusal = resolve_image(library, plan))
            return refusal;
    }
    if (const DecodedHeightTexture* image = given_image(plan)) {
        const std::shared_ptr<std::vector<unsigned char>>& data = plan.file ? plan.file->data : plan.picked->data;
        plan.after.image_data = data;
        plan.after.name       = plan.picked_entry ? plan.picked_entry->name : std::filesystem::u8path(*request.file_path).stem().u8string();
        plan.after.path       = plan.picked_entry ? plan.picked_entry->path : *request.file_path;
        plan.image_changed    = !plan.before || plan.before->empty() || !same_texture(raw_image_of(*plan.before), *image);
    }

    StackFacts facts;
    facts.owner_has_uv        = plan.owner->has_texture_uv();
    facts.physical_filaments  = int(Slic3r::GUI::GLGizmoTextureDisplacement::palette_inputs(plan.options, *wxGetApp().preset_bundle,
                                                                                            plater->get_extruders_colors()).filaments.size());
    facts.color_mix_count     = plan.options.color_mix_count;
    if (const DecodedHeightTexture* image = given_image(plan))
        facts.texture_has_color = image->has_color();
    else if (!plan.after.empty())
        facts.texture_has_color = Slic3r::height_texture_has_color(plan.after);

    LayerSettings settings = request.settings;
    bool          layer_changed = edits_layer && apply_layer_settings(plan.after, settings);
    bool          stack_changed = edits_layer && apply_stack_settings(plan.options, settings);
    if (const auto refusal = layer_edit_refusal(plan.object_id, stack, request, plan.before ? &*plan.before : nullptr, plan.after, facts))
        return refusal;

    bool view_changed = false;
    if (edits_layer) {
        const Slic3r::Transform3d to_world = Slic3r::texture_displacement_volume_to_world(*plan.owner);
        const bool became_view = plan.after.projection_method == Slic3r::TextureProjectionMethod::ViewProjected &&
                                 (!plan.before || plan.before->projection_method != Slic3r::TextureProjectionMethod::ViewProjected);
        std::optional<Axes> axes;
        if (settings.projection_frame) {
            const ProjectionFrameRequest& frame = *settings.projection_frame;
            const Axes frame_axes = frame.view ? view_axes_of(*frame.view) : Axes{*frame.right, *frame.up};
            const nlohmann::json* given = params.contains("projection_frame") ? &params.at("projection_frame") : nullptr;
            const Slic3r::Vec3d center = (given != nullptr && given->contains("center"))
                                             ? frame.center
                                             : Slic3r::Vec3d(plan.object->instance_bounding_box(0).center());
            const auto matrix = orthographic_frame_matrix(frame_axes.first, frame_axes.second, center, frame.width_mm, frame.height_mm, to_world);
            axes = frame_axes;
            const Slic3r::Transform3d owner_to_plate = volume_to_plate(*plan.object, *plan.owner, 0);
            if (!plate_transform_is_invertible(owner_to_plate))
                return "Object " + std::to_string(plan.object_id) + " is collapsed on an axis, so a frame cannot project through it";
            VisibleView view;
            view.forward = (-(frame_axes.first.cross(frame_axes.second))).normalized();
            if (frame.view && *frame.view == "current") {
                const Slic3r::GUI::Camera& camera = wxGetApp().plater()->get_camera();
                view.ortho = camera.get_type() == Slic3r::GUI::Camera::EType::Ortho;
                view.eye   = camera.get_position();
            }
            view.frame          = matrix;
            view.plate_to_frame = owner_to_plate.inverse();
            plan.frame_view     = view;
            view_changed |= !plan.after.view_project_projective || plan.after.view_project_matrix != matrix || plan.after.tile_enabled ||
                            plan.after.projection_method != Slic3r::TextureProjectionMethod::ViewProjected;
            plan.after.projection_method       = Slic3r::TextureProjectionMethod::ViewProjected;
            plan.after.view_project_projective = true;
            plan.after.view_project_matrix     = matrix;
            plan.after.tile_enabled            = false;
        } else if (settings.view_axes) {
            axes = *settings.view_axes;
        } else if (settings.view_direction) {
            axes = view_axes_of(*settings.view_direction);
        } else if (became_view) {
            axes = view_axes_of("current"); // as the panel captures the camera on choosing From view
        }
        if (axes) {
            const auto [right, up] = view_projector_axes(axes->first, axes->second, to_world);
            if (!nearly_same(plan.after.view_project_right, right) || !nearly_same(plan.after.view_project_up, up) || became_view)
                view_changed = true;
            plan.after.view_project_right = right;
            plan.after.view_project_up    = up;
        }
        if (plan.frame_view && !view_changed)
            plan.frame_view.reset();
        if (settings.adjust_image) {
            const DecodedHeightTexture* given = given_image(plan);
            const DecodedHeightTexture  source = given != nullptr ? *given : raw_image_of(plan.after);
            DecodedHeightTexture        adjusted = Slic3r::apply_texture_adjustments(source, adjustments_of(*settings.adjust_image));
            if (!same_texture(adjusted, source)) {
                plan.adjusted     = std::move(adjusted);
                plan.adjust_stem  = plan.after.name.empty() ? std::string("texture") : plan.after.name;
            }
        }
    }

    switch (request.action) {
    case LayerAction::add:
        plan.changed  = true;
        plan.snapshot = _u8L("Add texture displacement layer");
        break;
    case LayerAction::remove:
        plan.changed  = true;
        plan.snapshot = _u8L("Remove texture displacement layer");
        break;
    case LayerAction::move: {
        plan.move_from = layer_position(stack, plan.slot).value_or(0);
        plan.move_to   = *request.to_position;
        plan.changed   = plan.move_from != plan.move_to;
        plan.snapshot  = _u8L("Reorder texture displacement layers");
        break;
    }
    case LayerAction::edit: {
        const bool image = plan.image_changed || plan.adjusted.has_value();
        plan.changed     = layer_changed || stack_changed || view_changed || image;
        const bool frame_only = settings.projection_frame && !layer_changed && !stack_changed && !image;
        plan.snapshot = image && !layer_changed && !stack_changed && !view_changed ? _u8L("Change texture displacement texture")
                        : frame_only ? _u8L("Apply texture projection frame")
                                     : _u8L("Texture displacement editing");
        break;
    }
    }
    return std::nullopt;
}

// What the answer needs of the model once the edit is done, copied so the mask reports can be made off the main thread.
struct PendingLayer
{
    bool                                               present = false;
    Slic3r::TextureDisplacementLayer                   layer;
    int                                                position = 0;
    LayerTexture                                       texture;
    std::optional<bool>                                color_capable;
    std::vector<Slic3r::TextureDisplacementLayer>      stack;
    Slic3r::TextureDisplacementOptions                 options;
    Slic3r::Transform3d                                to_world = Slic3r::Transform3d::Identity();
    bool                                               owner_has_uv = false;
    std::vector<MaskInput>                             inputs;
};

PendingLayer capture_layer(const Slic3r::ModelObject& object, const Slic3r::ModelVolume& owner, int slot)
{
    PendingLayer out;
    const auto*  layer = layer_in_slot(owner.texture_displacement_layers, slot);
    if (layer == nullptr)
        return out;
    out.present      = true;
    out.layer        = *layer;
    out.stack        = owner.texture_displacement_layers;
    out.options      = owner.texture_displacement_options;
    out.position     = layer_position(out.stack, slot).value_or(0);
    out.to_world     = Slic3r::texture_displacement_volume_to_world(owner);
    out.owner_has_uv = owner.has_texture_uv();
    if (!layer->empty())
        out.color_capable = Slic3r::height_texture_has_color(*layer);
    const LibraryCopy library = library_copy();
    if (const auto index = library_index_of(library.entries, *layer)) {
        out.texture.id     = library.ids[size_t(*index)];
        out.texture.source = texture_source_name(library.entries[size_t(*index)].source);
    }
    for (size_t i = 0; i < object.volumes.size(); ++i) {
        const Slic3r::ModelVolume* v = object.volumes[i];
        if (!v->is_model_part())
            continue;
        MaskInput input;
        input.volume_id         = int(i);
        input.name              = v->name;
        input.mesh              = v->mesh_ptr();
        input.masks[size_t(slot)] = v->texture_displacement_facet(slot).get_data();
        out.inputs.push_back(std::move(input));
    }
    return out;
}

// The faces visible through the frame this call set, painted onto the layer in `slot` as the panel's Apply does (every
// model part, one part hiding another). Returns the number of facets painted.
int paint_frame(const EditPlan& plan, int slot)
{
    PaintTarget everything;
    everything.object       = plan.object;
    everything.object_id    = plan.object_id;
    everything.instance_idx = 0;
    for (int i = 0; i < int(plan.object->volumes.size()); ++i)
        if (plan.object->volumes[size_t(i)]->is_model_part()) {
            everything.volumes.push_back(plan.object->volumes[size_t(i)]);
            everything.volume_ids.push_back(i);
        }
    const PaintPlan                    paint_plan  = capture_paint_plan(everything);
    const std::vector<FacetAssignment> assignments = assign_visible(paint_plan.volumes, paint_plan.volumes, *plan.frame_view, 1);
    int                                painted     = 0;
    for (size_t i = 0; i < paint_plan.volumes.size() && i < assignments.size(); ++i) {
        const PaintPlanVolume& pv = paint_plan.volumes[i];
        apply_facet_states(*everything.volumes[size_t(pv.index)], PaintChannel::texture(slot), assignments[i].states, /*replace=*/false);
        painted += int(assignments[i].states.size()) - assignments[i].unassigned;
    }
    return painted;
}

// Writes the plan into the model. Main thread, after the snapshot. Returns the slot the layer is in now (-1 after a remove).
int write_edit(EditPlan& plan)
{
    Slic3r::ModelVolume& owner = *plan.owner;
    auto&                stack = owner.texture_displacement_layers;
    switch (plan.request.action) {
    case LayerAction::add:
        stack.push_back(plan.after);
        plan.options.active_slot = plan.slot;
        owner.texture_displacement_options = plan.options;
        return plan.slot;
    case LayerAction::edit:
        for (Slic3r::TextureDisplacementLayer& layer : stack)
            if (layer.slot == plan.slot)
                layer = plan.after;
        owner.texture_displacement_options = plan.options;
        return plan.slot;
    case LayerAction::remove: {
        Slic3r::GUI::remove_texture_layer(*plan.object, plan.slot);
        Slic3r::TextureDisplacementOptions& options = owner.texture_displacement_options;
        if (options.active_slot == plan.slot) {
            int lowest = 0;
            if (!stack.empty())
                lowest = std::min_element(stack.begin(), stack.end(), [](const auto& a, const auto& b) { return a.slot < b.slot; })->slot;
            options.active_slot = lowest;
        }
        return -1;
    }
    case LayerAction::move: {
        int active = owner.texture_displacement_options.active_slot;
        Slic3r::GUI::move_texture_layer(*plan.object, plan.slot, move_insertion_index(plan.move_from, plan.move_to), [&](int a, int b) {
            if (active == a)
                active = b;
            else if (active == b)
                active = a;
        });
        owner.texture_displacement_options.active_slot = active;
        std::vector<int> slots;
        for (const auto& layer : stack)
            slots.push_back(layer.slot);
        std::sort(slots.begin(), slots.end());
        return slots[size_t(plan.move_to)];
    }
    }
    return plan.slot;
}

// The edit on the main thread. `closed_tool` names a toolbar tool it closed. Fills `pending` for the answer's layer.
nlohmann::json edit_stack(const nlohmann::json& params, const LayerRequest& request, PendingLayer& pending, std::string& closed_tool)
{
    wxLogNull                 no_log; // a bad image would otherwise open a log window at idle
    McpDialogSuppressionGuard guard;
    Slic3r::GUI::Plater*      plater = wxGetApp().plater();
    EditPlan                  plan;
    if (const auto refusal = plan_edit(params, request, plan))
        return guard.report(error_response(*refusal));

    nlohmann::json answer = {{"status", "success"},
                             {"changed", plan.changed},
                             {"action", request.action == LayerAction::add ? "add" : request.action == LayerAction::edit ? "edit"
                                        : request.action == LayerAction::remove ? "remove" : "move"},
                             {"object_id", plan.object_id}};
    const auto finish = [&](nlohmann::json result) { return guard.report(with_closed_tool(std::move(result), closed_tool)); };
    const auto fail   = [&](const std::string& message) { return finish(error_response(message)); };

    int  slot = plan.slot;
    bool had_paint = false;
    int  facets_painted = -1;
    std::string removed_name;
    if (plan.changed) {
        // Every check is behind us and the call changes something: close an open panel, as the user would. Closing takes
        // the panel's own undo step and resets its live image adjustment, so the model is read again as it is now, and
        // the checks run again on it, before the library or the scene is touched.
        if (const auto failed = close_toolbar_tool_for_change(*plater, plan.object_id, plan.object->volumes.size(), closed_tool))
            return guard.report(with_closed_tool(*failed, closed_tool));
        if (!closed_tool.empty()) {
            plan = EditPlan{};
            if (const auto refusal = plan_edit(params, request, plan))
                return fail(*refusal);
            answer["changed"] = plan.changed;
            slot              = plan.slot;
        }
    }
    if (plan.changed) {
        // The images go to the library: a file that cannot be written leaves the scene as it was. With adjust_image
        // only the adjusted copy is kept, not the original it came from.
        std::optional<LibraryImage> stored;
        std::string                 error;
        if (plan.adjusted)
            stored = save_or_reuse(*plan.adjusted, plan.adjust_stem, error);
        else if (plan.file)
            stored = import_or_reuse(*request.file_path, plan.file->decoded, error);
        if ((plan.file || plan.adjusted) && !stored)
            return fail(error);
        if (stored) {
            plan.after.image_data = stored->data;
            plan.after.name       = stored->entry.name;
            plan.after.path       = stored->entry.path;
            plan.after.preview_adjust = Slic3r::TextureAdjustments{};
            const LibraryCopy library = library_copy();
            const auto        index   = library_index_of(library.entries, plan.after);
            answer["imported_texture"] = {{"texture", index ? nlohmann::json(library.ids[size_t(*index)]) : nlohmann::json(nullptr)},
                                          {"name", stored->entry.name},
                                          {"path", stored->entry.path},
                                          {"reused", stored->reused}};
        }
        if (plan.picked_entry && plan.picked_entry->source == TextureSource::User)
            touch_user_texture(plan.picked_entry->path);
        if (stored && stored->entry.source == TextureSource::User)
            touch_user_texture(stored->entry.path);

        had_paint = request.action == LayerAction::remove && slot_painted_fast(*plan.object, plan.slot);
        removed_name = plan.before ? plan.before->name : std::string();

        plater->take_snapshot(plan.snapshot);
        slot = write_edit(plan);
        // As the panel does on applying a frame: what is visible inside it is painted onto the layer.
        if (plan.frame_view) {
            facets_painted = paint_frame(plan, slot);
            answer["facets_painted"] = facets_painted;
        }
        if (Slic3r::GUI::GLCanvas3D* canvas = plater->get_view3D_canvas3D())
            canvas->set_as_dirty();
    }

    answer["stack_options"] = stack_options_json(plan.owner->texture_displacement_options);
    answer["needs_bake"]    = needs_bake_fast(*plan.object, plan.owner);
    answer["plate_index"]   = object_plate_index(*wxGetApp().plater(), plan.object_id);
    if (request.action == LayerAction::remove) {
        answer["removed_layer"] = {{"layer", plan.slot}, {"name", removed_name}, {"had_paint", had_paint}};
    } else {
        pending = capture_layer(*plan.object, *plan.owner, slot);
    }
    return finish(std::move(answer));
}

nlohmann::json set_texture_displacement(const nlohmann::json& params)
{
    std::string error;
    const auto  request = read_layer_request(params, error);
    if (!request)
        return error_response(error);

    PendingLayer pending;
    std::string  closed_tool;
    nlohmann::json answer = run_on_main_thread([&]() { return edit_stack(params, *request, pending, closed_tool); });
    if (answer.value("status", "") != "success" || !pending.present)
        return answer;

    // The mask reports walk the paint, so they are made here, off the main thread, from the copies.
    const auto reports = mask_reports(pending.inputs);
    std::vector<MaskReport> paint;
    for (const auto& row : reports)
        paint.push_back(row[size_t(pending.layer.slot)]);
    answer["layer"] = layer_json(pending.layer, pending.position, pending.position == 0, pending.texture, pending.color_capable, paint,
                                 pending.owner_has_uv, pending.stack, pending.options, pending.to_world);
    if (request->action != LayerAction::remove)
        add_next_steps(answer, texture_paint_next_steps(answer.at("object_id").get<int>(), pending.layer.slot, answer["layer"].at("painted").get<bool>(),
                                                        answer.value("needs_bake", false)));
    else
        add_next_steps(answer, bake_next_steps(answer.at("object_id").get<int>(), answer.value("needs_bake", false)));
    append_next_steps(answer, texture_preview_next_steps(answer.value("plate_index", -1), answer.at("object_id").get<int>(), answer.value("needs_bake", false)));
    return answer;
}


// ---- Reading the stack ----

// What get_texture_displacement needs of the model, copied so the mask reports can be made off the main thread.
struct PendingStack
{
    int plate_index = -1;   // the plate holding the object's first instance
    std::vector<StackLayerFacts>                               layers;
    Slic3r::TextureDisplacementOptions                         options;
    Slic3r::Transform3d                                        to_world     = Slic3r::Transform3d::Identity();
    bool                                                       owner_has_uv = false;
    int                                                        owner_volume_id = -1;
    std::vector<MaskInput>                                     inputs;
    nlohmann::json                                             ignored = nlohmann::json::array();
    nlohmann::json                                             parts   = nlohmann::json::array();
    std::vector<Slic3r::Vec2f>                       model_uv;    // its UV channel, if it has one
    indexed_triangle_set                             mesh;        // the stack owner's, for the recommended resolution
};

// Every model part with its eight masks, and the stacks on parts that do not own it.
void capture_parts(const Slic3r::ModelObject& object, const Slic3r::ModelVolume& owner, PendingStack& out)
{
    for (size_t i = 0; i < object.volumes.size(); ++i) {
        const Slic3r::ModelVolume* v = object.volumes[i];
        if (v != &owner && !v->texture_displacement_layers.empty())
            out.ignored.push_back({{"volume_id", i}, {"name", v->name}, {"layers", v->texture_displacement_layers.size()}});
        if (v == &owner)
            out.owner_volume_id = int(i);
        if (!v->is_model_part())
            continue;
        MaskInput input;
        input.volume_id = int(i);
        input.name      = v->name;
        input.mesh      = v->mesh_ptr();
        for (int slot = 0; slot < int(Slic3r::TEXTURE_DISPLACEMENT_MAX_LAYERS); ++slot)
            input.masks[size_t(slot)] = v->texture_displacement_facet(slot).get_data();
        out.inputs.push_back(std::move(input));
        out.parts.push_back({{"volume_id", i}, {"name", v->name}});
    }
}

nlohmann::json capture_stack(const nlohmann::json& params, std::optional<int> only_volume, PendingStack& out, int& object_id)
{
    std::string          error;
    Slic3r::GUI::Plater* plater = wxGetApp().plater();
    Slic3r::ModelObject* object = resolve_object_id(params, plater->model(), object_id, error);
    if (object == nullptr)
        return error_response(error);
    if (only_volume) {
        if (const auto refusal = volume_id_error(object_id, *object, *only_volume))
            return error_response(*refusal);
        if (!object->volumes[size_t(*only_volume)]->is_model_part())
            return error_response("volume_id " + std::to_string(*only_volume) + " of object " + std::to_string(object_id) +
                                  " is not a model part, and only a model part holds texture paint");
    }
    const Slic3r::ModelVolume* owner = texture_stack_owner(*object);
    if (owner == nullptr)
        return error_response("Object " + std::to_string(object_id) + " has no model part, so it holds no texture layers");

    out.options      = owner->texture_displacement_options;
    out.to_world     = Slic3r::texture_displacement_volume_to_world(*owner);
    out.owner_has_uv = owner->has_texture_uv();
    out.plate_index  = object_plate_index(*plater, object_id);
    out.mesh         = owner->mesh().its;
    if (owner->has_texture_uv())
        out.model_uv = owner->texture_uv;
    const LibraryCopy library = library_copy();
    for (const Slic3r::TextureDisplacementLayer& layer : owner->texture_displacement_layers) {
        StackLayerFacts facts;
        facts.layer = layer; // holds the image's bytes: whether it has colours is read off the main thread
        if (const auto index = library_index_of(library.entries, layer)) {
            facts.texture.id     = library.ids[size_t(*index)];
            facts.texture.source = texture_source_name(library.entries[size_t(*index)].source);
        }
        out.layers.push_back(std::move(facts));
    }
    capture_parts(*object, *owner, out);
    return {{"status", "success"}};
}

nlohmann::json get_texture_displacement(const nlohmann::json& params)
{
    std::string error;
    const auto  only_volume = read_volume_id(params, error);
    if (!error.empty())
        return error_response(error);

    PendingStack pending;
    int          object_id = -1;
    nlohmann::json captured = run_on_main_thread([&]() { return capture_stack(params, only_volume, pending, object_id); });
    if (captured.value("status", "") != "success")
        return captured;

    // The mask reports walk the paint, so they are made here, off the main thread, from the copies.
    for (StackLayerFacts& facts : pending.layers)
        if (!facts.layer.empty())
            facts.color_capable = Slic3r::height_texture_has_color(facts.layer);
    const auto reports = mask_reports(pending.inputs);
    StackAnswer stack  = texture_stack_json(pending.layers, reports, only_volume, pending.owner_has_uv, pending.options, pending.to_world);

    nlohmann::json answer = std::move(stack.json);
    std::vector<Slic3r::TextureDisplacementLayer> stack_layers;
    for (const StackLayerFacts& facts : pending.layers)
        stack_layers.push_back(facts.layer);
    const ResolutionAdvice advice = resolution_advice(stack_layers, {{&pending.mesh, pending.to_world, pending.model_uv.empty() ? nullptr : &pending.model_uv}},
                                                      pending.options.v2_refine_mm);
    answer["recommended_resolution_mm"] = recommended_resolution_json(advice);
    if (const std::string text = resolution_alias_warning(advice); !text.empty())
        answer["resolution_warning"] = text;
    answer["status"]          = "success";
    answer["object_id"]       = object_id;
    answer["stack_volume_id"] = pending.owner_volume_id;
    answer["parts"]           = pending.parts;
    answer["ignored_stacks"]  = pending.ignored;
    if (only_volume)
        answer["volume_id"] = *only_volume;
    const bool needs_bake = answer.value("needs_bake", false);
    if (stack.first_unpainted_layer)
        add_next_steps(answer, texture_paint_next_steps(object_id, *stack.first_unpainted_layer, false, needs_bake));
    else
        add_next_steps(answer, bake_next_steps(object_id, needs_bake));
    answer["plate_index"] = pending.plate_index;
    append_next_steps(answer, texture_preview_next_steps(pending.plate_index, object_id, needs_bake));
    return answer;
}


// ---- The unwrap of a layer ----

// Everything the unwrap work reads, copied on the main thread: the unwrap is worked out from these on the HTTP thread.
struct UnwrapInput
{
    int                                         object_id = -1;
    int                                         slot      = -1;
    Slic3r::ObjectID                            object_oid, owner_oid;
    Slic3r::TextureDisplacementLayer            layer;
    std::shared_ptr<const Slic3r::TriangleMesh> mesh;
    PaintData                                   mask;
    Slic3r::Transform3d                         to_world = Slic3r::Transform3d::Identity();
    Slic3r::Transform3d                         to_plate = Slic3r::Transform3d::Identity();

    // The same object, mesh, paint and unwrap state: what a plan made from `other` still holds for.
    bool same_inputs(const UnwrapInput& other) const
    {
        return object_oid == other.object_oid && owner_oid == other.owner_oid && mesh == other.mesh && mask == other.mask &&
               to_world.matrix() == other.to_world.matrix() && to_plate.matrix() == other.to_plate.matrix() &&
               same_unwrap_state(layer, other.layer) && layer.projection_method == other.layer.projection_method;
    }
};

// The slot a call names, 0..7.
std::optional<int> read_slot(const nlohmann::json& params, std::string& error)
{
    int slot = -1;
    if (!params.contains("layer") || !parse_integer_param(params.at("layer"), slot) || slot < 0 || slot >= int(Slic3r::TEXTURE_DISPLACEMENT_MAX_LAYERS)) {
        error = "layer must be the slot of a texture layer, a whole number from 0 to " + std::to_string(int(Slic3r::TEXTURE_DISPLACEMENT_MAX_LAYERS) - 1) +
                " (get_texture_displacement lists them)";
        return std::nullopt;
    }
    return slot;
}

// The refusals both unwrap tools share, and the copies. A null json when all is well, else the error answer. Main thread.
nlohmann::json capture_unwrap_input(const nlohmann::json& params, int slot, bool for_edit, UnwrapInput& in)
{
    std::string          error;
    Slic3r::GUI::Plater* plater = wxGetApp().plater();
    if (for_edit) {
        if (Slic3r::cgal_repair_dialog_running())
            return error_response("The app is running its own mesh repair, so no unwrap was changed: call again once it has finished");
    }
    Slic3r::ModelObject* object = resolve_object_id(params, plater->model(), in.object_id, error);
    if (object == nullptr)
        return error_response(error);
    if (for_edit)
        if (const auto refusal = edit_job_refusal(!plater->get_ui_job_worker().is_idle(), "set_texture_unwrap"))
            return error_response(*refusal);
    const Slic3r::ModelVolume* owner = texture_stack_owner(*object);
    if (owner == nullptr)
        return error_response("Object " + std::to_string(in.object_id) + " has no model part, so it holds no texture layers");
    const Slic3r::TextureDisplacementLayer* layer = layer_in_slot(owner->texture_displacement_layers, slot);
    if (layer == nullptr)
        return error_response("Object " + std::to_string(in.object_id) + " has no texture displacement layer in slot " + std::to_string(slot) +
                              " (get_texture_displacement lists its layers)");
    if (layer->projection_method != Slic3r::TextureProjectionMethod::LSCM)
        return error_response("Layer " + std::to_string(slot) + " of object " + std::to_string(in.object_id) + " maps its texture by " +
                              projection_name(layer->projection_method) + ", and only an unwrap layer has an unwrap: set_texture_displacement with projection unwrap makes it one");
    if (object->instances.empty())
        return error_response("Object " + std::to_string(in.object_id) + " has no instance, so it has no place to unwrap in");
    int owner_index = 0;
    for (size_t i = 0; i < object->volumes.size(); ++i)
        if (object->volumes[i] == owner)
            owner_index = int(i);
    in.slot      = slot;
    in.object_oid = object->id();
    in.owner_oid  = owner->id();
    in.layer      = *layer;
    in.mesh       = owner->mesh_ptr();
    in.mask       = owner->texture_displacement_facet(slot).get_data();
    in.to_world   = Slic3r::texture_displacement_volume_to_world(*owner);
    in.to_plate   = volume_to_plate(*object, *owner, 0);
    if (mask_is_stale(in.mask, in.mesh->its.indices.size()))
        return error_response("Layer " + std::to_string(slot) + "'s paint on the owner part (volume " + std::to_string(owner_index) +
                              ") indexes triangles its mesh no longer has, so there is no patch to unwrap: paint the layer again (paint_object, mode texture)");
    if (!mask_has_paint(in.mask, in.mesh->its.indices.size()))
        return error_response("Layer " + std::to_string(slot) + " is painted on no triangle of object " + std::to_string(in.object_id) + "'s first model part, "
                              "the part that owns the layers and the only one whose unwrap counts: paint it there first (paint_object, mode texture, texture_layer " +
                              std::to_string(slot) + ")");
    return nlohmann::json();
}

nlohmann::json unwrap_answer(const UnwrapInput& in, const Slic3r::TextureDisplacementLayer& layer, const Slic3r::PatchUnwrap& unwrap, bool include_vertices)
{
    nlohmann::json answer = unwrap_json(unwrap, layer, include_vertices);
    answer["status"]    = "success";
    answer["object_id"] = in.object_id;
    answer["layer"]     = in.slot;
    return answer;
}

nlohmann::json get_texture_unwrap(const nlohmann::json& params)
{
    std::string error;
    const auto  slot = read_slot(params, error);
    if (!slot)
        return error_response(error);
    bool include_vertices = false;
    if (params.contains("include_vertices") && !parse_boolean_param(params.at("include_vertices"), include_vertices))
        return error_response("include_vertices must be true or false; got " + params.at("include_vertices").dump() + ": omit it for no vertex list");

    UnwrapInput          in;
    const nlohmann::json refusal = run_on_main_thread([&]() { return capture_unwrap_input(params, *slot, /*for_edit=*/false, in); });
    if (!refusal.is_null())
        return refusal;

    // Pure mesh work on the copies, off the main thread. The solve cannot be interrupted, so the call's time and a quit are
    // checked before it and after it.
    using Clock                = std::chrono::steady_clock;
    const auto deadline        = Clock::now() + tool_wait_cap();
    const auto stopped         = [&]() { return Slic3r::this_thread_cancelled() || Clock::now() > deadline; };
    const auto stopped_answer = [&]() {
        return error_response(Slic3r::this_thread_cancelled() ? "The unwrap was stopped (the app is quitting)"
                                                              : "The unwrap took longer than this call may wait, so it is not answered: call again (the solve is kept, so "
                                                                "the next call finds it ready), or paint a smaller patch");
    };
    const indexed_triangle_set patch = unwrap_patch(*in.mesh, in.mask, in.to_world);
    if (patch.indices.empty())
        return error_response("Layer " + std::to_string(*slot) + " has no painted triangle to unwrap");
    if (stopped())
        return stopped_answer();
    const Slic3r::PatchUnwrap unwrap = layer_unwrap(patch, in.layer);
    if (stopped())
        return stopped_answer();
    if (unwrap.empty())
        return error_response("The layer's paint leaves nothing to unwrap");
    return unwrap_answer(in, in.layer, unwrap, include_vertices);
}

// The unwrap tool's change on the main thread. `expected` is what the plan was made from. An open
// toolbar tool is closed first, only now that the call changes something; closing flushes the tool's paint into the model,
// so the model is read again, and when it is no longer what was planned `replan` gets the new input (nothing is applied).
nlohmann::json apply_unwrap_edit(const nlohmann::json& params, const UnwrapInput& expected, const UnwrapEdit& edit, std::string& closed_tool,
                                 std::optional<UnwrapInput>& replan)
{
    wxLogNull                 no_log;
    McpDialogSuppressionGuard guard;
    Slic3r::GUI::Plater*      plater = wxGetApp().plater();
    const auto                finish = [&](nlohmann::json result) { return guard.report(with_closed_tool(std::move(result), closed_tool)); };

    UnwrapInput now;
    if (const nlohmann::json refusal = capture_unwrap_input(params, expected.slot, /*for_edit=*/true, now); !refusal.is_null())
        return finish(refusal);
    const auto changed_meanwhile = []() {
        return error_response("The layer, its paint or the mesh changed while the unwrap was worked out, so nothing was changed: call again");
    };
    if (!now.same_inputs(expected))
        return finish(changed_meanwhile());

    std::string          unused;
    Slic3r::ModelObject* object = resolve_object_id(params, plater->model(), now.object_id, unused);
    {
        std::string closed_now;
        const auto  failed = close_toolbar_tool_for_change(*plater, now.object_id, object->volumes.size(), closed_now);
        if (!closed_now.empty())
            closed_tool = closed_now;
        if (failed)
            return guard.report(with_closed_tool(*failed, closed_tool));
        if (!closed_now.empty()) {
            UnwrapInput after;
            if (const nlohmann::json refusal = capture_unwrap_input(params, expected.slot, /*for_edit=*/true, after); !refusal.is_null())
                return finish(refusal);
            if (!after.same_inputs(expected)) {
                replan = std::move(after);
                return nlohmann::json();
            }
            object = resolve_object_id(params, plater->model(), now.object_id, unused); // the same object, found again
        }
    }
    Slic3r::ModelVolume* owner = texture_stack_owner(*object);
    for (Slic3r::TextureDisplacementLayer& layer : owner->texture_displacement_layers)
        if (layer.slot == expected.slot) {
            plater->take_snapshot(edit.snapshot);
            copy_unwrap_state(edit.layer, layer);
        }
    if (Slic3r::GUI::GLCanvas3D* canvas = plater->get_view3D_canvas3D())
        canvas->set_as_dirty();
    return finish({{"status", "success"}});
}

nlohmann::json set_texture_unwrap(const nlohmann::json& params)
{
    std::string error;
    const auto  request = read_unwrap_request(params, error);
    if (!request)
        return error_response(error);
    const auto slot = read_slot(params, error);
    if (!slot)
        return error_response(error);

    using Clock               = std::chrono::steady_clock;
    const auto deadline       = Clock::now() + tool_wait_cap();
    const auto cancelled      = [deadline] { return Slic3r::this_thread_cancelled() || Clock::now() > deadline; };
    std::string closed_tool;

    UnwrapInput          in;
    const nlohmann::json refusal = run_on_main_thread([&]() { return capture_unwrap_input(params, *slot, /*for_edit=*/true, in); });
    if (!refusal.is_null())
        return refusal;

    // The plan: pure mesh work on the copies, off the main thread.
    const auto plan = [&](const UnwrapInput& input, std::string& why) -> std::optional<UnwrapEdit> {
        const indexed_triangle_set patch = unwrap_patch(*input.mesh, input.mask, input.to_world);
        if (patch.indices.empty()) {
            why = "Layer " + std::to_string(input.slot) + " has no painted triangle to unwrap";
            return std::nullopt;
        }
        return edit_unwrap(input.layer, *request, input.mesh->its, patch, input.to_plate, cancelled, why);
    };
    const auto answer_of = [&](const UnwrapInput& input, const UnwrapEdit& edit) {
        nlohmann::json answer = unwrap_answer(input, edit.layer, edit.unwrap, /*include_vertices=*/false);
        answer["changed"]           = edit.changed;
        answer["charts_renumbered"] = edit.resolved;
        if (!edit.snapped_paths.empty()) {
            nlohmann::json snapped = nlohmann::json::array();
            for (const auto& [from, to] : edit.snapped_paths)
                snapped.push_back({{"from_vertex", from}, {"to_vertex", to}});
            answer["seam_paths_snapped"] = snapped;
        }
        if (!edit.notes.empty())
            answer["notes"] = edit.notes;
        return with_closed_tool(std::move(answer), closed_tool);
    };

    std::optional<UnwrapEdit> edit = plan(in, error);
    if (!edit)
        return error_response(error);
    // Each round closes an open toolbar tool again: one reopened while the plan was redone is closed before the write.
    for (int round = 0; round < 3 && edit->changed; ++round) {
        std::optional<UnwrapInput> replan;
        const nlohmann::json       applied = run_on_main_thread([&]() { return apply_unwrap_edit(params, in, *edit, closed_tool, replan); });
        if (!replan) {
            if (applied.value("status", "") != "success")
                return applied;
            return answer_of(in, *edit);
        }
        // The panel was open and closing it changed the paint the plan read: plan again on the model as it is now.
        in   = std::move(*replan);
        edit = plan(in, error);
        if (!edit)
            return with_closed_tool(error_response(error), closed_tool);
        if (round == 2)
            return with_closed_tool(error_response("The paint kept changing while the unwrap was worked out, so nothing was changed: call again"), closed_tool);
    }
    return answer_of(in, *edit);
}


// A number property held to the panel's range for `key` (layer_setting_bounds), so the schema and the reader cannot drift.
nlohmann::json bounded_property(const char* key, const char* type, const std::string& description)
{
    const SettingBound* bound = setting_bound(key);
    nlohmann::json      out   = {{"type", type}, {"description", description}};
    if (bound != nullptr) {
        out["minimum"] = std::string(type) == "integer" ? nlohmann::json(long(bound->min)) : nlohmann::json(bound->min);
        out["maximum"] = std::string(type) == "integer" ? nlohmann::json(long(bound->max)) : nlohmann::json(bound->max);
    }
    return out;
}

nlohmann::json bounded_range(double min, double max, const std::string& description)
{
    return {{"type", "number"}, {"minimum", min}, {"maximum", max}, {"description", description}};
}

nlohmann::json vector3_property(const std::string& description)
{
    return {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 3}, {"description", description}};
}

nlohmann::json axes_properties()
{
    return {{"right", vector3_property("The projector's right axis in plate axes")}, {"up", vector3_property("The projector's up axis in plate axes")}};
}

nlohmann::json int_pair_list(const std::string& description)
{
    return {{"type", "array"},
            {"items", {{"type", "array"}, {"items", {{"type", "integer"}, {"minimum", 0}}}, {"minItems", 2}, {"maxItems", 2}}},
            {"description", description}};
}

nlohmann::json int_list(const std::string& description)
{
    return {{"type", "array"}, {"items", {{"type", "integer"}, {"minimum", 0}}}, {"description", description}};
}

nlohmann::json set_texture_unwrap_schema()
{
    const nlohmann::json point2 = {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 2}, {"maxItems", 2}};
    nlohmann::json       island = {
        {"type", "object"},
        {"properties", {{"chart", {{"type", "integer"}, {"minimum", 0}, {"description", "The chart (island) to place"}}},
                        {"offset", {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 2}, {"maxItems", 2}, {"description", "Where the island sits, in the unwrap's mm"}}},
                        {"rotation_deg", {{"type", "number"}, {"description", "Rotation about the island's own centre, in degrees"}}},
                        {"scale", {{"type", "number"}, {"minimum", 0.001}, {"maximum", 1000}, {"description", "Size factor about the island's own centre (1: the size the unwrap gave it)"}}},
                        // What get_texture_unwrap reports beside the placement, accepted and ignored.
                        {"centroid", point2},
                        {"group", {{"type", "integer"}}},
                        {"area_mm2", {{"type", "number"}}},
                        {"bounding_box", {{"type", {"object", "null"}}}},
                        {"neighbours", {{"type", "array"}, {"items", {{"type", "integer"}}}}}}},
        {"required", {"chart"}},
        {"additionalProperties", false}};
    nlohmann::json properties = {
        {"object_id", {{"type", "integer"}, {"minimum", 0}, {"description", "Object index (0-based)"}}},
        {"layer", {{"type", "integer"}, {"minimum", 0}, {"maximum", int(Slic3r::TEXTURE_DISPLACEMENT_MAX_LAYERS) - 1}, {"description", "The slot of an unwrap layer that is painted on the object's first model part"}}},
        {"unwrap_id", {{"type", "string"}, {"description", "The unwrap_id get_texture_unwrap (or the last answer) gave: with it, chart and vertex numbers are refused if the paint, the mesh or the seams changed since. Without it they are taken as they are"}}},
        {"seam_angle_deg", bounded_range(5., 90., "Edges sharper than this cut the unwrap into islands (the panel's Seam angle); once the layer has any seam, only seams cut")},
        {"auto_connect_islands", {{"type", "boolean"}, {"description", "Lay islands out as a connected net; turning it on lays them out now"}}},
        {"add_seams", int_pair_list("Mesh edges to cut along, as [vertex, vertex] pairs of the owner part's mesh")},
        {"remove_seams", int_pair_list("Seams to take away, as [vertex, vertex] pairs")},
        {"seam_paths", {{"type", "array"},
                        {"items", {{"type", "object"},
                                   {"properties", {{"from", vector3_property("Plate mm; snapped to the nearest vertex of the owner part")},
                                                   {"to", vector3_property("Plate mm; snapped to the nearest vertex of the owner part")}}},
                                   {"required", {"from", "to"}},
                                   {"additionalProperties", false}}},
                        {"description", "Seams along the shortest path over mesh edges between two points"}}},
        {"clear_seams", {{"type", "boolean"}, {"description", "Remove every seam first"}}},
        {"cut_islands", int_list("Charts to cut in two across their longest axis (a seam is added)")},
        {"relayout", {{"type", "boolean"}, {"description", "Lay the islands out as a connected net again"}}},
        {"islands", {{"type", "array"}, {"items", island}, {"description", "Place islands by chart; only the fields given change. An entry of get_texture_unwrap's islands is accepted as it is"}}},
        {"join", {{"type", "array"},
                  {"items", {{"type", "object"},
                             {"properties", {{"chart", {{"type", "integer"}, {"minimum", 0}}},
                                             {"to", {{"type", "integer"}, {"minimum", 0}, {"description", "The neighbour to join to (default: the nearest one)"}}}}},
                             {"required", {"chart"}},
                             {"additionalProperties", false}}},
                  {"description", "Unfold a chart against a neighbour it shares an edge with and group the two, so they are one island"}}},
        {"unjoin", int_list("Charts to put back at their own packed place and out of their group")},
        {"average_scale", {{"type", "boolean"}, {"description", "Give every island the mean of the islands' scales"}}},
        {"clear_uv_edits", {{"type", "boolean"}, {"description", "Drop every vertex edit first"}}},
        {"uv_edits", {{"type", "array"},
                      {"items", {{"type", "object"},
                                 {"properties", {{"vertex", {{"type", "integer"}, {"minimum", 0}, {"description", "An unwrapped vertex, as get_texture_unwrap lists with include_vertices"}}},
                                                 {"uv", {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 2}, {"maxItems", 2}, {"description", "Its place in the unwrap's mm, before the island's placement"}}}}},
                                 {"required", {"vertex", "uv"}},
                                 {"additionalProperties", false}}},
                      {"description", "Move unwrapped vertices; the edit rides with its island"}}}};
    return {{"type", "object"}, {"properties", properties}, {"required", {"object_id", "layer"}}};
}

nlohmann::json set_texture_displacement_schema()
{
    nlohmann::json frame_properties = axes_properties();
    frame_properties["view"]        = {{"type", "string"}, {"enum", view_direction_names()}, {"description", "A named view to project through, or current: the 3D view's camera now. Instead of right and up"}};
    frame_properties["center"]      = vector3_property("The frame's centre in plate mm (default: the object's centre)");
    frame_properties["width_mm"]    = {{"type", "number"}, {"exclusiveMinimum", 0}, {"description", "Width in mm, greater than 0"}};
    frame_properties["height_mm"]   = {{"type", "number"}, {"exclusiveMinimum", 0}, {"description", "Height in mm, greater than 0"}};

    nlohmann::json properties = {
        {"object_id", {{"type", "integer"}, {"minimum", 0}, {"description", "Object index (0-based). The layers belong to the object"}}},
        {"action", {{"type", "string"}, {"enum", {"add", "edit", "remove", "move"}}, {"description", "add: a new layer in the first free slot; edit: change the layer in slot layer; remove: drop it and its paint; move: put it at to_position"}}},
        {"layer", {{"type", "integer"}, {"minimum", 0}, {"maximum", int(Slic3r::TEXTURE_DISPLACEMENT_MAX_LAYERS) - 1}, {"description", "The layer's slot (edit, remove, move). add takes the first free slot"}}},
        {"to_position", {{"type", "integer"}, {"minimum", 0}, {"description", "move: the 0-based position in slot order"}}},
        {"texture", {{"type", "string"}, {"description", "A texture id from get_texture_library (a name alone works when only one texture has it)"}}},
        {"file_path", {{"type", "string"}, {"description", "An image file (png, jpg, bmp) to use as the texture: imported into the texture library, or the identical texture already there reused. Instead of texture"}}},
        {"depth_mm", bounded_property("depth_mm", "number", "Maximum displacement along the surface normal, in mm")},
        {"tile_size_mm", bounded_property("tile_size_mm", "number", "Size of one texture tile, in mm")},
        {"rotation_deg", bounded_property("rotation_deg", "number", "Texture rotation in degrees")},
        {"midlevel", bounded_property("midlevel", "number", "The height that does not move the surface: 0 only raises, 0.5 lets dark texels cut in")},
        {"smoothing", bounded_property("smoothing", "number", "Blur of the height map before it is sampled (0 raw, 1 strongest)")},
        {"edge_fade_amount", bounded_property("edge_fade_amount", "number", "How far edge_fade reaches into the painted patch")},
        {"offset", {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 2}, {"maxItems", 2}, {"description", "Placement [u, v] in texture repeats"}}},
        {"invert", {{"type", "boolean"}, {"description", "Swap raised and cut texels"}}},
        {"edge_fade", {{"type", "boolean"}, {"description", "Fade the displacement toward the edge of the painted patch"}}},
        {"tile", {{"type", "boolean"}, {"description", "Repeat the texture (false: placed once)"}}},
        {"color", {{"type", "boolean"}, {"description", "Colour the painted area from the texture's own colours (a colour texture only)"}}},
        {"color_mix", {{"type", "boolean"}, {"description", "Stack-wide: interleave filament pairs to reach colours between them"}}},
        {"tile_method", {{"type", "string"}, {"enum", tile_method_names()}, {"description", "How the texture repeats"}}},
        {"projection", {{"type", "string"}, {"enum", projection_names()}, {"description", "How the texture is mapped: triplanar, cylindrical, spherical, unwrap, view (from a view), model_uv (the model's own UV channel)"}}},
        {"blend", {{"type", "string"}, {"enum", blend_names()}, {"description", "How the layer combines with the layers below it (the base layer ignores it)"}}},
        {"view_direction", {{"type", "string"}, {"enum", view_direction_names()}, {"description", "projection view: project from a named view of the 3D view's cameras, or current: its camera now"}}},
        {"view_axes", {{"type", "object"}, {"properties", axes_properties()}, {"additionalProperties", false}, {"description", "projection view: the projector's right and up axes in plate axes, as a layer reports them"}}},
        {"projection_frame", {{"type", {"object", "boolean"}}, {"properties", frame_properties}, {"additionalProperties", false}, {"description", "Project the texture once through a rectangle (view or right and up, center, width_mm, height_mm) with tiling off; false clears the frame"}}},
        {"adjust_image", {{"type", "object"},
                          {"properties", {{"brightness", bounded_property("brightness", "number", "-1 black to 1 white")},
                                          {"contrast", bounded_property("contrast", "number", "-1 flat to 1 hard threshold")},
                                          {"sharpness", bounded_property("sharpness", "number", "0 unchanged to 1")},
                                          {"blur", bounded_property("blur", "number", "0 unchanged to 1")},
                                          {"hue_deg", bounded_property("hue_deg", "number", "Hue shift in degrees (colour only)")}}},
                          {"additionalProperties", false},
                          {"description", "Save an adjusted copy of the layer's image into the library and use it"}}},
        {"color_mix_count", bounded_property("color_mix_count", "integer", "Stack-wide: the most mixed colors the palette may offer")},
        {"color_denoise", bounded_property("color_denoise", "integer", "Stack-wide: passes that clear colour specks")}};
    return {{"type", "object"}, {"properties", properties}, {"required", {"object_id", "action"}}};
}

} // namespace

void OrcaMCPServer::register_texture_tools()
{
    register_tool({
        "get_texture_library",
        ToolCategory::TextureDisplacement,
        "List the textures a layer can use",
        "The textures a texture displacement layer can use, as its picker lists them: built-in, imported into this data "
        "folder's texture library (set_texture_displacement's file_path imports one there; an identical image already "
        "there is reused), and those of installed texture packs. Each has a texture id a layer takes (a name alone works "
        "when only one texture has it), and color: true when it has colours a layer can print with filaments, false for a "
        "grayscale height map, null when the file is not a PNG that is read directly. Reading it changes nothing.",
        {
            {"type", "object"},
            {"properties", {
                {"source", {
                    {"type", "string"},
                    {"enum", {"builtin", "user", "pack"}},
                    {"description", "List only the textures of one source (default: all). counts always covers the whole library"}
                }}
            }}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            return get_texture_library(params);
        }
    });

    register_tool({
        "delete_user_texture",
        ToolCategory::TextureDisplacement,
        "Delete a texture you imported",
        "Delete a texture from this data folder's texture library: only one that was imported (source user), never a "
        "built-in or pack texture. Layers using it keep their image, which they hold themselves and a saved project "
        "stores; the answer lists them (still_used_by). A file delete: not an undo step.",
        {
            {"type", "object"},
            {"properties", {
                {"texture", {
                    {"type", "string"},
                    {"description", "The texture's id from get_texture_library, e.g. user/My bricks.png"}
                }}
            }},
            {"required", {"texture"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            return delete_user_texture(params);
        }
    });

    register_tool({
        "set_texture_displacement",
        ToolCategory::TextureDisplacement,
        "Add, edit, move or remove texture layers",
        "Add, edit, move or remove one texture displacement layer of an object, as the texture displacement panel does. "
        "An object holds up to 8 layers, combined in slot order; each applies where paint_object (mode texture) paints "
        "it, and nothing reaches the print until the layers are baked into the mesh. add takes the first free slot with "
        "texture (an id from get_texture_library) or the library's first; file_path imports an image (an identical one "
        "already in the library is reused, imported_texture says which); edit changes only what is given; move puts a "
        "layer at to_position in slot order, its paint with it (it may take another slot: layer in the answer); remove "
        "drops it and its paint. projection view projects from view_direction (a named view, or current: the 3D view's "
        "camera now) or view_axes; projection_frame projects once through a rectangle (view or right and up, center, "
        "width_mm, height_mm) with tiling off, as the panel's Apply does it also paints the layer where the frame sees the "
        "surface (facets_painted); false clears it; adjust_image saves an adjusted copy of the texture into "
        "the library and uses it. One undo step; nothing to change is changed: false. Refused while an arrange, orient "
        "or bake runs; an open toolbar tool is closed first when the call changes something.",
        set_texture_displacement_schema(),
        [](const nlohmann::json& params) -> nlohmann::json {
            return set_texture_displacement(params);
        }
    });

    register_tool({
        "get_texture_displacement",
        ToolCategory::TextureDisplacement,
        "Read an object's texture layers",
        "Read an object's texture displacement layers, as set_texture_displacement takes them: in slot order (layer is the "
        "slot, position the place in that order; the lowest is the base layer, which ignores blend), each with every setting "
        "set_texture_displacement sets, its texture (image.texture is the id get_texture_library lists, null for an image only "
        "the project holds), inactive_settings (stored but greyed out in the panel, with why), and its paint on each model "
        "part: painted_facets, coverage_percent of the part's surface, and stale for a mask that no longer fits its mesh. "
        "volume_id limits paint to one model part. Also stack_options (the bake's settings), needs_bake (some layer is painted, "
        "so a bake would change the mesh), free_layers (an object holds 8), stack_volume_id (the part that owns the layers: "
        "the first model part), ignored_stacks (layers on other parts, which nothing applies) and parts (the model parts "
        "paint addresses). Reading it changes nothing.",
        {
            {"type", "object"},
            {"properties", {
                {"object_id", {{"type", "integer"}, {"minimum", 0}, {"description", "Object index (0-based)"}}},
                {"volume_id", {{"type", "integer"}, {"minimum", 0}, {"description", "Show the paint of this model part only (the volume index get_object_info lists)"}}}
            }},
            {"required", {"object_id"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            return get_texture_displacement(params);
        }
    });

    register_tool({
        "get_texture_unwrap",
        ToolCategory::TextureDisplacement,
        "Read a layer's unwrap: islands, seams",
        "Read the unwrap of a texture displacement layer whose projection is unwrap, as the texture panel's UV pane shows it: "
        "the painted patch of the object's first model part (the part that owns the layers) cut into charts (islands) along "
        "the seams and the seam angle (once the layer has any seam, only the seams cut). Each island has its area_mm2, centroid and bounding_box (the unwrap's mm), its "
        "placement (offset, rotation_deg, scale), its join group and the neighbours it shares an edge with; also the seams "
        "(pairs of mesh vertices), the seam angle, auto_connect_islands and the vertex edits. include_vertices lists every "
        "unwrapped vertex (vertex, source_vertex, chart, uv), up to 20000, and at most 5000 islands are listed "
        "(islands_truncated). Chart and vertex numbers hold only until the paint, the mesh or the seams change; unwrap_id "
        "names this unwrap, and set_texture_unwrap refuses numbers given with another. set_texture_unwrap's entries are "
        "accepted back as they are. Refused for a layer that is not an unwrap or is "
        "painted nowhere on that part. Reading it changes nothing.",
        {
            {"type", "object"},
            {"properties", {
                {"object_id", {{"type", "integer"}, {"minimum", 0}, {"description", "Object index (0-based)"}}},
                {"layer", {{"type", "integer"}, {"minimum", 0}, {"maximum", int(Slic3r::TEXTURE_DISPLACEMENT_MAX_LAYERS) - 1}, {"description", "The layer's slot"}}},
                {"include_vertices", {{"type", "boolean"}, {"description", "Also list every unwrapped vertex (capped at 20000, truncated says so)"}}}
            }},
            {"required", {"object_id", "layer"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            return get_texture_unwrap(params);
        }
    });

    register_tool({
        "set_texture_unwrap",
        ToolCategory::TextureDisplacement,
        "Edit a layer's unwrap: seams, islands",
        "Edit the unwrap of a texture displacement layer whose projection is unwrap, as the texture panel's UV pane does, in its "
        "order: seam_angle_deg and auto_connect_islands, seams (add_seams, remove_seams, clear_seams, seam_paths snapped to "
        "the nearest vertices, cut_islands), then the unwrap is made anew when seams or the angle changed (its charts are "
        "numbered anew, and the islands are laid out as a net when the layer asks for it), relayout, islands (placement by "
        "chart), join, unjoin, average_scale, clear_uv_edits, uv_edits. Chart and vertex numbers name the unwrap as get_texture_unwrap "
        "reports it before this call (pass its unwrap_id to have a stale number refused), so a call that changes seams or the angle takes no island or vertex edit: send those "
        "in a second call. Refused: an unknown chart or vertex, a join of charts that share no edge, a seam that is no mesh "
        "edge, a layer that is not an unwrap or is painted nowhere on the owner part. One undo step named as the panel names "
        "the edit, none for no change (changed false); an open toolbar tool is closed first when the call changes something. "
        "Answers the unwrap as get_texture_unwrap does.",
        set_texture_unwrap_schema(),
        [](const nlohmann::json& params) -> nlohmann::json {
            return set_texture_unwrap(params);
        }
    });
}
