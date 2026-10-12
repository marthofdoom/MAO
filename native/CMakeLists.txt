# MAO native plugin — structure copied from the working sibling MEO project
# (itself from SkyrimDev/HelloWorld-using-CommonLibSSE-NG). Doctrine: adapt
# working sources, don't write build systems from memory. imgui + json are
# linked from M0 so the vcpkg cache is built once; the M2 essence viewer uses
# them, earlier milestones just don't call in.
#
# imgui carries the dx11-binding feature only. The win32 platform backend is NOT
# taken from vcpkg: imgui_impl_win32.cpp is vendored here (upstream v1.92.8,
# byte-exact, must match the vcpkg-pinned imgui) and compiled into this target
# with IMGUI_IMPL_WIN32_DISABLE_GAMEPAD, so the backend's own XInput poll is
# compiled OUT. The input hook in plugin.cpp is the single gamepad source; the
# poll's per-frame HasGamepad rewrite killed gamepad nav (MFO v1.0.59).
# Re-adding vcpkg's win32-binding feature would duplicate every
# ImGui_ImplWin32_* symbol at link: the loud failure is intentional.
cmake_minimum_required(VERSION 3.21)

project(MAO VERSION 0.0.1 LANGUAGES CXX)

find_package(CommonLibSSE CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
add_commonlibsse_plugin(${PROJECT_NAME} SOURCES plugin.cpp imgui_impl_win32.cpp)
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_precompile_headers(${PROJECT_NAME} PRIVATE PCH.h)
# The vendored backend TU compiles standalone, as vcpkg compiled it: no PCH (it
# must not inherit CommonLibSSE's headers/macros). The gamepad define only
# touches guarded regions of imgui_impl_win32.cpp. /w: third-party code under
# our flag set for the first time, so no warning may become an error.
set_source_files_properties(imgui_impl_win32.cpp PROPERTIES
    SKIP_PRECOMPILE_HEADERS ON
    COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:MSVC>:/w>")
target_compile_definitions(${PROJECT_NAME} PRIVATE IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)
target_link_libraries(${PROJECT_NAME} PRIVATE imgui::imgui d3d11 nlohmann_json::nlohmann_json)
