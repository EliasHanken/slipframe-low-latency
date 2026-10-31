cmake_minimum_required(VERSION 3.20)
project(SlipframeLowLatency CXX)

# NVIDIA's public NVAPI (MIT) is fetched at a pinned commit; nothing of it ships in the package, because the DLL links it
# statically and the driver provides the rest. Unity's plugin API comes with the editor install.
include(FetchContent)
FetchContent_Declare(nvapi GIT_REPOSITORY https://github.com/NVIDIA/nvapi.git GIT_TAG 70d337d)
FetchContent_MakeAvailable(nvapi)
set(UNITY_PLUGIN_API "C:/Program Files/Unity/Hub/Editor/6000.3.19f1/Editor/Data/PluginAPI" CACHE PATH "Unity PluginAPI headers")

add_library(SlipframeLowLatency SHARED slipframe_low_latency.cpp)
target_compile_definitions(SlipframeLowLatency PRIVATE UNICODE _UNICODE)
target_include_directories(SlipframeLowLatency PRIVATE ${nvapi_SOURCE_DIR} ${UNITY_PLUGIN_API})
target_compile_features(SlipframeLowLatency PRIVATE cxx_std_17)
target_link_libraries(SlipframeLowLatency PRIVATE ${nvapi_SOURCE_DIR}/amd64/nvapi64.lib)
set_target_properties(SlipframeLowLatency PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded")
