get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)
add_executable(${_TEST_NAME}_tests
    ${_TEST_NAME}_tests_main.cpp
    test_bambu_filament_ids.cpp
    test_creality_cfs_match.cpp
    test_agent_storage_state.cpp
    test_dev_mapping.cpp
    test_filament_bitmap_utils.cpp
    test_scene_raycaster.cpp
    test_lazy.cpp
    test_prebuild_queue.cpp
    test_staged_build.cpp
    test_network_versions.cpp
    test_action_source.cpp
    test_plugin_host_api.cpp
    # Exercise seam enums and predicates through the embedded Python host API.
    test_precise_seam_plugin.cpp
    test_plugin_capability_config.cpp
    test_plugin_config.cpp
    test_plugin_capabilities_in_use.cpp
    test_plugin_install.cpp
    test_plugin_lifecycle.cpp
    test_plugin_printer_agent.cpp
    test_printhost.cpp
    test_slicing_pipeline_bindings.cpp
    test_slicing_pipeline_config.cpp
    test_plugin_sort.cpp
    test_plugin_cloud_metadata.cpp
    test_plugin_audit.cpp
    test_flashforge_api.cpp
    test_flashforge_local_api.cpp
    test_flashforge_send_options.cpp
    test_flashforge_console.cpp
    test_flashforge_live.cpp
    test_obico_link.cpp
    test_mcp_printer_answers.cpp
    test_mcp_printer_control.cpp
    test_print_host_keys.cpp
    test_send_dialog_routing.cpp
    test_project_match.cpp
    test_slice_estimate.cpp
    test_preset_query.cpp
    test_preset_install.cpp
    test_printer_switch.cpp
    test_config_value_shape.cpp
    test_hex_color.cpp
    test_color_recipe.cpp
    test_material_mapping.cpp
    test_mcp_print_options.cpp
    test_paint_geometry.cpp
    test_scalar_params.cpp
    test_paint_select.cpp
    test_paint_remap.cpp
    test_extrusion_features.cpp
    test_object_placement.cpp
    test_plate_instances.cpp
    test_plate_occupancy.cpp
    test_transform_frames.cpp
    test_render_math.cpp
    test_mcp_image_files.cpp
    test_first_layer_plan.cpp
    test_instance_box.cpp
    test_layer_ranges.cpp
    test_object_settings.cpp
    test_layer_plan.cpp
    test_filament_model.cpp
    test_filament_slots.cpp
    test_mcp_tool_list.cpp
    test_mcp_tool_arguments.cpp
    test_mcp_argument_values.cpp
    test_mcp_ui_job.cpp
    test_mcp_model_load.cpp
    test_mcp_next_steps.cpp
    test_mcp_scene_description.cpp
    test_mcp_suppression.cpp
    test_mcp_shutdown.cpp
    test_mcp_quit.cpp
    test_queued_call.cpp
    test_slice_credit.cpp
    test_slice_progress.cpp
    test_gcode_check.cpp
    test_mcp_exports.cpp
    test_layer_gcode.cpp
    test_mcp_user_view.cpp
    test_mcp_pending_events.cpp
    test_mcp_source_files.cpp
    test_config_values.cpp
    test_http_server.cpp
    test_thread_cancel.cpp
    test_mcp_request_guard.cpp
    test_mcp_port_choice.cpp
    test_instance_registry.cpp
    test_mcp_client_config.cpp
    test_mesh_health.cpp
    test_mesh_repair.cpp
    test_part_edits.cpp
    test_arrange_options.cpp
    test_instance_edits.cpp
    test_plate_settings.cpp
    test_shortcuts.cpp
    ../fff_print/test_helpers.cpp
    )

if (MSVC)
    target_link_libraries(${_TEST_NAME}_tests Setupapi.lib)
    # Every test starts as every libslic3r_gui source does, with the Windows headers first: the GUI
    # library gets them from its PCH, or this same force-include with the PCH off
    # (src/slic3r/CMakeLists.txt). Without it, a test that reaches wx after libslic3r headers (as
    # PartPlate.hpp does) fails in wx/msw/private.h and asio.
    target_compile_options(${_TEST_NAME}_tests PRIVATE "/FIslic3r/win_platform.hpp")
endif ()

target_link_libraries(${_TEST_NAME}_tests test_common libslic3r_gui libslic3r pybind11::embed Catch2::Catch2WithMain)

# The golden tools file test_mcp_tool_list.cpp checks the MCP tool registry against, and rewrites
# when ORCAMCP_UPDATE_TOOLS_GOLDEN=1. CI's test jobs check out scripts/ at this same path.
set(ORCAMCP_TOOLS_GOLDEN_FILE "${CMAKE_SOURCE_DIR}/scripts/orcamcp_tools.json")
file(TO_NATIVE_PATH "${ORCAMCP_TOOLS_GOLDEN_FILE}" ORCAMCP_TOOLS_GOLDEN_FILE)
target_compile_definitions(${_TEST_NAME}_tests PRIVATE ORCAMCP_TOOLS_GOLDEN_FILE=R"\(${ORCAMCP_TOOLS_GOLDEN_FILE}\)")
# The tools reference, whose get_scene_info example test_mcp_scene_description.cpp holds to the keys a
# real response has. CI's test jobs check out docs/tools at this same path.
set(ORCAMCP_REFERENCE_DOC "${CMAKE_SOURCE_DIR}/docs/tools/reference.md")
file(TO_NATIVE_PATH "${ORCAMCP_REFERENCE_DOC}" ORCAMCP_REFERENCE_DOC)
target_compile_definitions(${_TEST_NAME}_tests PRIVATE ORCAMCP_REFERENCE_DOC=R"\(${ORCAMCP_REFERENCE_DOC}\)")
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
elseif (NOT FLATPAK)
    # The CI unit-test runner only receives the build/tests tree, so both the
    # interpreter and the libpython the test binary links have to travel next to
    # the executable; find_bundled_python_home() picks the copy up from there.
    if (APPLE)
        target_link_options(${_TEST_NAME}_tests PRIVATE
            "LINKER:-rpath,@executable_path/python/lib")
    else ()
        set_property(TARGET ${_TEST_NAME}_tests APPEND PROPERTY BUILD_RPATH "$ORIGIN/python/lib")
    endif ()

    add_custom_command(TARGET ${_TEST_NAME}_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -rf
            "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>/python"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_PREFIX_PATH}/libpython"
            "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>/python"
        COMMENT "Copying Python runtime for the plugin host API tests"
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

# scripts/run_unit_tests.sh installs the tests' numpy into that runtime with this uv,
# staged where the app build tree keeps it (<exe dir>/tools/uv, see src/CMakeLists.txt).
if (ORCA_BUNDLED_UV_EXECUTABLE)
    add_custom_command(TARGET ${_TEST_NAME}_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>/tools/uv"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ORCA_BUNDLED_UV_EXECUTABLE}"
            "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>/tools/uv/${ORCA_BUNDLED_UV_FILENAME}"
        COMMENT "Copying uv for the plugin host API tests"
        VERBATIM
    )
endif()

orcaslicer_discover_tests(${_TEST_NAME}_tests)
