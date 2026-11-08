#include <shared.hpp>


std::atomic_bool g_running{true};
asio::io_context g_io;
tcp::acceptor g_acceptor{g_io};

std::mutex g_sockets_lock;
std::vector<std::shared_ptr<tcp::socket>> g_sockets;

spdlog::logger* g_log = nullptr;


void handle_signal(int){
    g_running = false;
    g_acceptor.close();

    std::lock_guard lk(g_sockets_lock);
    for (auto& socket : g_sockets) {
        if (!socket) continue;

        socket->close();
    }

    std::exit(0);
}


struct UserCred {
    std::string salt_hex;
    std::string hash_hex;
};

class UserDb {
public:
    explicit UserDb(const fs::path& root) : db_path_(root / "users.db") {
        load();
    }

    [[nodiscard]] bool has_user(const std::string& name) const {
        return users_.contains(name);
    }

    [[nodiscard]] UserCred get_user(const std::string& name) const {
        const auto& j = users_.at(name);
        return { j.at("salt").get<std::string>(), j.at("hash").get<std::string>() };
    }

    void add_user(const std::string& name, const std::string& password) {
        std::string salt = random_salt_hex();
        std::string hash = pbkdf2_sha256_hex(password, salt);

        users_[name] = json{{"salt", salt}, {"hash", hash}};
        save();
    }

private:
    fs::path db_path_;
    json users_ = json::object();

    void load() {
        if (!fs::exists(db_path_)) return;
        std::ifstream in(db_path_);
        if (!in) return;
        try {
            in >> users_;
        } catch (...) {
            users_ = json::object();
        }
    }

    void save () const {
        std::ofstream out(db_path_, std::ios::trunc);
        if (!out) {
            g_log->error("Failed to open users.db for writing");
            return;
        }
        out << users_.dump(2);
    }
};


void cleanup_old_part_files(const fs::path& root) {
    try {
        const auto now = fs::file_time_type::clock::now();
        const auto cutoff = now - std::chrono::hours{1};

        for (auto it = fs::recursive_directory_iterator(root);
             it != fs::recursive_directory_iterator(); ++it) {

            if (!fs::is_regular_file(it->path())) continue;

            std::string name = it->path().filename().string();
            if (name.size() <= 5 || name.substr(name.size() - 5) != ".part")
                continue;

            auto file_time = fs::last_write_time(it->path());
            if (file_time < cutoff) {
                fs::remove(it->path());
                if (g_log) g_log->info("Cleaned old .part: {}", it->path().string());
            }
             }
    } catch (...) {}
}


void cleanup_thread_func(const fs::path& root) {
    while (g_running) {
        cleanup_old_part_files(root);
        std::this_thread::sleep_for(std::chrono::minutes(1));
    }
}


fs::path resolve_user_path(const fs::path& user_root, const std::string& user_enter) {
    fs::path user_path(user_enter);
    user_path = user_path.relative_path();

    fs::path resolved_path = user_root;
    for (const auto& part :user_path) {
        if (part == "." || part.empty())
            continue;
        if (part == "..") {
            if (resolved_path == user_root)
                continue;
            resolved_path = resolved_path.parent_path();
            continue;
        }
        resolved_path /= part;
    }
    return resolved_path;
}


json cmd_list(const fs::path& user_root, const json& request) {
    const std::string local_path = request.value("path", "");
    fs::path local_dir = resolve_user_path(user_root, local_path);

    if (!fs::exists(local_dir) || !fs::is_directory(local_dir)) {
        return make_message(ErrorCode::NotFound, "Directory not found");
    }

    json content = json::array();
    try {
        for (const auto& entry : fs::directory_iterator(local_dir)) {
            json item;
            item["name"] = entry.path().filename().string();
            item["is_dir"] = fs::is_directory(entry);

            content.push_back(item);
        }
    } catch (const std::exception& e) {
        return make_message(ErrorCode::InternalError, e.what());
    }

    return make_message(ErrorCode::Ok, "", content);
}


json cmd_cd(const fs::path& user_root, const json& request) {
    const std::string local_path = request.value("path", "");
    fs::path local_dir = resolve_user_path(user_root, local_path);

    if (!fs::exists(local_dir) || !fs::is_directory(local_dir)) {
        return make_message(ErrorCode::NotFound, "Directory not found");
    }

    fs::path user_path = fs::relative(local_dir, user_root);
    if (user_path.empty() || user_path == ".") {
        user_path = "/";
    } else {
        user_path = "/" / user_path;
    }

    return make_message(ErrorCode::Ok, user_path);
}


json cmd_mkdir(const fs::path& user_root, const json& request) {
    const std::string local_path = request.value("path", "");
    fs::path directory_path = resolve_user_path(user_root, local_path);

    if (fs::exists(directory_path)) {
        return make_message(ErrorCode::AlreadyExists, "Directory already exists");
    }

    try {
        fs::create_directories(directory_path);
        return make_message(ErrorCode::Ok, "Directory created");

    } catch (const std::exception& e) {
        return make_message(ErrorCode::InternalError, e.what());
    }
}


json cmd_delete(const fs::path& user_root, const json& request) {
    const std::string local_path = request.value("path", "");

    fs::path target = resolve_user_path(user_root, local_path);
    if (!fs::exists(target)) {
        return make_message(ErrorCode::NotFound, "File not found");
    }
    if (fs::is_directory(target)) {
        return make_message(ErrorCode::InvalidCommand,
            "DELETE cannot remove directories. Use RMDIR");
    }
    try {
        fs::remove(target);

        return make_message(ErrorCode::Ok, "File deleted");
    } catch (const std::exception& e) {
        return make_message(ErrorCode::InternalError, e.what());
    }
}


json cmd_rmdir(const fs::path& user_root, const json& request) {
    const std::string local_path = request.value("path", "");

    fs::path target = resolve_user_path(user_root, local_path);
    if (!fs::exists(target) || !fs::is_directory(target)) {
        return make_message(ErrorCode::NotFound, "Directory not found");
    }
    try {
        fs::remove_all(target);
        return make_message(ErrorCode::Ok, "Directory removed");

    } catch (const std::exception& e) {
        return make_message(ErrorCode::InternalError, e.what());
    }
}


json cmd_move(const fs::path& user_root, const json& request) {
    const std::string source_path = request.value("src", "");
    const std::string destination_path = request.value("dst", "");

    fs::path absolute_src_path = resolve_user_path(user_root, source_path);
    fs::path absolute_dst_path = resolve_user_path(user_root, destination_path);

    if (!fs::exists(absolute_src_path)) {
        return make_message(ErrorCode::NotFound, "Source not found");
    }

    if (fs::exists(absolute_dst_path)) {
        return make_message(ErrorCode::AlreadyExists, "Destination already exists");
    }

    try {
        if (!absolute_dst_path.parent_path().empty()) {
            fs::create_directories(absolute_dst_path.parent_path());
        }

        fs::rename(absolute_src_path, absolute_dst_path);
        return make_message(ErrorCode::Ok, "Moved");
    } catch (const std::exception& e) {
        return make_message(ErrorCode::InternalError, e.what());
    }
}


json cmd_copy(const fs::path& user_root, const json& request) {
    const std::string source_path = request.value("src", "");
    const std::string destination_path = request.value("dst", "");

    fs::path absolute_src_path = resolve_user_path(user_root, source_path);
    fs::path absolute_dst_path = resolve_user_path(user_root, destination_path);

    if (!fs::exists(absolute_src_path)) {
        return make_message(ErrorCode::NotFound, "Source not found");
    }

    if (fs::exists(absolute_dst_path)) {
        return make_message(ErrorCode::AlreadyExists, "Destination already exists");
    }

    try {
        if (fs::is_directory(absolute_src_path)) {

            if (absolute_dst_path == absolute_src_path ||
            std::mismatch(
                absolute_src_path.begin(), absolute_src_path.end(), absolute_dst_path.begin()
                ).first == absolute_src_path.end()) {

                return make_message(ErrorCode::InvalidCommand, "Cannot copy directory into itself");
            }

            fs::create_directories(absolute_dst_path);
            fs::copy(absolute_src_path, absolute_dst_path, fs::copy_options::recursive);

        } else {
            if (!absolute_dst_path.parent_path().empty()) {
                fs::create_directories(absolute_dst_path.parent_path());
            }
            fs::copy_file(absolute_src_path, absolute_dst_path);
        }

        return make_message(ErrorCode::Ok, "Copied");
    } catch (const std::exception& e) {
        return make_message(ErrorCode::InternalError, e.what());
    }
}


json cmd_sync(const fs::path& user_root, const json& request) {
    const std::string local_path = request.value("path", "");

    fs::path base_path = resolve_user_path(user_root, local_path);
    if (!fs::exists(base_path) || !fs::is_directory(base_path)) {
        return make_message(ErrorCode::NotFound, "Directory not found");
    }

    json content = json::object();
    try {
        for (auto it = fs::recursive_directory_iterator(base_path); it != fs::recursive_directory_iterator(); ++it) {
            if (!fs::is_regular_file(*it)) continue;
            fs::path rel = fs::relative(it->path(), base_path);
            content[rel.string()] = sha256_file_hex(it->path());
        }
    } catch (const std::exception& e) {
        return make_message(ErrorCode::InternalError, e.what());
    }

    return make_message(ErrorCode::Ok, "", content);
}


json dispatch_command(const fs::path& user_root, const json& request) {
    const std::string cmd = request.value("cmd", "");

    if (cmd == "LIST") return cmd_list(user_root, request);
    if (cmd == "CD") return cmd_cd(user_root, request);
    if (cmd == "MKDIR") return cmd_mkdir(user_root, request);
    if (cmd == "DELETE") return cmd_delete(user_root, request);
    if (cmd == "RMDIR") return cmd_rmdir(user_root, request);
    if (cmd == "MOVE") return cmd_move(user_root, request);
    if (cmd == "COPY") return cmd_copy(user_root, request);
    if (cmd == "SYNC") return cmd_sync(user_root, request);
    return make_message(ErrorCode::InvalidCommand, "Unknown cmd");
}


void handle_upload(tcp::socket& socket, const fs::path& user_root, const json& request){
    const std::string local_path = request.value("remote_path", "");
    const std::string remote_path = request.value("local_path", "");
    std::uint64_t total_size = request.value("size", std::uint64_t{0});

    fs::path target = resolve_user_path(user_root, local_path);
    fs::path temp = target.string() + ".part";


    if (fs::exists(target)) {
        send_json(socket, make_message(ErrorCode::AlreadyExists, "File already exists"));
        return;
    }

    json data = json::object();
    std::uint64_t offset = 0;

    if (fs::exists(temp) && fs::is_regular_file(temp)) {
        auto size = fs::file_size(temp);
        if (size > 512) {
            offset = size - 512;
        } else {
            std::error_code ec;
            fs::remove(temp, ec);
        }
    }

    data["offset"] = offset;

    if (!send_json(socket, make_message(ErrorCode::Ok, "", data))) {
        throw std::runtime_error("client disconnected");
    }

    try {
        if (!target.parent_path().empty()) {
            fs::create_directories(target.parent_path());
        }
    } catch (const std::exception& e) {
        send_json(socket, make_message(ErrorCode::InternalError, e.what()));
        return;
    }

    std::fstream out(temp, std::ios::binary | std::ios::in | std::ios::out);

    if (!out) {
        out.open(temp, std::ios::binary | std::ios::out | std::ios::trunc);
        if (!out) {
            send_json(socket, make_message(ErrorCode::PermissionDenied, "Failed to open target file"));
            return;
        }

        char header_buf[512] = {0};
        std::string metadata = remote_path;

        if (metadata.size() >= sizeof(header_buf)) metadata.resize(sizeof(header_buf) - 1);

        std::memcpy(header_buf, metadata.data(), metadata.size());
        out.write(header_buf, sizeof(header_buf));
    }

    out.seekp(static_cast<std::streamoff>(512 + offset), std::ios::beg);

    char net_buf[NET_CHUNK_SIZE];

    while (offset < total_size) {
        const auto need = static_cast<std::size_t>(
            std::min<std::uint64_t>(NET_CHUNK_SIZE, total_size - offset));

        asio::error_code ec;
        std::size_t n = asio::read(socket, asio::buffer(net_buf, need), ec);
        if (ec) {
            g_log->error("Upload read error: {}", ec.message());
            break;
        }

        out.write(net_buf, static_cast<std::streamsize>(n));
        if (!out) {
            g_log->error("Download write error: {}", ec.message());
            break;
        }

        offset += n;
    }

    out.close();

    json response;
    char local_buf[LOCAL_CHUNK_SIZE];

    if (offset == total_size) {
        try {
            std::ifstream in(temp, std::ios::binary);
            in.seekg(512, std::ios::beg);

            std::ofstream final_out(target, std::ios::binary | std::ios::trunc);
            if (!final_out) {
                send_json(socket, make_message(ErrorCode::InternalError, "Failed to create final file"));
                return;
            }

            while (in) {
                in.read(local_buf, sizeof(local_buf));
                std::streamsize n = in.gcount();
                if (n <= 0) break;

                final_out.write(local_buf, n);
            }

            final_out.close();
            in.close();
            fs::remove(temp);

            response = make_message(ErrorCode::Ok, "Upload complete");
        } catch (const std::exception& e) {
            send_json(socket, make_message(ErrorCode::InternalError,
                std::string("Finalize failed: ") + e.what()));
            return;
        }
    } else {
        response = make_message(ErrorCode::InternalError, "Upload interrupted",
            json{{"received", offset}});
    }

    if (!send_json(socket, response)) {
        throw std::runtime_error("client disconnected");
    }
}


void handle_download(tcp::socket& socket, const fs::path& user_root, const json& header) {
    const std::string local_path = header.value("path", "");
    std::uint64_t offset = header.value("offset", std::uint64_t{0});

    if (local_path.empty()) {
        send_json(socket, make_message(ErrorCode::InvalidCommand, "Path required"));
        return;
    }

    fs::path target = resolve_user_path(user_root, local_path);
    if (!fs::exists(target) || !fs::is_regular_file(target)) {
        send_json(socket, make_message(ErrorCode::NotFound,"File not found"));
        return;
    }

    auto size = fs::file_size(target);

    if (!send_json(socket, make_message(
        ErrorCode::Ok,"", json{{"size", size}}))) {
        throw std::runtime_error("client disconnected");
    }

    std::ifstream in(target, std::ios::binary);
    if (!in) {
        send_json(socket, make_message(ErrorCode::PermissionDenied, "Failed to open target file"));
        return;
    }

    in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);

    char buf[NET_CHUNK_SIZE];
    std::uint64_t sent = offset;

    while (sent < size) {
        in.read(buf, sizeof(buf));
        std::streamsize n = in.gcount();
        if (n <= 0) break;

        asio::error_code ec;
        asio::write(socket, asio::buffer(buf, static_cast<std::size_t>(n)), ec);
        if (ec) {
            g_log->error("Download write error: {}", ec.message());
            break;
        }
        sent += static_cast<std::uint64_t>(n);
    }
}


bool handle_auth(tcp::socket& socket, UserDb& db, const fs::path& root, std::string& out_username,
                        fs::path& out_user_root) {
    json first;
    if (!recv_json(socket, first)) {
        throw std::runtime_error("client disconnected");
    }

    const std::string username = first.value("username", "");
    if (username.empty()) {
        out_username  = "public";
        out_user_root = root / "public";
        try {
            fs::create_directories(out_user_root);
        } catch (const std::exception& e) {
            send_json(socket, make_message(ErrorCode::InternalError,e.what()));
            return false;
        }

        send_json(socket, make_message(ErrorCode::Ok, "", json{{"mode", "public"}}));
        return true;
    }

    try {
        if (db.has_user(username)) {
            send_json(socket, make_message(ErrorCode::Ok, "need_password"));

            json pw_json;
            if (!recv_json(socket, pw_json)) {
                throw std::runtime_error("client disconnected");
            }

            const std::string password = pw_json.value("password", "");
            UserCred user_entry = db.get_user(username);
            std::string derived = pbkdf2_sha256_hex(password, user_entry.salt_hex);\

            if (derived != user_entry.hash_hex) {
                send_json(socket, make_message(ErrorCode::AuthFailed,"Invalid password"));
                return false;
            }

            out_username  = username;
            out_user_root = root / "users" / username;

            try {
                fs::create_directories(out_user_root);
            } catch (const std::exception& e) {
                send_json(socket, make_message(ErrorCode::InternalError, e.what()));
                return false;
            }

            json data = json::object();
            data["mode"] = "private";
            data["incomplete_uploads"] = list_incomplete_transfers(out_user_root);

            send_json(socket, make_message(ErrorCode::Ok, "", data));
            return true;
        }

        send_json(socket, make_message(ErrorCode::Ok, "need_register"));
        json reg_json;
        if (!recv_json(socket, reg_json)) {
            throw std::runtime_error("client disconnected");
        }

        const std::string password = reg_json.value("password", "");
        if (password.empty()) {
            send_json(socket, make_message(ErrorCode::InvalidCommand,"Empty password"));
            return false;
        }

        db.add_user(username, password);
        out_username  = username;
        out_user_root = root / "users" / username;

        try {
            fs::create_directories(out_user_root);
        } catch (const std::exception& e) {
            send_json(socket, make_message(ErrorCode::InternalError, e.what()));
            return false;
        }

        send_json(socket, make_message(ErrorCode::Ok, "", json{{"mode", "registered"}}));
        return true;
    } catch (const std::exception& e) {
        send_json(socket, make_message(ErrorCode::InternalError,e.what()));
        return false;
    }
}



void handle_client(tcp::socket& socket, UserDb& user_db, const fs::path& root){
    std::string ip = socket.remote_endpoint().address().to_string();

    try {
        std::string username;
        fs::path user_root;

        if (!handle_auth(socket, user_db, root, username, user_root)) {
            socket.close();
            return;
        }

        g_log->info("Client connected: {}", ip);
        while (g_running) {
            json request;
            if (!recv_json(socket, request)) {
                g_log->info("Client disconnected: {}", ip);
                break;
            }

            const std::string cmd = request.value("cmd", "");

            if (cmd == "EXIT") {
                g_log->info("Client disconnected: {}", ip);
                break;
            }
            if (cmd == "UPLOAD") {
                handle_upload(socket, user_root, request);
                continue;
            }
            if (cmd == "DOWNLOAD") {
                handle_download(socket, user_root, request);
                continue;
            }

            json resp = dispatch_command(user_root, request);
            if (!send_json(socket, resp)) break;
        }

    } catch (const std::exception& e) {
        g_log->error("Client handler error at {}: {}", ip, e.what());
    }

    asio::error_code ec;
    socket.close(ec);
}


int main(int argc, char* argv[]){
    int port = -1;
    fs::path root;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        } else if (arg == "--root" && i + 1 < argc) {
            root = argv[++i];
        }
    }

    if (port <= 0 || root.empty()) {
        std::cout << "Usage: ./server --port <PORT> --root <ROOT_PATH>" << std::endl;
        return 1;
    }

    try {
        fs::create_directories(root / "public");
        fs::create_directories(root / "users");
    } catch (const std::exception& e) {
        std::cout << "Failed to create root dirs: " << e.what() << "\n";
        return 1;
    }

    std::signal(SIGTERM, handle_signal);
    std::signal(SIGINT,  handle_signal);

    UserDb user_db(root);
    g_log = spdlog::default_logger().get();
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S] [%l] %v");


    std::thread cleanup_thread{cleanup_thread_func, root};
    cleanup_thread.detach();


    try {
        g_acceptor.open(tcp::v4());
        g_acceptor.set_option(tcp::acceptor::reuse_address(true));
        g_acceptor.bind(tcp::endpoint(tcp::v4(), static_cast<unsigned short>(port)));
        g_acceptor.listen();

        std::cout << "Listening on port " << port << " root=\"" << root.string() << "\"\n";

        std::vector<std::thread> client_threads;

        while (g_running) {
            auto sock = std::make_shared<tcp::socket>(g_io);

            asio::error_code ec;
            g_acceptor.accept(*sock, ec);
            if (ec) {
                if (!g_running) {
                    break;
                }
                g_log->error("Accept error: {}", ec.message());
                continue;
            }

            {
                std::lock_guard lk(g_sockets_lock);
                g_sockets.push_back(sock);
            }

            client_threads.emplace_back([sock, &user_db, root]() {
                handle_client(*sock, user_db, root);

                std::lock_guard lk(g_sockets_lock);
                std::erase(g_sockets, sock);
            });
        }

    } catch (const std::exception& e) {
        g_log->error("Server error: {}", e.what());
        return 1;
    }

    return 0;
}


