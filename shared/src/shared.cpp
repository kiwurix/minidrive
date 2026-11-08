#include "shared.hpp"
#include <openssl/rand.h>


std::string bytes_to_hex(const unsigned char* data, size_t len) {
    static const char* digits = "0123456789ABCDEF";
    std::string out;
    out.resize(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out[2 * i] = digits[data[i] >> 4 & 0xF];
        out[2 * i + 1] = digits[data[i] & 0xF];
    }
    return out;
}


std::vector<unsigned char> hex_to_bytes(const std::string& hex) {
    std::vector<unsigned char> out;
    out.reserve(hex.size() / 2);

    auto hex_val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };

    for (std::size_t i = 0; i < hex.size(); i += 2) {
        int hi = hex_val(hex[i]);
        int lo = hex_val(hex[i + 1]);
        out.push_back(static_cast<unsigned char>((hi << 4) | lo));
    }
    return out;
}



std::string random_salt_hex() {
    unsigned char salt[SALT_BYTES];
    if (RAND_bytes(salt, SALT_BYTES) != 1) throw std::runtime_error("RAND_bytes failed");

    return bytes_to_hex(salt, SALT_BYTES);
}


std::string pbkdf2_sha256_hex(const std::string& password, const std::string& salt_hex) {
    std::vector<unsigned char> salt = hex_to_bytes(salt_hex);
    if (salt.empty()) throw std::runtime_error("Invalid salt hex");

    unsigned char out[HASH_BYTES];

    if (PKCS5_PBKDF2_HMAC(password.c_str(), static_cast<int>(password.size()), salt.data(),
                          static_cast<int>(salt.size()), PBKDF2_ITER, EVP_sha256(),
                          HASH_BYTES, out)!= 1) {
        throw std::runtime_error("PBKDF2 failed");
    }

    return bytes_to_hex(out, HASH_BYTES);
}


std::string sha256_file_hex(const fs::path& p) {
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return "";

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        std::fclose(f);
        return "";
    }

    const EVP_MD* md = EVP_sha256();
    if (EVP_DigestInit_ex(ctx, md, nullptr) != 1) {
        EVP_MD_CTX_free(ctx);
        std::fclose(f);
        return "";
    }

    unsigned char buf[NET_CHUNK_SIZE];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        if (EVP_DigestUpdate(ctx, buf, n) != 1) {
            EVP_MD_CTX_free(ctx);
            std::fclose(f);
            return "";
        }
    }
    std::fclose(f);

    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int out_len = 0;
    if (EVP_DigestFinal_ex(ctx, out, &out_len) != 1) {
        EVP_MD_CTX_free(ctx);
        return "";
    }

    EVP_MD_CTX_free(ctx);
    return bytes_to_hex(out, out_len);
}


bool send_json(tcp::socket& socket, const json& j, spdlog::logger* log) {
    const std::string data = j.dump();
    auto len = static_cast<std::uint32_t>(data.size());

    std::uint32_t net_len = htonl(len);

    asio::error_code ec;
    asio::write(socket, asio::buffer(&net_len, sizeof(net_len)), ec);
    if (ec) return false;

    asio::write(socket, asio::buffer(data.data(), data.size()), ec);
    if (log) log->info("SEND: {}", data);

    return !ec;
}


bool recv_json(tcp::socket& socket, json& out, spdlog::logger* log) {
    std::uint32_t net_len = 0;
    asio::error_code ec;

    asio::read(socket, asio::buffer(&net_len, sizeof(net_len)), ec);
    if (ec) return false;

    std::uint32_t len = ntohl(net_len);
    if (len == 0) return false;

    std::string buf(len, '\0');

    asio::read(socket, asio::buffer(buf.data(), buf.size()), ec);
    if (ec) return false;

    try {
        out = json::parse(buf);
        if (log) log->info("RECV: {}", out.dump());

        return true;
    } catch (...) {
        return false;
    }
}


const char* error_code_to_string(int code) {
    switch (static_cast<ErrorCode>(code)) {
        case ErrorCode::Ok: return "OK"   ;
        case ErrorCode::NotFound: return "Not found";
        case ErrorCode::PermissionDenied: return "Permission denied";
        case ErrorCode::InvalidCommand: return "Invalid command";
        case ErrorCode::InternalError: return "Internal error";
        case ErrorCode::AuthFailed: return "Authentication error";
        case ErrorCode::AlreadyExists: return "Already exists";
        default: return "Unknown error";
    }
}


json make_message(ErrorCode ec, const std::string& message, const json& data) {
    int error_code = static_cast<int>(ec);
    const bool ok = (ec == ErrorCode::Ok);

    return json{
            {"status",  ok ? "OK" : "error"},
            {"code",    error_code},
            {"message", message},
            {"data",    data}
    };
}


std::vector<std::pair<std::string, std::string>> list_incomplete_transfers(const fs::path& root) {
    std::vector<std::pair<std::string, std::string>> result;

    try {
        for (auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator(); ++it) {

            if (!fs::is_regular_file(*it)) continue;

            const fs::path local_path = it->path();
            const std::string name = local_path.filename().string();

            if (name.size() <= 5 || name.substr(name.size() - 5) != ".part") continue;

            std::ifstream in(local_path, std::ios::binary);
            if (!in) continue;

            char header[512] = {0};
            in.read(header, sizeof(header));
            const std::streamsize n = in.gcount();
            if (n <= 0) continue;

            char* end = std::find(header, header + n, '\0');
            char* nl  = std::find(header, header + n, '\n');
            if (nl < end) end = nl;

            std::string remote(header, end);
            if (remote.empty()) continue;

            fs::path rel_path = fs::relative(local_path, root);
            rel_path.replace_extension();

            result.emplace_back(rel_path.string(), remote);
        }
    } catch (const std::exception& e) {
        std::cerr << "Scan .part error: " << e.what() << "\n";
    }

    return result;
}
