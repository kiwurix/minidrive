#include "shared.hpp"


fs::path g_client_root = fs::current_path();
spdlog::logger* g_log = nullptr;
asio::io_context g_io;
tcp::socket g_socket(g_io);


void print_server_error(const json& response) {
    int code = response.value("code", -1);

    std::cout << "ERROR: " << code << "\n" << error_code_to_string(code) << "\n"
              << response.value("message", "unknown") << std::endl;
}


void print_help() {
    std::cout <<
        "Available commands:\n"
        " HELP\n"
        " LIST [path]\n"
        " UPLOAD <local_path> [remote_path]	\n"
        " DOWNLOAD <remote_path> [local_path]\n"
        " DELETE <path>\n"
        " CD <path>\n"
        " MKDIR <path>\n"
        " RMDIR <path>\n"
        " MOVE <src> <dst>\n"
        " COPY <src> <dst>\n"
        " SYNC <src> <dst>\n"
        " EXIT" << std::endl;
}


bool upload_file(const fs::path& local_path, const std::string& remote_path) {
    if (!fs::exists(local_path) || !fs::is_regular_file(local_path)) {
        std::cout << "Local file not found" << std::endl;
        return false;
    }
    const auto total_size = fs::file_size(local_path);

    json request;
    request["cmd"] = "UPLOAD";
    request["remote_path"] = remote_path;
    request["local_path"] = local_path.string();
    request["size"] = total_size;

    if (!send_json(g_socket, request, g_log)) {
        throw std::runtime_error("Connection lost");
    }

    json response;
    if (!recv_json(g_socket, response, g_log)) {
        throw std::runtime_error("Connection lost");
    }

    if (response.value("status", "") != "OK") {
        print_server_error(response);
        return false;
    }

    std::uint64_t offset = response["data"].value("offset", std::uint64_t{0});

    std::ifstream in(local_path, std::ios::binary);
    if (!in) {
        std::cout << "Failed to open local file" << std::endl;
        return false;
    }

    if (offset > 0) in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);

    char buf[NET_CHUNK_SIZE];

    if (g_log) g_log->info("Start of file byte transmission");
    while (offset < total_size) {
        const auto need = static_cast<std::size_t>(
            std::min<std::uint64_t>(NET_CHUNK_SIZE, total_size - offset));

        in.read(buf, static_cast<std::streamsize>(need));
        const auto n = in.gcount();
        if (n <= 0) break;

        asio::error_code ec;
        asio::write(g_socket, asio::buffer(buf, static_cast<std::size_t>(n)), ec);
        if (ec) {
            std::cout << "Upload failed: " << ec.message() << "\n" << std::endl;
            if (g_log) g_log->info("File byte transmission failed: {}", ec.message());
            return false;
        }

        offset += static_cast<std::uint64_t>(n);
    }

    if (g_log) g_log->info("End of file byte transmission");

    if (!recv_json(g_socket, response, g_log)) {
        throw std::runtime_error("Connection lost");
    }

    std::string status = response.value("status", "");
    if (status != "OK") {
        std::uint64_t received = 0;

        received = response["data"].value("received", std::uint64_t{0});

        print_server_error(response);
        if (received) {
            std::cout << " received=" << received << std::endl;
        }
        return false;
    }

    std::cout << "OK" << std::endl;
    std::cout << "Upload completed" << std::endl;
    return true;
}


bool download_file(const std::string& remote_path, const fs::path& local_path){
    if (fs::exists(local_path)) {
        std::cout << "File already exists" << std::endl;
        return false;
    }

    fs::path target = local_path.string() + ".part";

    std::uint64_t offset = 0;

    if (fs::exists(target) && fs::is_regular_file(target)) {
        auto size = fs::file_size(target);
        if (size > 512) {
            offset = size - 512;
        } else {
            std::error_code ec;
            fs::remove(target, ec);
        }
    }

    json header;
    header["cmd"] = "DOWNLOAD";
    header["path"] = remote_path;
    header["offset"] = offset;

    if (!send_json(g_socket, header, g_log)) {
        throw std::runtime_error("Connection lost");
    }

    json info;
    if (!recv_json(g_socket, info, g_log)) {
        throw std::runtime_error("Connection lost");
    }
    if (info.value("status", "") != "OK") {
        print_server_error(info);
        return false;
    }

    const auto size = static_cast<std::uint64_t>(info["data"].value("size", 0));

    if (!target.parent_path().empty()) {
        std::error_code ec;
        fs::create_directories(target.parent_path(), ec);
        if (ec) {
            std::cout << "Failed to create local directories: " << ec.message() << std::endl;
            return false;
        }
    }

    std::fstream out(target, std::ios::binary | std::ios::in | std::ios::out);

    if (!out) {
        out.open(target, std::ios::binary | std::ios::out | std::ios::trunc);
        if (!out) {
            std::cout << "Failed to open local .part file" << std::endl;
            return false;
        }

        char header_buf[512] = {0};

        std::string metadata = remote_path;
        if (metadata.size() >= sizeof(header_buf)) metadata.resize(sizeof(header_buf) - 1);

        std::memcpy(header_buf, metadata.data(), metadata.size());
        out.write(header_buf, sizeof(header_buf));

        if (!out) {
            std::cout << "Write to .part failed" << std::endl;
            return false;
        }
    }

    out.seekp(static_cast<std::streamoff>(512 + offset), std::ios::beg);

    char net_buf[NET_CHUNK_SIZE];

    if (g_log) g_log->info("Start of file byte reception");
    while (offset < size) {
        const auto need = static_cast<std::size_t>(
            std::min<std::uint64_t>(NET_CHUNK_SIZE, size - offset));

        asio::error_code ec;
        const auto n = asio::read(g_socket, asio::buffer(net_buf, need), ec);

        if (ec) {
            std::cout << "Download failed: " << ec.message() << std::endl;
            if (g_log) g_log->info("File byte reception failed: {}", ec.message());
            return false;
        }

        out.write(net_buf, static_cast<std::streamsize>(n));

        offset += static_cast<std::uint64_t>(n);
    }
    if (g_log) g_log->info("End of file byte reception");

    out.close();

    char local_buf[LOCAL_CHUNK_SIZE];
    try {
        std::ifstream in(target, std::ios::binary);
        in.seekg(512, std::ios::beg);

        std::ofstream final_out(local_path, std::ios::binary | std::ios::trunc);
        if (!final_out) {
            std::cout << "Failed to create final file" << std::endl;
            return false;
        }

        while (in) {
            in.read(local_buf, sizeof(local_buf));
            std::streamsize n = in.gcount();
            if (n <= 0) break;
            final_out.write(local_buf, n);
        }

        final_out.close();
        in.close();
        fs::remove(target);

    } catch (const std::exception& e) {
        std::cerr << "Finalize failed: " << e.what() << std::endl;;
        return false;
    }

    std::cout << "OK" << std::endl;
    std::cout << "Download completed" << std::endl;
    return true;
}


std::string join_path(const std::string& cwd, const std::string& arg) {
    if (arg.empty()) return cwd;
    if (arg[0] == '/') {
        return arg;
    }
    if (cwd == "/" || cwd == ".") {
        return "/" + arg;
    }
    return cwd + "/" + arg;
}


void cmd_exit() {
    json req;
    req["cmd"] = "EXIT";
    send_json(g_socket, req, g_log);
}


void handle_signal(int){
    cmd_exit();
    g_socket.close();
    g_io.stop();

    std::exit(0);
}


void cmd_help() {
    print_help();
}


void cmd_list(const std::string& path) {
    json req;
    req["cmd"] = "LIST";
    req["path"] = path;
    send_json(g_socket, req, g_log);
    json resp;
    if (!recv_json(g_socket, resp, g_log)) {
        throw std::runtime_error("Connection lost");
    }
    if (resp.value("status", "") != "OK") {
        print_server_error(resp);
        return;
    }

    std::cout << "OK" << std::endl;
    for (const auto& item : resp["data"]) {
        std::cout << (item["is_dir"].get<bool>() ? "[D] " : "    ")
                  << item["name"].get<std::string>() << std::endl;;
    }
}


void cmd_mkdir(const std::string& path) {
    json req;
    req["cmd"] = "MKDIR";
    req["path"] = path;
    send_json(g_socket, req, g_log);
    json resp;
    recv_json(g_socket, resp, g_log);
    if (resp.value("status", "") != "OK") {
        print_server_error(resp);
    } else {
        std::cout << "OK" << std::endl;
        std::cout << resp.value("message", "") << std::endl;;
    }
}


bool cmd_delete(const std::string& path) {
    json req;
    req["cmd"] = "DELETE";
    req["path"] = path;
    send_json(g_socket, req, g_log);
    json resp;
    recv_json(g_socket, resp, g_log);
    if (resp.value("status", "") != "OK") {
        print_server_error(resp);
        return false;
    } else {
        std::cout << "OK" << std::endl;
        std::cout << resp.value("message", "") << std::endl;
        return true;
    }
}


void cmd_rmdir(const std::string& path) {
    json req;
    req["cmd"] = "RMDIR";
    req["path"] = path;
    send_json(g_socket, req, g_log);
    json resp;
    recv_json(g_socket, resp, g_log);
    if (resp.value("status", "") != "OK") {
        print_server_error(resp);
    } else {
        std::cout << "OK" << std::endl;
        std::cout << resp.value("message", "") << std::endl;
    }
}


void cmd_move(const std::string& src, const std::string& dst) {
    json req;
    req["cmd"] = "MOVE";
    req["src"] = src;
    req["dst"] = dst;
    send_json(g_socket, req, g_log);
    json resp;
    recv_json(g_socket, resp, g_log);
    if (resp.value("status", "") != "OK") {
        print_server_error(resp);
    } else {
        std::cout << "OK" << std::endl;
        std::cout << resp.value("message", "") << std::endl;
    }
}


void cmd_copy(const std::string& src, const std::string& dst) {
    json req;
    req["cmd"] = "COPY";
    req["src"] = src;
    req["dst"] = dst;
    send_json(g_socket, req, g_log);
    json resp;
    recv_json(g_socket, resp, g_log);
    if (resp.value("status", "") != "OK") {
        print_server_error(resp);
    } else {
        std::cout << "OK" << std::endl;
        std::cout << resp.value("message", "") << std::endl;
    }
}


bool get_remote_hashes(const std::string& remote_dir, json& out_hashes) {
    json req;
    req["cmd"] = "SYNC";
    req["path"] = remote_dir;
    if (!send_json(g_socket, req, g_log)) throw std::runtime_error("Connection lost");;

    json resp;
    if (!recv_json(g_socket, resp, g_log)) throw std::runtime_error("Connection lost");;

    if (resp.value("status", "") != "OK") {
        print_server_error(resp);
        return false;
    }

    out_hashes = resp["data"];
    return true;
}


void do_sync(const fs::path& local_dir, const std::string& remote_dir) {
    if (!fs::exists(local_dir)) {
        std::cout << "Local directory not found" << std::endl;
        return;
    }

    json remote_hashes;
    if (!get_remote_hashes(remote_dir, remote_hashes)) {
        return;
    }

    json local_hashes = json::object();
    try {
        for (auto it = fs::recursive_directory_iterator(local_dir);
             it != fs::recursive_directory_iterator(); ++it) {
            if (!fs::is_regular_file(*it)) continue;
            fs::path rel = fs::relative(it->path(), local_dir);
            local_hashes[rel.string()] = sha256_file_hex(it->path());
             }
    }catch (const std::filesystem::filesystem_error& e) {
        std::cout << "Failed to access local directory: " << e.code().message() << std::endl;
        return;
    }

    std::cout << "Hashes collected" << std::endl;

    std::vector<std::string> files_to_delete;
    std::vector<std::string> files_to_upload;
    std::vector<std::string> dirs_to_remove;

    std::vector<std::string> errors;

    for (auto it = local_hashes.begin(); it != local_hashes.end(); ++it) {
        const std::string& rel_path = it.key();
        const std::string local_hash = it.value().get<std::string>();

        auto r = remote_hashes.find(rel_path);
        if (r == remote_hashes.end()) {
            files_to_upload.push_back(rel_path);
        } else {
            const std::string remote_hash = r.value().get<std::string>();
            if (remote_hash != local_hash) {
                files_to_delete.push_back(rel_path);
                files_to_upload.push_back(rel_path);
            }
        }
    }

    for (auto it = remote_hashes.begin(); it != remote_hashes.end(); ++it) {
        const std::string& rel_path = it.key();
        if (!local_hashes.contains(rel_path)) {
            files_to_delete.push_back(rel_path);

            fs::path p(rel_path);
            fs::path dir = p.parent_path();

            while (!dir.empty()) {
                fs::path local_path = local_dir / dir;
                if (!fs::exists(local_path)) {
                    if (std::find(dirs_to_remove.begin(), dirs_to_remove.end(), dir) == dirs_to_remove.end()) {
                        dirs_to_remove.push_back(dir);
                    }
                    dir = dir.parent_path();
                }else {
                    break;
                }
            }
        }
    }

    std::cout << "Files to delete: " << files_to_delete.size() << std::endl;
    std::size_t delete_success = 0;
    for (const auto& rel_path : files_to_delete) {
        std::string remote_path = join_path(remote_dir, rel_path);

        if (cmd_delete(remote_path)) {
            ++delete_success;
        }else {
            errors.push_back(std::string("Failed to delete file: '") + remote_path + "'");
        }
    }

    for (const auto& rel_dir : dirs_to_remove) {
        std::string remote_path = join_path(remote_dir, rel_dir);
        cmd_rmdir(remote_path);
    }

    std::cout << "Files to upload: " << files_to_upload.size() << std::endl;
    std::size_t upload_success = 0;
    for (const auto& rel_path : files_to_upload) {
        fs::path local_path   = local_dir / rel_path;
        std::string remote_path = join_path(remote_dir, rel_path);

        if (upload_file(local_path, remote_path)) {
            ++upload_success;
        }else {
            errors.push_back(std::string("Failed to upload file '") + local_dir.string() + "'");
        }
    }

    std::cout << "OK" << std::endl;
    std::cout << "SYNC completed. Uploaded: " << upload_success << ", deleted: " << delete_success << std::endl;

    for (const auto& error : errors) {
        std::cout << error << std::endl;
    }
}


void resume_transfers(const std::vector<std::pair<std::string, std::string>>& incomplete_uploads,
    const std::vector<std::pair<std::string, std::string>>& incomplete_downloads){

    if (incomplete_uploads.empty() && incomplete_downloads.empty())
        return;

    std::cout << "Incomplete upload/downloads detected, resume? (y/n): ";
    std::string ans;

    std::getline(std::cin, ans);
    if (ans != "y" && ans != "Y") {
        for (const auto& p : incomplete_uploads | std::views::keys) {
            const std::string& local_path = p;
            std::error_code ec;
            fs::remove(local_path, ec);
            if (ec) {
                std::cout << "Failed to remove local file: " << local_path << " : " << ec.message() << std::endl;
            }
        }

        for (const auto& p : incomplete_downloads | std::views::values) {
            const std::string& remote_path = p;
            cmd_delete(remote_path);
        }
        return;
    }

    for (const auto& p : incomplete_uploads) {
        const std::string& local_rel  = p.first;
        const std::string& remote_str = p.second;

        fs::path local_path = g_client_root / local_rel;
        std::cout << "UPLOAD " << local_rel << " -> " << remote_str << std::endl;
        upload_file(local_path, remote_str);
    }

    for (const auto& p : incomplete_downloads) {
        const std::string& local_rel  = p.first;
        const std::string& remote_str = p.second;

        fs::path final_path = g_client_root / local_rel;
        std::cout << "DOWNLOAD " << remote_str << " -> " << local_rel << std::endl;
        download_file(remote_str, final_path);
    }
}


bool handle_auth(const std::string& username) {
    json first;
    first["username"] = username;
    if (!send_json(g_socket, first, g_log)) throw std::runtime_error("Connection lost");

    json resp;
    if (!recv_json(g_socket, resp, g_log)) {
        throw std::runtime_error("Connection lost");
    }

    std::string status = resp.value("status", "");

    if (username.empty()) {
        if (status == "OK" && resp["data"].value("mode", "") == "public") {
            std::cout << "[warning] operating in public mode - files are visible to everyone" << std::endl;
            return true;
        }

        std::cerr << "Unexpected auth response" << std::endl;
        return false;
    }

    if (status == "OK" && resp.value("message", "") == "need_password") {
        std::string password;
        std::cout << "Password: ";

        std::getline(std::cin, password);
        std::erase_if(password, [](unsigned char c) { return c >= 0x80; });

        json pw;
        pw["password"] = password;
        if (!send_json(g_socket, pw, g_log) || !recv_json(g_socket, resp, g_log)) {
            throw std::runtime_error("Connection lost");
        }

        std::string st2 = resp.value("status", "");
        if (st2 == "OK" && resp.contains("data") && resp["data"].value("mode", "") == "private") {

            std::vector<std::pair<std::string, std::string>> incomplete_uploads;
            for (const auto& j : resp["data"]["incomplete_uploads"]) {
                std::string remote  = j[0].get<std::string>();
                std::string local = j[1].get<std::string>();

                incomplete_uploads.emplace_back(local, remote);

            }

            auto incomplete_downloads = list_incomplete_transfers(g_client_root);

            std::cout << "Logged as " << username << std::endl;

            resume_transfers(incomplete_uploads, incomplete_downloads);
            return true;
        }

        print_server_error(resp);
        return false;
   }

    if (status == "OK" && resp.value("message", "") == "need_register") {
        std::cout << "User " << username << " not found. Register? (y/n): ";
        std::string user_answer;

        std::getline(std::cin, user_answer);
        if (user_answer != "y" && user_answer != "Y") {
            std::cout << "Registration cancelled." << std::endl;
            return false;
        }

        std::string password;
        std::cout << "Password: ";

        std::getline(std::cin, password);
        json reg;
        reg["password"] = password;
        if (!send_json(g_socket, reg, g_log) || !recv_json(g_socket, resp, g_log)) {
            throw std::runtime_error("Connection lost");
        }

        std::string st2 = resp.value("status", "");
        if (st2 == "OK" && resp["data"].value("mode", "") == "registered") {
            std::cout << "User registered successfully." << std::endl;
            return false;
        }

        print_server_error(resp);
        return false;
    }

    std::cerr << "Unexpected auth response" << std::endl;
    return false;
}


std::vector<std::string> split_tokens(const std::string& line) {
    std::istringstream iss(line);
    std::vector<std::string> tokens;

    for (std::string word; iss >> word; ) {
        tokens.push_back(std::move(word));
    }
    return tokens;
}


int repl(std::string &username) {
    std::string cwd = "/";
    print_help();
    if (username.empty()) username = "public";

    while (true) {
        try {
            std::cout << username << ":" << cwd << "> ";
            std::string line;

            std::getline(std::cin, line);
            auto tokens = split_tokens(line);
            if (tokens.empty()) continue;

            const std::string& cmd = tokens[0];
            if (g_log) g_log->info("USER_ENTER: {}", line);

            if (cmd == "EXIT") {
                cmd_exit();
                return 0;
            }

            if (cmd == "HELP") {
                cmd_help();
                continue;
            }

            if (cmd == "LIST") {
                std::string path = tokens.size() > 1? join_path(cwd, tokens[1]) : cwd;
                cmd_list(path);
                continue;
            }

            if (cmd == "MKDIR" && tokens.size() >= 2) {
                std::string path = join_path(cwd, tokens[1]);
                cmd_mkdir(path);
                continue;
            }

            if (cmd == "DELETE" && tokens.size() >= 2) {
                std::string path = join_path(cwd, tokens[1]);
                cmd_delete(path);
                continue;
            }

            if (cmd == "RMDIR" && tokens.size() >= 2) {
                std::string path = join_path(cwd, tokens[1]);
                cmd_rmdir(path);
                continue;
            }

            if (cmd == "MOVE" && tokens.size() >= 3) {
                std::string src = join_path(cwd, tokens[1]);
                std::string dst = join_path(cwd, tokens[2]);
                cmd_move(src, dst);
                continue;
            }

            if (cmd == "COPY" && tokens.size() >= 3) {
                std::string src = join_path(cwd, tokens[1]);
                std::string dst = join_path(cwd, tokens[2]);
                cmd_copy(src, dst);
                continue;
            }

            if (cmd == "UPLOAD" && tokens.size() >= 2) {
                fs::path local = join_path(g_client_root, tokens[1]);
                std::string remote = tokens.size() >= 3 ? join_path(cwd, tokens[2])
                                                        : join_path(cwd, local.filename().string());
                upload_file(local, remote);
                continue;
            }

            if (cmd == "DOWNLOAD" && tokens.size() >= 2) {
                const std::string remote = join_path(cwd, tokens[1]);
                fs::path local = (tokens.size() >= 3) ?
                fs::path(join_path(g_client_root.string(), tokens[2]))
                                                        : g_client_root / fs::path(tokens[1]).filename();
                download_file(remote, local);
                continue;
            }

            if (cmd == "SYNC" && tokens.size() >= 3) {
                fs::path local_dir = join_path(g_client_root, tokens[1]);
                const std::string remote_dir = join_path(cwd, tokens[2]);
                do_sync(local_dir, remote_dir);
                continue;
            }

            if (cmd == "CD" && tokens.size() >= 2) {
                std::string path = join_path(cwd, tokens[1]);
                json req;
                req["cmd"] = "CD";
                req["path"] = path;
                send_json(g_socket, req, g_log);
                json resp;
                if (!recv_json(g_socket, resp, g_log)) {
                    throw std::runtime_error("Connection lost");
                }
                if (resp.value("status", "") != "OK") {
                    print_server_error(resp);
                    continue;
                }
                cwd = resp.value("message", "/");
                std::cout << "OK" << std::endl;
                continue;
            }

            std::cout << "Unknown or invalid command. Type HELP." << std::endl;
        } catch (const std::exception& e) {
            if (std::string(e.what()) == "Connection lost") {
                std::cout << "Connection lost" << std::endl;
                return -1;
            }
            std::cerr << "Client error: " << e.what() << std::endl;
        }
    }
}


int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Usage: ./client [username@]<server_ip>:<port> [--log <log_file>]" << std::endl;
        return 1;
    }

    std::string endpoint = argv[1];
    std::string username;
    std::string server_ip;

    auto at_pos = endpoint.find('@');
    auto host_part = endpoint;
    if (at_pos != std::string::npos) {
        username = endpoint.substr(0, at_pos);
        host_part = endpoint.substr(at_pos + 1);
    }

    auto colon_pos = host_part.rfind(':');
    if (colon_pos == std::string::npos) {
        std::cout << "Invalid endpoint format, expected ip:port" << std::endl;
        return 1;
    }

    server_ip = host_part.substr(0, colon_pos);
    std::string port_str = host_part.substr(colon_pos + 1);
    int port = 0;
    try {
        port = std::stoi(port_str);
    } catch (...) {
        std::cout << "Invalid port" << std::endl;
        return 1;
    }

    if (argc > 2) {
        if (std::string(argv[2]) == "--log") {
            try {
                auto log_path = join_path(g_client_root, argv[3]);
                g_log = spdlog::basic_logger_mt("client_log", log_path).get();
                spdlog::set_pattern("[%Y-%m-%d %H:%M:%S] [%l] %v");
            }catch (const std::exception& e) {
                std::cout << "Error creating log file: " << e.what() << std::endl;
                return 1;
            }

        }else {
            std::cout << "Usage: ./client [username@]<server_ip>:<port> [--log <log_file>]" << std::endl;
            return 1;
        }
    }

    try {
        tcp::resolver resolver(g_io);
        auto endpoints = resolver.resolve(server_ip, std::to_string(port));
        asio::connect(g_socket, endpoints);

        std::signal(SIGTERM, handle_signal);
        std::signal(SIGINT,  handle_signal);

        if (!handle_auth(username)) {
            return 0;
        }

        return repl(username);
    } catch (const std::exception& e) {
        std::cout << "Client error: " << e.what() << std::endl;
        return 1;
    }
}
