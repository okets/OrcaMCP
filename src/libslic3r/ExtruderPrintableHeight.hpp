#ifndef slic3r_ExtruderPrintableHeight_hpp_
#define slic3r_ExtruderPrintableHeight_hpp_

namespace Slic3r {

// Orca: whether an extruder_printable_height limits its extruder. 0 -- the default, and what every
// profile without extruder heights carries, every non-Bambu multi-extruder one among them -- and nil
// (NaN) set no limit of their own: the bed's printable_height applies.
inline bool limits_extruder_height(double extruder_printable_height) { return extruder_printable_height > 0.; }

} // namespace Slic3r

#endif // slic3r_ExtruderPrintableHeight_hpp_
