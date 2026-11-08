#pragma once

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <filesystem>
#include <asio.hpp>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <csignal>


namespace fs = std::filesystem;
using json = nlohmann::json;
using asio::ip::tcp;


constexpr size_t SALT_BYTES = 16;
constexpr size_t HASH_BYTES = 32;
constexpr int PBKDF2_ITER  = 100000;
constexpr size_t NET_CHUNK_SIZE = 1024 * 64;
constexpr size_t LOCAL_CHUNK_SIZE = 1024 * 1024 * 4;


std::string bytes_to_hex(const unsigned char* data, std::size_t len);
std::vector<unsigned char> hex_to_bytes(const std::string& hex);


std::string random_salt_hex();
std::string pbkdf2_sha256_hex(const std::string& password, const std::string& salt_hex);

std::string sha256_file_hex(const fs::path& p);


bool send_json(tcp::socket& socket, const json& j, spdlog::logger* log = nullptr);
bool recv_json(tcp::socket& socket, json& out, spdlog::logger* log = nullptr);


enum class ErrorCode {
    Ok = 0,
    NotFound = 1,
    PermissionDenied = 2,
    InvalidCommand = 3,
    InternalError = 4,
    AuthFailed = 5,
    AlreadyExists = 6
};


const char* error_code_to_string(int code);


json make_message(ErrorCode ec, const std::string& message, const json& data = json::object());


std::vector<std::pair<std::string, std::string>> list_incomplete_transfers(const fs::path& root);
