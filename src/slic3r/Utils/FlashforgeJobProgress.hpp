#ifndef slic3r_Utils_FlashforgeJobProgress_hpp_
#define slic3r_Utils_FlashforgeJobProgress_hpp_

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "FlashforgeApi.hpp"

namespace Slic3r {

struct GCodeProcessorResult;

namespace FlashforgeJobProgress {

// A Flashforge's `printProgress` is how far it has read through the G-code file, by bytes, and its
// `printLayer` ran two layers ahead of the layer printing on a Creator 5 Pro (firmware 1.9.9,
// 2026-10-09: 20.6 % of the file is inside layer 36, which the user watched print, while the printer
// said 38). Bytes are not time: a multi-colour board's top layers held 80 % of the file and a fraction
// of the time, so a remaining time projected from bytes read 16 h where 5 h were left.
//
// For a print this app sent, the slice says where each layer starts in the file and how much of the
// slicer's time comes before it. That table turns the printer's byte progress into the layer printing
// and the share of the work done.
struct SliceTable
{
    std::uint64_t              file_bytes{0};
    std::vector<std::uint64_t> layer_start_bytes; // ascending; layer i (1-based i + 1) starts here
    std::vector<double>        layer_start_s;     // the slicer's seconds before layer i starts
    double                     total_s{0};        // the slicer's seconds for the whole file

    bool valid() const;
};

// The table of a sliced plate, from its G-code processor result: each layer's first move, its line's
// byte offset in the file (`lines_ends`) and the move times before it (normal mode). Empty (not valid)
// when the result has no layers or no line offsets.
SliceTable slice_table(const GCodeProcessorResult& result);

// The table travels with an upload in its extended info, under this key, as JSON.
extern const char* const kExtendedInfoKey;
std::string to_json(const SliceTable& table);
// std::nullopt for anything that is not a valid table.
std::optional<SliceTable> from_json(const std::string& text);
// The table an upload carries, if any.
std::optional<SliceTable> from_extended_info(const std::map<std::string, std::string>& extended_info);
// The table an upload carries, if it describes the file sent: a plain G-code upload is a copy that
// post-processing scripts may have changed, so one of another size is not the sliced file. The G-code
// inside a .gcode.3mf is the sliced file itself, byte for byte (_add_gcode_file_to_archive).
std::optional<SliceTable> for_upload(const std::map<std::string, std::string>& extended_info,
                                     bool                                      gcode_in_3mf,
                                     std::uint64_t                             uploaded_bytes);

struct Reading
{
    int    layer{0};          // 1-based layer printing; the start G-code counts as the first layer's
    int    layers{0};         // the slice's layer count
    double time_fraction{0};  // the share of the slicer's time done, 0..1
};

// What a byte progress (0..1) means on the sliced file.
Reading read(const SliceTable& table, double byte_progress);

// Reads `status` by the table of the print it is printing: the layer, layer count and work done
// (the slicer's time done) from the slice,
// remaining_s projected from the slicer's time done (FlashforgeApi::project_remaining_s), and
// `progress_source` "slice". Leaves the status as the printer sent it when the job is
// not active, its numbers are not readings (implausible_telemetry) or the table is not valid.
void apply(FlashforgeApi::PrinterStatus& status, const SliceTable& table);

}} // namespace Slic3r::FlashforgeJobProgress

#endif
