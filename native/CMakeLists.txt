# MAO native plugin — structure copied from the working sibling MEO project
# (itself from SkyrimDev/HelloWorld-using-CommonLibSSE-NG). Doctrine: adapt
# working sources, don't write build systems from memory. imgui + json are
# linked from M0 so the vcpkg cache is built once; the M2 essence viewer uses
# them, earlier milestones just don't call in.
cmake_minimum_required(VERSION 3.21)

project(MAO VERSION 0.0.1 LANGUAGES CXX)

find_package(CommonLibSSE CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
add_commonlibsse_plugin(${PROJECT_NAME} SOURCES plugin.cpp)
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_precompile_headers(${PROJECT_NAME} PRIVATE PCH.h)
target_link_libraries(${PROJECT_NAME} PRIVATE imgui::imgui d3d11 nlohmann_json::nlohmann_json)
