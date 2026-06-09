project(XLS20PredictorAgent VERSION 1.2.0 LANGUAGES CXX)

# Configure C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Define project directories
set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Find Required Dependencies
find_package(Threads REQUIRED)
find_package(Boost REQUIRED COMPONENTS system)
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json REQUIRED)

# Collect all source files
set(SOURCES
    ${SRC_DIR}/network/ws_client.cpp
    ${SRC_DIR}/network/http_client.cpp          # Phase 5: async HTTP GET/POST client
    ${SRC_DIR}/network/webhook_dispatcher.cpp   # Phase 5: outbound signal alert engine
    ${SRC_DIR}/parser/tx_filter.cpp
    ${SRC_DIR}/model/price_bucket.cpp
    ${SRC_DIR}/model/holt_winters.cpp
    ${SRC_DIR}/registry/collection_registry.cpp
    ${SRC_DIR}/output/forecast_printer.cpp
    ${SRC_DIR}/main.cpp
    # Note: src/model/calibration.hpp is header-only — no .cpp entry needed.
    # It is on the include path via ${SRC_DIR} below.
)

# Define the executable
add_executable(${PROJECT_NAME} ${SOURCES})

# Include project headers and dependency directories
target_include_directories(${PROJECT_NAME} PRIVATE 
    ${SRC_DIR}
)

# Link Libraries
target_link_libraries(${PROJECT_NAME} PRIVATE
    Threads::Threads
    Boost::system
    OpenSSL::SSL
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
)

# Compiler-specific optimization flags
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -O3)
elseif(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4 /O2)
endif()
