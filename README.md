# MiniDrive

A C++20 client-server file storage application. The command-line client connects over TCP to upload, download and manage files on the server.

## Features

- Public storage and separate directories for registered users.
- User registration and login with salted PBKDF2-SHA256 password hashes.
- File uploads and downloads with support for resuming interrupted transfers.
- Directory listing, navigation, creation, deletion, moving and copying.
- Local-to-server directory synchronization using SHA256 file hashes.
- Multiple client connections handled by server threads.
- Optional client logging with spdlog.

Control messages use length-prefixed JSON. File contents are transferred in binary chunks. Common protocol and hashing functions are in `shared/`.

## Build

Requires a C++20 compiler, CMake 3.22+, Git and OpenSSL development libraries. CMake downloads the pinned Asio, nlohmann/json, spdlog and libsodium dependencies.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
```

## Run

Start the server with a port and storage directory:

```bash
./build/server/server --port 50000 --root ./storage
```

Connect from another terminal in public mode:

```bash
./build/client/client 127.0.0.1:50000
```

Or connect with a username:

```bash
./build/client/client demo@127.0.0.1:50000
```

For a new username, follow the registration prompt and then reconnect to log in. Add `--log client.log` to enable client logging. These executable paths are for a single-configuration Linux build.

## Client commands

```text
HELP
LIST [path]
CD <path>
MKDIR <path>
RMDIR <path>
UPLOAD <local_path> [remote_path]
DOWNLOAD <remote_path> [local_path]
DELETE <path>
MOVE <src> <dst>
COPY <src> <dst>
SYNC <local_dir> <remote_dir>
EXIT
```

`SYNC` mirrors the local directory to the server, including deleting remote files that are absent locally. Local file paths are relative to the client's working directory unless an absolute path is used.

## Structure

- `client/` - command-line interface and file transfers.
- `server/` - connections, user accounts and remote file operations.
- `shared/` - common messages, hashes and transfer helpers.
- `cmake/` - dependency configuration.
