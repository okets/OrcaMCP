// src/slic3r/GUI/OrcaMCP/OrcaMCPExportTools.cpp
#include "OrcaMCPExportTools.hpp"

#include "OrcaMCPCommon.hpp"
#include "OrcaMCPExports.hpp"
#include "OrcaMCPGcodeCheck.hpp"

#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"

#include <boost/filesystem.hpp>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {

// Every plate as Export plate sliced file reads it.
std::vector<SlicedFilePlate> sliced_file_plates_of(PartPlateList& plate_list)
{
    std::vector<SlicedFilePlate> plates;
    for (int i = 0; i < plate_list.get_plate_count(); ++i) {
        PartPlate* plate = plate_list.get_plate(i);
        SlicedFilePlate state;
        state.index           = i;
        state.has_objects     = !plate->empty();
        state.all_unprintable = plate->is_all_instances_unprintable();
        state.sliced          = plate_gcode_checked(*plate);
        state.check_refusal   = plate_gcode_check_refusal(*plate, i);
        plates.push_back(std::move(state));
    }
    return plates;
}

} // namespace

nlohmann::json export_sliced_file(Plater& plater, McpDialogSuppressionGuard& guard, const std::string& output_path, bool all_plates)
{
    if (plater.model().objects.empty())
        return error_response("The scene has no objects, so there is no G-code to export.");
    if (plater.is_export_gcode_scheduled())
        return error_response("Another export job is running.");
    PartPlateList&                     plate_list = plater.get_partplate_list();
    const std::vector<SlicedFilePlate> plates     = sliced_file_plates_of(plate_list);
    const int                          selected   = plate_list.get_curr_plate_index();
    if (const auto refusal = sliced_file_refusal(plates, all_plates, selected))
        return error_response(*refusal);

    guard.answer_file(output_path);
    const bool written = plater.export_gcode_3mf(all_plates);
    nlohmann::json answer;
    if (written) {
        answer = {{"status", "success"},
                  {"output_path", output_path},
                  {"format", "gcode.3mf"},
                  {"plates", sliced_file_plates(plates, all_plates, selected)}};
        boost::system::error_code error;
        const auto                bytes = boost::filesystem::file_size(output_path, error);
        if (!error)
            answer["bytes"] = bytes;
    } else {
        answer = error_response("The app did not write the sliced file; error_messages or active_warnings say why when it did");
    }
    answer["active_warnings"] = get_active_warnings_json(&plater);
    return guard.fail_on_errors(guard.report(answer));
}

}}} // namespace Slic3r::GUI::OrcaMCP
