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

# Zlib / Minizip
FetchContent_Declare(
  zlib
  URL https://github.com/madler/zlib/archive/refs/tags/v1.3.1.zip
)
FetchContent_MakeAvailable(zlib)

# Curl (Static with Schannel on Windows)
set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
set(CURL_USE_OPENSSL OFF CACHE BOOL "" FORCE)
set(CURL_ENABLE_SSL ON CACHE BOOL "" FORCE)
FetchContent_Declare(
  curl
  URL https://github.com/curl/curl/releases/download/curl-8_11_0/curl-8.11.0.zip
)
FetchContent_MakeAvailable(curl)

# Minizip sources
set(MINIZIP_SOURCES
    ${zlib_SOURCE_DIR}/contrib/minizip/ioapi.c
    ${zlib_SOURCE_DIR}/contrib/minizip/unzip.c
)

if(WIN32)
    list(APPEND MINIZIP_SOURCES ${zlib_SOURCE_DIR}/contrib/minizip/iowin32.c)
endif()


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

# Try to find OpenSSL to copy DLLs
find_package(OpenSSL)
if (OPENSSL_FOUND)
    message(STATUS "Found OpenSSL: ${OPENSSL_VERSION}")
    # Try to locate the DLLs
    # On Windows, they are often in the bin directory next to the lib directory
    get_filename_component(OPENSSL_LIB_DIR ${OPENSSL_CRYPTO_LIBRARY} DIRECTORY)
    get_filename_component(OPENSSL_ROOT_DIR_GUESS ${OPENSSL_LIB_DIR} DIRECTORY)
    
    file(GLOB OPENSSL_DLLS "${OPENSSL_ROOT_DIR_GUESS}/bin/libssl*.dll" "${OPENSSL_ROOT_DIR_GUESS}/bin/libcrypto*.dll")
    
    if (OPENSSL_DLLS)
        message(STATUS "Found OpenSSL DLLs: ${OPENSSL_DLLS}")
        # Not copying OpenSSL DLLs anymore as we use curl with Schannel
        # foreach(DLL ${OPENSSL_DLLS})
        #      add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
        #         COMMAND ${CMAKE_COMMAND} -E copy_if_different
        #         "${DLL}"
        #         "$<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>"
        #     )
        #     install(FILES "${DLL}" DESTINATION "${CMAKE_PROJECT_NAME}/bin/64bit")
        # endforeach()
    else()
        message(WARNING "OpenSSL found but DLLs not found in ${OPENSSL_ROOT_DIR_GUESS}/bin. You may need to manually copy libssl and libcrypto DLLs.")
    endif()
endif()

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

# Zlib/Minizip includes
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${zlib_SOURCE_DIR}
    ${zlib_SOURCE_DIR}/contrib/minizip
    ${zlib_BINARY_DIR}
    ${curl_SOURCE_DIR}/include
    ${curl_BINARY_DIR}/include
)

# Link against the shared library import lib
# The tarball usually contains sherpa-onnx-c-api.lib or similar. 
# Let's assume sherpa-onnx-c-api based on common conventions, but we might need to adjust.
# Based on the shared lib tarball content which I can't see yet, I'll try to link 'sherpa-onnx-c-api'.
# If it fails, I'll inspect the dir.
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE sherpa-onnx-c-api cpp-pinyin::cpp-pinyin zlibstatic libcurl)

target_compile_features(${CMAKE_PROJECT_NAME} PRIVATE cxx_std_20)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE 
    src/plugin-main.cpp 
    src/plugin-config.cpp 
    src/model-manager.cpp 
    src/asr-model.cpp 
    src/utils.cpp 
    src/profanity-filter.cpp 
    src/video-delay.cpp
    ${MINIZIP_SOURCES}
)

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
    COMMAND ${CMAKE_COMMAND} -E copy
    "${CMAKE_CURRENT_SOURCE_DIR}/data/models.json"
    "$<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>/data/models.json"
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

install(
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/data/models.json"
    DESTINATION "${CMAKE_PROJECT_NAME}/data"
)

# InnoSetup Installer Configuration
set(UUID_APP "530C2670-1206-4349-9014-550806860710")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/installer/installer.iss.in"
    "${CMAKE_CURRENT_BINARY_DIR}/installer.iss"
    @ONLY
)
