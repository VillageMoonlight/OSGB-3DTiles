cmake_minimum_required(VERSION 3.20)
project(osgb2tiles VERSION 1.0.0 LANGUAGES CXX)

# C++17 标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# ============================================================
# 查找依赖库（vcpkg 中 OSG 的包名是 osg，CMake 包名 OpenSceneGraph）
# ============================================================

# OpenSceneGraph（vcpkg port: osg）
find_package(OpenSceneGraph REQUIRED COMPONENTS
    osg
    osgDB
    osgUtil
    osgViewer
    osgGA
    osgText
    OpenThreads
)

# nlohmann/json
find_package(nlohmann_json CONFIG REQUIRED)

# meshoptimizer
find_package(meshoptimizer CONFIG REQUIRED)

# draco（可选）
find_package(draco CONFIG QUIET)
if(draco_FOUND)
    message(STATUS "Draco found - geometry compression enabled")
    add_definitions(-DHAVE_DRACO)
else()
    message(STATUS "Draco not found - geometry compression disabled")
endif()

# ktx（KTX2 Basis Universal 纹理压缩）
find_package(Ktx CONFIG QUIET)
if(Ktx_FOUND)
    message(STATUS "libktx found - KTX2/BasisU texture compression enabled")
    add_definitions(-DHAVE_KTX)
else()
    message(STATUS "libktx not found - KTX2 texture compression disabled (install via: vcpkg install ktx)")
endif()

# stb（header-only，vcpkg 提供 find_package(Stb REQUIRED)）
find_package(Stb REQUIRED)
set(STB_INCLUDE_DIR ${Stb_INCLUDE_DIR})

# cxxopts（header-only）
find_package(cxxopts CONFIG REQUIRED)

# libwebp（WebP 纹理编码）
find_package(WebP CONFIG REQUIRED)
message(STATUS "WebP found: ${WebP_VERSION}")

# tinygltf（header-only，vcpkg 提供 find_path 方式）
find_path(TINYGLTF_INCLUDE_DIR tiny_gltf.h
    REQUIRED
)
message(STATUS "tinygltf include dir: ${TINYGLTF_INCLUDE_DIR}")

# GDAL（坐标转换）
find_package(GDAL CONFIG REQUIRED)
message(STATUS "GDAL found: ${GDAL_VERSION}")

# ============================================================
# 源文件列表
# ============================================================
set(SOURCES
    src/main.cpp
    src/OsgbReader.cpp
    src/OsgbMetaReader.cpp
    src/GeometryConverter.cpp
    src/MeshSimplifier.cpp
    src/TextureProcessor.cpp
    src/GlbWriter.cpp
    src/TilesetBuilder.cpp
    src/TopLevelMerger.cpp
    src/ThreadPool.cpp
    src/Logger.cpp
)

set(HEADERS
    src/Config.h
    src/OsgbReader.h
    src/OsgbMetaReader.h
    src/GeometryConverter.h
    src/MeshSimplifier.h
    src/TextureProcessor.h
    src/GlbWriter.h
    src/TilesetBuilder.h
    src/TopLevelMerger.h
    src/ThreadPool.h
    src/Logger.h
    src/MathUtils.h
)

# ============================================================
# 可执行目标
# ============================================================
add_executable(osgb2tiles ${SOURCES} ${HEADERS})

target_include_directories(osgb2tiles PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${OPENSCENEGRAPH_INCLUDE_DIRS}
    ${STB_INCLUDE_DIR}
    ${TINYGLTF_INCLUDE_DIR}
)

target_link_libraries(osgb2tiles PRIVATE
    ${OPENSCENEGRAPH_LIBRARIES}
    nlohmann_json::nlohmann_json
    meshoptimizer::meshoptimizer
    cxxopts::cxxopts
    GDAL::GDAL
    WebP::webp
)

if(draco_FOUND)
    target_link_libraries(osgb2tiles PRIVATE draco::draco)
endif()

if(Ktx_FOUND)
    target_link_libraries(osgb2tiles PRIVATE KTX::ktx)
endif()

# ============================================================
# 平台特定设置
# ============================================================
if(MSVC)
    # Windows: Unicode, 多处理器编译
    target_compile_options(osgb2tiles PRIVATE /W3 /MP /utf-8)
    target_compile_definitions(osgb2tiles PRIVATE _UNICODE UNICODE _CRT_SECURE_NO_WARNINGS)
    # 禁用 min/max 宏（与 std::min/max 冲突）
    target_compile_definitions(osgb2tiles PRIVATE NOMINMAX)
elseif(APPLE)
    # macOS: 支持 Apple Silicon (arm64) + x86_64
    target_compile_options(osgb2tiles PRIVATE -Wall -Wextra)
else()
    # Linux
    target_compile_options(osgb2tiles PRIVATE -Wall -Wextra)
    target_link_libraries(osgb2tiles PRIVATE pthread)
endif()

# ============================================================
# 安装规则
# ============================================================
install(TARGETS osgb2tiles RUNTIME DESTINATION bin)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/config.json DESTINATION bin)

# ============================================================
# osgb_viewer — DasViewer 风格 OSGB 三维浏览器（仅 Windows）
# ============================================================
if(WIN32)
add_executable(osgb_viewer src/osgb_viewer.cpp)

target_include_directories(osgb_viewer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${OPENSCENEGRAPH_INCLUDE_DIRS}
)

target_link_libraries(osgb_viewer PRIVATE
    ${OPENSCENEGRAPH_LIBRARIES}
    dwmapi
    ole32
    shell32
    comctl32
    comdlg32
    gdi32
    user32
)

if(MSVC)
    # /FI 强制在任何源文件最前面包含 windows.h，
    # 确保 commctrl.h 所需的 Win32 基础类型（CALLBACK 等）已经定义
    target_compile_options(osgb_viewer PRIVATE
        /W3 /MP /utf-8
        "/FIwindows.h"
    )
    target_compile_definitions(osgb_viewer PRIVATE _UNICODE UNICODE NOMINMAX _CRT_SECURE_NO_WARNINGS)
    set_target_properties(osgb_viewer PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG   "/SUBSYSTEM:CONSOLE"
    )
endif()

install(TARGETS osgb_viewer RUNTIME DESTINATION bin)
endif() # WIN32


# ============================================================
# 构建信息
# ============================================================
message(STATUS "=== osgb2tiles Build Configuration ===")
message(STATUS "OSG version  : ${OPENSCENEGRAPH_VERSION}")
message(STATUS "Build type   : ${CMAKE_BUILD_TYPE}")
message(STATUS "Install dir  : ${CMAKE_INSTALL_PREFIX}")
message(STATUS "C++ standard : ${CMAKE_CXX_STANDARD}")
