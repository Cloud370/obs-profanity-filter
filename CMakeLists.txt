cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)

include(compilerconfig)
include(defaults)
include(helpers)

include(FetchContent)

message(STATUS "Fetching sherpa-onnx...")
FetchContent_Declare(
  sherpa_onnx
  URL https://huggingface.co/csukuangfj/sherpa-onnx-libs/resolve/main/win64/1.12.10/sherpa-onnx-v1.12.10-win-x64-shared.tar.bz2
)
FetchContent_MakeAvailable(sherpa_onnx)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
  
  # Force link to the library we know exists (Fallback for local builds)
  set(OBS_FRONTEND_LIB_PATH "${CMAKE_CURRENT_SOURCE_DIR}/.deps/lib/obs-frontend-api.lib")
  if(EXISTS "${OBS_FRONTEND_LIB_PATH}")
      target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE "${OBS_FRONTEND_LIB_PATH}")
  endif()
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core REQUIRED)
  get_target_property(QT_WIDGETS_INC Qt6::Widgets INTERFACE_INCLUDE_DIRECTORIES)
  message(STATUS "Qt6::Widgets include dirs: ${QT_WIDGETS_INC}")
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

# Sherpa ONNX Setup
set(SHERPA_ONNX_ROOT "${sherpa_onnx_SOURCE_DIR}")
message(STATUS "Sherpa ONNX Root: ${SHERPA_ONNX_ROOT}")

# CPP-Pinyin Setup
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CPP_PINYIN_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(thirdparty/cpp-pinyin)
if(MSVC)
    target_compile_options(cpp-pinyin PRIVATE /wd4251 /wd4267)
endif()

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE "${SHERPA_ONNX_ROOT}/include")
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE "${SHERPA_ONNX_ROOT}/lib")

# Link against the shared library import lib
# The tarball usually contains sherpa-onnx-c-api.lib or similar. 
# Let's assume sherpa-onnx-c-api based on common conventions, but we might need to adjust.
# Based on the shared lib tarball content which I can't see yet, I'll try to link 'sherpa-onnx-c-api'.
# If it fails, I'll inspect the dir.
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE sherpa-onnx-c-api cpp-pinyin::cpp-pinyin)

target_compile_features(${CMAKE_PROJECT_NAME} PRIVATE cxx_std_20)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/plugin-main.cpp src/plugin-config.cpp src/asr-model.cpp src/utils.cpp src/profanity-filter.cpp)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

# Copy the DLLs to the plugin directory so it can run
add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
    "${SHERPA_ONNX_ROOT}/bin/onnxruntime.dll"
    "$<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${SHERPA_ONNX_ROOT}/bin/onnxruntime_providers_shared.dll"
    "$<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>"
    COMMAND ${CMAKE_COMMAND} -E copy
    "${SHERPA_ONNX_ROOT}/lib/sherpa-onnx-c-api.dll"
    "$<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/cpp-pinyin/res/dict"
    "$<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>/dict"
)

# Install dependencies
install(FILES
    "${SHERPA_ONNX_ROOT}/bin/onnxruntime.dll"
    "${SHERPA_ONNX_ROOT}/bin/onnxruntime_providers_shared.dll"
    "${SHERPA_ONNX_ROOT}/lib/sherpa-onnx-c-api.dll"
    DESTINATION "${CMAKE_PROJECT_NAME}/bin/64bit"
)

install(
    DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/cpp-pinyin/res/dict"
    DESTINATION "${CMAKE_PROJECT_NAME}/data"
)

# InnoSetup Installer Configuration
set(UUID_APP "530C2670-1206-4349-9014-550806860710")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/installer/installer.iss.in"
    "${CMAKE_CURRENT_BINARY_DIR}/installer.iss"
    @ONLY
)
