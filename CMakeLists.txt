cmake_minimum_required(VERSION 3.16)
project(CardiacVisualizer VERSION 0.1.0 LANGUAGES C CXX)

find_package(Threads REQUIRED)

# Vendored SQLite3 amalgamation (C). Its own target with warnings relaxed: the
# amalgamation is third-party generated and trips -Wconversion heavily. Our code
# that touches it lives in cardiac_core and keeps the strict warning set.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite3/sqlite3.c")
    add_library(sqlite3 STATIC third_party/sqlite3/sqlite3.c)
    target_include_directories(sqlite3 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite3")
    target_compile_options(sqlite3 PRIVATE $<$<NOT:$<C_COMPILER_ID:MSVC>>:-w>)
    target_link_libraries(sqlite3 PUBLIC Threads::Threads)
    set(CARDIAC_HAVE_SQLITE3 TRUE)
else()
    set(CARDIAC_HAVE_SQLITE3 FALSE)
    message(STATUS "SQLite3 amalgamation missing; database features disabled")
endif()

add_library(cardiac_core
    src/types.cpp
    src/data_normalizer.cpp
    src/quality_controller.cpp
    src/trajectory_engine.cpp
    src/async_analyzer.cpp
    src/session_controller.cpp
    src/session_exporter.cpp
    src/rolling_metrics.cpp
    src/ble_hrs_parser.cpp
    src/ble_hrs_adapter.cpp
    src/hrv_spectrum.cpp
    src/ibi_csv_importer.cpp
)
if(CARDIAC_HAVE_SQLITE3)
    target_sources(cardiac_core PRIVATE src/database_manager.cpp)
    target_compile_definitions(cardiac_core PUBLIC CARDIAC_HAVE_SQLITE3=1)
    target_link_libraries(cardiac_core PUBLIC sqlite3)
endif()
target_include_directories(cardiac_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(cardiac_core PUBLIC cxx_std_17)
set_target_properties(cardiac_core PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(cardiac_core PUBLIC Threads::Threads)
if(MSVC)
    target_compile_options(cardiac_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(cardiac_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
endif()

# This is a core-integration harness, not the upcoming ImGui/OpenGL desktop UI.
add_executable(cardiac_core_demo main.cpp test_support/synthetic_test_adapter.cpp)
target_include_directories(cardiac_core_demo PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/test_support")
target_link_libraries(cardiac_core_demo PRIVATE cardiac_core)
target_compile_definitions(cardiac_core_demo PRIVATE CARDIAC_CORE_DEMO_ONLY)

# Headless bulk importer: replays every CHF2DB *_IBI.csv in a data folder into
# the runtime SQLite history DB via the async DatabaseManager. Requires SQLite3.
if(CARDIAC_HAVE_SQLITE3)
    add_executable(cardiac_import_chf2db apps/import_chf2db.cpp)
    target_link_libraries(cardiac_import_chf2db PRIVATE cardiac_core)
    if(NOT MSVC)
        target_compile_options(cardiac_import_chf2db PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
    endif()

    # Headless cohort expander: derives N deterministic SYNTHETIC_TEST subjects
    # from the real CHF2DB IBI bases through the same pipeline, so the
    # multi-subject grid's 500+ index/paging path can actually be exercised and
    # measured. Clearly labelled as synthetic in every stored row.
    add_executable(cardiac_build_cohort apps/build_cohort.cpp)
    target_link_libraries(cardiac_build_cohort PRIVATE cardiac_core)
    if(NOT MSVC)
        target_compile_options(cardiac_build_cohort PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
    endif()
endif()

# Optional desktop dashboard (Dear ImGui + GLFW + OpenGL3, vendored).
option(CARDIAC_BUILD_DASHBOARD "Build ImGui/GLFW desktop dashboard" ON)
if(CARDIAC_BUILD_DASHBOARD)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/imgui.cpp" AND
       EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/glfw/CMakeLists.txt")
        set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
        add_subdirectory(third_party/glfw)
        add_library(imgui STATIC
            third_party/imgui/imgui.cpp
            third_party/imgui/imgui_draw.cpp
            third_party/imgui/imgui_tables.cpp
            third_party/imgui/imgui_widgets.cpp
            third_party/imgui/backends/imgui_impl_glfw.cpp
            third_party/imgui/backends/imgui_impl_opengl3.cpp)
        target_include_directories(imgui PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui"
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/backends")
        target_link_libraries(imgui PUBLIC glfw)
        if(NOT MSVC)
            target_compile_options(imgui PRIVATE -w)
        endif()
        add_library(cardiac_dashboard
            src/ui/dashboard_draw.cpp
            src/ui/dashboard_frame.cpp
            src/ui/dashboard_session.cpp
            src/ui/dashboard_app.cpp
            src/ui/grid_ui.cpp
            src/ui/grid_viewport.cpp
            src/ui/render/cgl.cpp
            src/ui/render/gl_shader.cpp
            src/ui/render/gl_framebuffer.cpp
            src/ui/render/trajectory_renderer.cpp)
        target_include_directories(cardiac_dashboard PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/ui"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/render"
            "${CMAKE_CURRENT_SOURCE_DIR}/test_support")
        target_link_libraries(cardiac_dashboard PUBLIC cardiac_core imgui glfw opengl32)
        if(WIN32)
            target_link_libraries(cardiac_dashboard PUBLIC gdi32)
        endif()
        if(NOT MSVC)
            target_compile_options(cardiac_dashboard PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
        endif()
        add_executable(cardiac_dashboard_app apps/dashboard_main.cpp test_support/synthetic_test_adapter.cpp)
        target_link_libraries(cardiac_dashboard_app PRIVATE cardiac_dashboard cardiac_core)
    else()
        message(STATUS "Dashboard vendored deps missing; CARDIAC_BUILD_DASHBOARD disabled")
    endif()
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(cardiac_core_tests tests/core_tests.cpp test_support/synthetic_test_adapter.cpp)
    target_include_directories(cardiac_core_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/test_support")
    target_link_libraries(cardiac_core_tests PRIVATE cardiac_core)
    add_test(NAME cardiac_core_tests COMMAND cardiac_core_tests)

    # Phase-2 headless extensions: HRV spectrum, CHF2DB CSV importer, async DB.
    add_executable(cardiac_extension_tests tests/extension_tests.cpp)
    target_include_directories(cardiac_extension_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(cardiac_extension_tests PRIVATE cardiac_core)
    if(NOT MSVC)
        target_compile_options(cardiac_extension_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
    endif()
    add_test(NAME cardiac_extension_tests COMMAND cardiac_extension_tests)

    # GPU smoke test for the advanced renderer; skips cleanly with no 3.3 context.
    if(TARGET cardiac_dashboard)
        add_executable(cardiac_gl_smoke tests/gl_smoke_test.cpp)
        target_link_libraries(cardiac_gl_smoke PRIVATE cardiac_dashboard cardiac_core)
        if(NOT MSVC)
            target_compile_options(cardiac_gl_smoke PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
        endif()
        add_test(NAME cardiac_gl_smoke COMMAND cardiac_gl_smoke)

        # Multi-subject grid: cohort indexing, filtering/ranges, paging, the async
        # batch-request contract, time-window slicing + decimation, playback
        # looping, and the reserved live-smartwatch slot. Pure CPU - no GL and no
        # ImGui context - so it validates the 500+ subject scalability invariant on
        # headless CI, where cardiac_gl_smoke covers the GPU path.
        add_executable(cardiac_grid_tests tests/grid_tests.cpp)
        target_link_libraries(cardiac_grid_tests PRIVATE cardiac_dashboard cardiac_core)
        if(NOT MSVC)
            target_compile_options(cardiac_grid_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
        endif()
        add_test(NAME cardiac_grid_tests COMMAND cardiac_grid_tests)
    endif()
endif()