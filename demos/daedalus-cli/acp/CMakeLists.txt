# ACP Client Library CMakeLists.txt
# Agent Communication Protocol C++23 client implementation

cmake_minimum_required(VERSION 3.28)

# ACP library
set(ACP_LIB_NAME acp)

# Find uuid + curl via pkg-config (we need PkgConfig before pkg_check_modules).
find_package(PkgConfig REQUIRED)
pkg_check_modules(UUID REQUIRED uuid)

# Headers (relative to this CMakeLists.txt location)
set(ACP_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/include/acp/AcpFwd.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/acp/AcpClient.h
)

# Sources (relative to this CMakeLists.txt location)
set(ACP_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/AcpClient.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/AcpTypes.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/AcpTransport.cpp
)

# Create library
add_library(${ACP_LIB_NAME} STATIC ${ACP_SOURCES} ${ACP_HEADERS})

# Include directories - public include is at include/acp/
target_include_directories(${ACP_LIB_NAME} PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${UUID_INCLUDE_DIRS}
)

# C++23
target_compile_features(${ACP_LIB_NAME} PRIVATE cxx_std_23)

# Dependencies
target_link_libraries(${ACP_LIB_NAME} PRIVATE
    nlohmann_json::nlohmann_json
    CURL::libcurl
    ${UUID_LIBRARIES}
)

# Warning flags
target_compile_options(${ACP_LIB_NAME} PRIVATE
    -Wall -Wextra -Wpedantic
)

# Install headers
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/acp/
    DESTINATION include/acp
    FILES_MATCHING PATTERN "*.h"
)

# Build tests if enabled
if(ACP_BUILD_TESTS)
    add_subdirectory(tests)
endif()
