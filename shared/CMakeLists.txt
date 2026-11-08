add_library(minidrive_shared STATIC
    src/version.cpp
        include/shared.hpp
        src/shared.cpp
)

target_include_directories(minidrive_shared
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

find_package(OpenSSL REQUIRED)

target_link_libraries(minidrive_shared
    PUBLIC
        OpenSSL::Crypto
        asio::asio
        nlohmann_json::nlohmann_json
        libsodium::libsodium
        spdlog::spdlog_header_only
    PUBLIC
        minidrive_warnings
)

set_target_properties(minidrive_shared PROPERTIES EXPORT_NAME shared)
