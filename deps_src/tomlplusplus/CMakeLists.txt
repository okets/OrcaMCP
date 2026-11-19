cmake_minimum_required(VERSION 3.13)
project(tomlplusplus)

# toml++ 3.4.0, header-only (https://github.com/marzer/tomlplusplus, MIT). Only
# src/slic3r/GUI/OrcaMCP/OrcaMCPCodexToml.cpp includes it: Connect reads and checks Codex's config.toml.
add_library(tomlplusplus INTERFACE)

target_include_directories(tomlplusplus SYSTEM
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_sources(tomlplusplus INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/toml.hpp
)

target_compile_features(tomlplusplus INTERFACE cxx_std_17)
