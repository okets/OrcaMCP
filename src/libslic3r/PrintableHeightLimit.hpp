#ifndef slic3r_PrintableHeightLimit_hpp_
#define slic3r_PrintableHeightLimit_hpp_

namespace Slic3r {

// Orca: whether a printable height -- the bed's printable_height or an extruder's
// extruder_printable_height -- is a limit at all. 0 sets none: the build volume takes a
// printable_height of 0 for no height limit (BuildVolume::object_state), and 0 is the
// extruder_printable_height default, which every profile without extruder heights carries, every
// non-Bambu multi-extruder one among them. Nil (NaN) sets none either.
inline bool is_height_limit(double printable_height) { return printable_height > 0.; }

} // namespace Slic3r

#endif // slic3r_PrintableHeightLimit_hpp_
