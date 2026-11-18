#ifndef slic3r_BundledPython_hpp_
#define slic3r_BundledPython_hpp_

#include <boost/filesystem/path.hpp>

// Upstream keeps find_bundled_python_home and find_python_executable file-private in
// PythonInterpreter.cpp. OrcaMCP's Connect names the app's own Python with them (carried item CI in
// CLAUDE.md), so they are declared here -- in a header of their own, because PythonInterpreter.hpp
// pulls in Python's C headers and no caller that only wants the path should carry them.
// The definitions, in PythonInterpreter.cpp, are byte-for-byte upstream's.

namespace Slic3r {

// The bundled CPython home inside this build's install tree (deps/python3), or an empty path when
// the install ships no runtime.
boost::filesystem::path find_bundled_python_home();

// The interpreter executable inside `python_home`, or an empty path when it holds none.
boost::filesystem::path find_python_executable(const boost::filesystem::path& python_home);

} // namespace Slic3r

#endif // slic3r_BundledPython_hpp_
