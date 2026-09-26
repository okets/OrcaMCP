get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)
add_executable(${_TEST_NAME}_tests
    ${_TEST_NAME}_tests_main.cpp
    test_bambu_filament_ids.cpp
    test_creality_cfs_match.cpp
    test_agent_storage_state.cpp
    test_dev_mapping.cpp
    test_filament_bitmap_utils.cpp
    test_network_versions.cpp
    test_action_source.cpp
    test_plugin_host_api.cpp
    test_plugin_capability_config.cpp
    test_plugin_config.cpp
    test_plugin_capabilities_in_use.cpp
    test_plugin_install.cpp
    test_plugin_lifecycle.cpp
    test_slicing_pipeline_bindings.cpp
    test_slicing_pipeline_config.cpp
    test_plugin_sort.cpp
    test_plugin_cloud_metadata.cpp
    test_plugin_audit.cpp
    test_flashforge_api.cpp
    test_flashforge_console.cpp
    test_flashforge_live.cpp
    test_obico_link.cpp
    test_print_host_keys.cpp
    test_send_dialog_routing.cpp
    test_project_match.cpp
    test_slice_estimate.cpp
    test_preset_query.cpp
    test_config_value_shape.cpp
    test_hex_color.cpp
    test_color_recipe.cpp
    test_material_mapping.cpp
    test_paint_geometry.cpp
    test_scalar_params.cpp
    test_paint_select.cpp
    test_object_placement.cpp
    test_plate_occupancy.cpp
    test_transform_frames.cpp
    test_render_math.cpp
    test_first_layer_plan.cpp
    test_filament_model.cpp
    test_mcp_tool_list.cpp
    test_mcp_model_load.cpp
    test_mcp_suppression.cpp
    ../fff_print/test_helpers.cpp
    )

if (MSVC)
    target_link_libraries(${_TEST_NAME}_tests Setupapi.lib)
endif ()

target_link_libraries(${_TEST_NAME}_tests test_common libslic3r_gui libslic3r pybind11::embed Catch2::Catch2WithMain)

# The golden tools file test_mcp_tool_list.cpp checks the MCP tool registry against, and rewrites
# when ORCAMCP_UPDATE_TOOLS_GOLDEN=1. CI's test jobs check out scripts/ at this same path.
set(ORCAMCP_TOOLS_GOLDEN_FILE "${CMAKE_SOURCE_DIR}/scripts/orcamcp_tools.json")
file(TO_NATIVE_PATH "${ORCAMCP_TOOLS_GOLDEN_FILE}" ORCAMCP_TOOLS_GOLDEN_FILE)
target_compile_definitions(${_TEST_NAME}_tests PRIVATE ORCAMCP_TOOLS_GOLDEN_FILE=R"\(${ORCAMCP_TOOLS_GOLDEN_FILE}\)")
set_property(TARGET ${_TEST_NAME}_tests PROPERTY FOLDER "tests")

orcaslicer_copy_test_dlls()

if (WIN32)
    add_custom_command(TARGET ${_TEST_NAME}_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>/python"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_PREFIX_PATH}/libpython" "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>/python"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_PREFIX_PATH}/libpython/python${_bundled_python_abi}.dll"
            "${CMAKE_PREFIX_PATH}/libpython/vcruntime140.dll"
            "${CMAKE_PREFIX_PATH}/libpython/vcruntime140_1.dll"
            "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>"
        COMMENT "Copying Python runtime for slic3rutils plugin host API tests"
        VERBATIM
    )
elseif (APPLE)
    target_link_options(${_TEST_NAME}_tests PRIVATE
        "LINKER:-rpath,@executable_path/python/lib")

    add_custom_command(TARGET ${_TEST_NAME}_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -rf
            "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>/python"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_PREFIX_PATH}/libpython"
            "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>/python"
        COMMENT "Copying Python runtime for macOS plugin host API tests"
        VERBATIM
    )
elseif (FLATPAK)
    # Same <testdir>/python home as WIN32/APPLE; symlink since /app/libpython
    # already ships in the flatpak (the test exe links libpython3.12.so from it).
    add_custom_command(TARGET ${_TEST_NAME}_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -rf
            "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>/python"
        COMMAND ${CMAKE_COMMAND} -E create_symlink
            "${CMAKE_PREFIX_PATH}/libpython"
            "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>/python"
        COMMENT "Linking Python runtime for flatpak plugin host API tests"
        VERBATIM
    )
endif()

orcaslicer_discover_tests(${_TEST_NAME}_tests)
