#include "HttpServer.h"

#include <arpa/inet.h>
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <pty.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <unistd.h>
#include <fcntl.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" char** environ;

namespace {
std::string urlDecode(const std::string& input) {
    std::string output;
    output.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '%') {
            if (i + 2 < input.size()) {
                try {
                    const std::string hex = input.substr(i + 1, 2);
                    const unsigned char decoded = static_cast<unsigned char>(std::stoi(hex, nullptr, 16));
                    output.push_back(static_cast<char>(decoded));
                    i += 2;
                } catch (...) {
                    output.push_back(input[i]);
                }
            } else {
                output.push_back(input[i]);
            }
        } else if (input[i] == '+') {
            output.push_back(' ');
        } else {
            output.push_back(input[i]);
        }
    }
    return output;
}

std::string readFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

std::string jsonEscape(const std::string& input) {
    std::string out;
    for (char ch : input) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += ch; break;
        }
    }
    return out;
}

std::string buildTargetsJson(const std::vector<RemoteTarget>& targets) {
    std::ostringstream json;
    json << "{\"targets\":[";
    for (size_t i = 0; i < targets.size(); ++i) {
        const auto& t = targets[i];
        json << "{\"id\":" << t.id
               << ",\"name\":\"" << jsonEscape(t.name) << "\""
             << ",\"ip\":\"" << jsonEscape(t.ip) << "\""
             << ",\"username\":\"" << jsonEscape(t.username) << "\""
             << ",\"port\":" << t.port
             << ",\"services\":\"" << jsonEscape(t.services) << "\""
             << ",\"last_status\":\"" << jsonEscape(t.lastStatus) << "\""
             << ",\"last_checked_at\":\"" << jsonEscape(t.lastCheckedAt) << "\""
             << ",\"memory_available\":" << (t.memory.available ? "true" : "false")
             << ",\"memory_used_kb\":";
        if (t.memory.available) json << t.memory.usedKb;
        else json << "null";
        json << ",\"memory_total_kb\":";
        if (t.memory.available) json << t.memory.totalKb;
        else json << "null";
        json << ",\"memory_checked_at\":\"" << jsonEscape(t.memory.checkedAt) << "\"}";
        if (i + 1 < targets.size()) {
            json << ",";
        }
    }
    json << "]}";
    return json.str();
}

std::string buildChecksJson(const std::vector<ServiceCheckResult>& checks) {
    std::ostringstream json;
    json << "{\"checks\":[";
    for (size_t i = 0; i < checks.size(); ++i) {
        const auto& c = checks[i];
        json << "{\"id\":" << c.id
             << ",\"target_id\":" << c.targetId
               << ",\"name\":\"" << jsonEscape(c.name) << "\""
             << ",\"ip\":\"" << jsonEscape(c.ip) << "\""
             << ",\"username\":\"" << jsonEscape(c.username) << "\""
             << ",\"service_name\":\"" << jsonEscape(c.serviceName) << "\""
             << ",\"active\":" << (c.active ? "true" : "false")
             << ",\"status_text\":\"" << jsonEscape(c.statusText) << "\""
             << ",\"checked_at\":\"" << jsonEscape(c.checkedAt) << "\"" << "}";
        if (i + 1 < checks.size()) {
            json << ",";
        }
    }
    json << "]}";
    return json.str();
}

std::string mimeType(const std::string& path) {
    const std::string ext = std::filesystem::path(path).extension().string();
    if (ext == ".html") return "text/html; charset=utf-8";
    if (ext == ".css") return "text/css; charset=utf-8";
    if (ext == ".js") return "application/javascript; charset=utf-8";
    if (ext == ".json") return "application/json; charset=utf-8";
    if (ext == ".svg") return "image/svg+xml";
    return "application/octet-stream";
}

void sendResponse(int clientFd, const std::string& status, const std::string& contentType, const std::string& body) {
    std::ostringstream header;
    header << "HTTP/1.1 " << status << "\r\n"
           << "Content-Type: " << contentType << "\r\n"
           << "Content-Length: " << body.size() << "\r\n"
           << "Connection: close\r\n\r\n";

    const std::string headerText = header.str();
    const auto sendAll = [clientFd](const char* data, size_t size) {
        size_t sent = 0;
        while (sent < size) {
            const ssize_t result = send(clientFd, data + sent, size - sent, MSG_NOSIGNAL);
            if (result < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            if (result == 0) return false;
            sent += static_cast<size_t>(result);
        }
        return true;
    };

    if (sendAll(headerText.data(), headerText.size()) && !body.empty()) {
        sendAll(body.data(), body.size());
    }
}

void serveStaticFile(int clientFd, const std::string& rootDir, const std::string& path) {
    std::string requestPath = path;
    if (requestPath.empty() || requestPath == "/") {
        requestPath = "/index.html";
    }

    std::filesystem::path candidate(rootDir);
    candidate /= urlDecode(requestPath.substr(1));

    try {
        const auto canonicalRoot = std::filesystem::weakly_canonical(rootDir);
        const auto canonicalCandidate = std::filesystem::weakly_canonical(candidate);
        const std::string rootString = canonicalRoot.string();
        const std::string candidateString = canonicalCandidate.string();
        if (candidateString.rfind(rootString, 0) != 0) {
            sendResponse(clientFd, "403 Forbidden", "text/plain; charset=utf-8", "Forbidden");
            return;
        }

        const std::string content = readFile(candidateString);
        if (content.empty()) {
            sendResponse(clientFd, "404 Not Found", "text/plain; charset=utf-8", "Not Found");
            return;
        }

        sendResponse(clientFd, "200 OK", mimeType(candidateString), content);
    } catch (const std::exception&) {
        sendResponse(clientFd, "404 Not Found", "text/plain; charset=utf-8", "Not Found");
    }
}

std::string readBody(const std::string& request) {
    const std::string marker = "\r\n\r\n";
    const auto pos = request.find(marker);
    if (pos == std::string::npos) {
        return {};
    }
    return request.substr(pos + marker.size());
}

std::unordered_map<std::string, std::string> parseForm(const std::string& body) {
    std::unordered_map<std::string, std::string> fields;
    std::string token;
    std::istringstream bodyStream(body);
    while (std::getline(bodyStream, token, '&')) {
        const auto eq = token.find('=');
        if (eq != std::string::npos) {
            fields[urlDecode(token.substr(0, eq))] = urlDecode(token.substr(eq + 1));
        }
    }
    return fields;
}

bool parseTargetRoute(const std::string& path, int& targetId, std::string& resource) {
    const std::string prefix = "/api/targets/";
    if (path.rfind(prefix, 0) != 0) {
        return false;
    }

    const std::string remainder = path.substr(prefix.size());
    const auto slash = remainder.find('/');
    const std::string idText = remainder.substr(0, slash);
    try {
        size_t parsed = 0;
        targetId = std::stoi(idText, &parsed);
        if (parsed != idText.size() || targetId <= 0) {
            return false;
        }
    } catch (...) {
        return false;
    }
    resource = slash == std::string::npos ? "" : remainder.substr(slash + 1);
    return true;
}

std::string shellQuote(const std::string& value) {
    std::string quoted = "'";
    for (char character : value) {
        if (character == '\'') quoted += "'\\''";
        else quoted += character;
    }
    quoted += "'";
    return quoted;
}

bool targetHasService(const RemoteTarget& target, const std::string& serviceName) {
    if (serviceName.empty() || !std::isalnum(static_cast<unsigned char>(serviceName.front()))) return false;
    for (unsigned char character : serviceName) {
        if (!std::isalnum(character) && character != '_' && character != '-' && character != '.' && character != '@' && character != ':') return false;
    }

    std::istringstream services(target.services);
    std::string service;
    while (std::getline(services, service, ',')) {
        const auto start = service.find_first_not_of(" \t");
        const auto end = service.find_last_not_of(" \t");
        if (start != std::string::npos && service.substr(start, end - start + 1) == serviceName) return true;
    }
    return false;
}

int runRemoteAction(const RemoteTarget& target, const std::string& command, const std::string& sshPassword, const std::string& sudoPassword, std::string& output) {
    int outputPipe[2];
    int inputSocket[2];
    if (pipe(outputPipe) != 0) return -1;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, inputSocket) != 0) {
        close(outputPipe[0]);
        close(outputPipe[1]);
        return -1;
    }

    std::vector<std::string> arguments = {
        "ssh", "-o", sshPassword.empty() ? "BatchMode=yes" : "BatchMode=no",
        "-o", "StrictHostKeyChecking=accept-new", "-o", "ConnectTimeout=8",
        "-o", "NumberOfPasswordPrompts=1"
    };
    if (!sshPassword.empty()) arguments.insert(arguments.end(), {"-o", "PreferredAuthentications=publickey,password,keyboard-interactive"});
    arguments.insert(arguments.end(), {"-p", std::to_string(target.port), "-l", target.username, "--", target.ip, command});

    std::vector<char*> argumentPointers;
    for (auto& argument : arguments) argumentPointers.push_back(argument.data());
    argumentPointers.push_back(nullptr);

    std::vector<std::string> environment;
    for (char** current = environ; current && *current; ++current) {
        const std::string variable(*current);
        if (variable.rfind("SSH_ASKPASS=", 0) == 0 || variable.rfind("SSH_ASKPASS_REQUIRE=", 0) == 0 ||
            variable.rfind("DISPLAY=", 0) == 0 || variable.rfind("MONITORING_SSH_PASSWORD=", 0) == 0) continue;
        environment.push_back(variable);
    }
    if (!sshPassword.empty()) {
        environment.emplace_back("SSH_ASKPASS=/opt/monitoring-linux/monitoring-linux-ssh-askpass");
        environment.emplace_back("SSH_ASKPASS_REQUIRE=force");
        environment.emplace_back("DISPLAY=monitoring-linux:0");
        environment.emplace_back("MONITORING_SSH_PASSWORD=" + sshPassword);
    }
    std::vector<char*> environmentPointers;
    for (auto& variable : environment) environmentPointers.push_back(variable.data());
    environmentPointers.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        close(outputPipe[0]);
        close(outputPipe[1]);
        return -1;
    }
    const int actionResult = posix_spawn_file_actions_adddup2(&actions, outputPipe[1], STDOUT_FILENO) ||
                             posix_spawn_file_actions_adddup2(&actions, outputPipe[1], STDERR_FILENO) ||
                             posix_spawn_file_actions_addclose(&actions, outputPipe[0]) ||
                             posix_spawn_file_actions_addclose(&actions, outputPipe[1]) ||
                             posix_spawn_file_actions_adddup2(&actions, inputSocket[1], STDIN_FILENO) ||
                             posix_spawn_file_actions_addclose(&actions, inputSocket[0]) ||
                             posix_spawn_file_actions_addclose(&actions, inputSocket[1]);
    if (actionResult != 0) {
        posix_spawn_file_actions_destroy(&actions);
        close(outputPipe[0]);
        close(outputPipe[1]);
        close(inputSocket[0]);
        close(inputSocket[1]);
        return -1;
    }

    pid_t child = -1;
    const int spawnResult = posix_spawnp(&child, "ssh", &actions, nullptr, argumentPointers.data(), environmentPointers.data());
    posix_spawn_file_actions_destroy(&actions);
    close(outputPipe[1]);
    close(inputSocket[1]);
    if (spawnResult != 0) {
        close(outputPipe[0]);
        close(inputSocket[0]);
        return -1;
    }

    std::string sudoInput = sudoPassword.empty() ? std::string{} : sudoPassword + "\n";
    size_t sent = 0;
    while (sent < sudoInput.size()) {
        const ssize_t count = send(inputSocket[0], sudoInput.data() + sent, sudoInput.size() - sent, MSG_NOSIGNAL);
        if (count < 0) {
            if (errno == EINTR) continue;
            kill(child, SIGTERM);
            waitpid(child, nullptr, 0);
            close(inputSocket[0]);
            close(outputPipe[0]);
            return -1;
        }
        if (count == 0) {
            kill(child, SIGTERM);
            waitpid(child, nullptr, 0);
            close(inputSocket[0]);
            close(outputPipe[0]);
            return -1;
        }
        sent += static_cast<size_t>(count);
    }
    volatile char* sudoInputData = sudoInput.empty() ? nullptr : sudoInput.data();
    for (size_t index = 0; index < sudoInput.size(); ++index) sudoInputData[index] = '\0';
    shutdown(inputSocket[0], SHUT_WR);
    close(inputSocket[0]);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    bool pipeOpen = true;
    bool childExited = false;
    int childStatus = 0;
    while (pipeOpen || !childExited) {
        if (std::chrono::steady_clock::now() >= deadline) {
            kill(child, SIGTERM);
            waitpid(child, &childStatus, 0);
            close(outputPipe[0]);
            return 124;
        }

        pollfd descriptor{outputPipe[0], static_cast<short>(pipeOpen ? POLLIN : 0), 0};
        poll(&descriptor, 1, 100);
        if (pipeOpen && descriptor.revents & (POLLIN | POLLHUP | POLLERR)) {
            char buffer[4096];
            const ssize_t count = read(outputPipe[0], buffer, sizeof(buffer));
            if (count > 0) output.append(buffer, static_cast<size_t>(count));
            else pipeOpen = false;
        }
        if (!childExited && waitpid(child, &childStatus, WNOHANG) == child) childExited = true;
    }
    close(outputPipe[0]);
    return WIFEXITED(childStatus) ? WEXITSTATUS(childStatus) : 255;
}

std::string requestHeader(const std::string& request, const std::string& wantedName) {
    const auto headerEnd = request.find("\r\n\r\n");
    if (headerEnd == std::string::npos) return {};
    const std::string wanted = [&] {
        std::string name = wantedName;
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return name;
    }();
    std::istringstream headers(request.substr(0, headerEnd));
    std::string line;
    std::getline(headers, line);
    while (std::getline(headers, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string name = line.substr(0, colon);
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (name == wanted) {
            const auto valueStart = line.find_first_not_of(" \t", colon + 1);
            return valueStart == std::string::npos ? std::string{} : line.substr(valueStart);
        }
    }
    return {};
}

uint32_t rotateLeft(uint32_t value, unsigned shift) {
    return (value << shift) | (value >> (32 - shift));
}

std::array<unsigned char, 20> sha1(const std::string& value) {
    std::vector<unsigned char> bytes(value.begin(), value.end());
    const uint64_t bitLength = static_cast<uint64_t>(bytes.size()) * 8;
    bytes.push_back(0x80);
    while ((bytes.size() % 64) != 56) bytes.push_back(0);
    for (int shift = 56; shift >= 0; shift -= 8) bytes.push_back(static_cast<unsigned char>(bitLength >> shift));

    uint32_t h0 = 0x67452301;
    uint32_t h1 = 0xEFCDAB89;
    uint32_t h2 = 0x98BADCFE;
    uint32_t h3 = 0x10325476;
    uint32_t h4 = 0xC3D2E1F0;
    for (size_t block = 0; block < bytes.size(); block += 64) {
        uint32_t words[80]{};
        for (size_t i = 0; i < 16; ++i) {
            const size_t offset = block + i * 4;
            words[i] = (static_cast<uint32_t>(bytes[offset]) << 24) |
                       (static_cast<uint32_t>(bytes[offset + 1]) << 16) |
                       (static_cast<uint32_t>(bytes[offset + 2]) << 8) |
                       static_cast<uint32_t>(bytes[offset + 3]);
        }
        for (size_t i = 16; i < 80; ++i) words[i] = rotateLeft(words[i - 3] ^ words[i - 8] ^ words[i - 14] ^ words[i - 16], 1);
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (size_t i = 0; i < 80; ++i) {
            uint32_t f = 0, k = 0;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            const uint32_t temp = rotateLeft(a, 5) + f + e + k + words[i];
            e = d; d = c; c = rotateLeft(b, 30); b = a; a = temp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }

    std::array<unsigned char, 20> digest{};
    const uint32_t hashWords[] = {h0, h1, h2, h3, h4};
    for (size_t i = 0; i < 5; ++i) {
        digest[i * 4] = static_cast<unsigned char>(hashWords[i] >> 24);
        digest[i * 4 + 1] = static_cast<unsigned char>(hashWords[i] >> 16);
        digest[i * 4 + 2] = static_cast<unsigned char>(hashWords[i] >> 8);
        digest[i * 4 + 3] = static_cast<unsigned char>(hashWords[i]);
    }
    return digest;
}

std::string base64(const unsigned char* data, size_t size) {
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(((size + 2) / 3) * 4);
    for (size_t i = 0; i < size; i += 3) {
        const uint32_t value = (static_cast<uint32_t>(data[i]) << 16) |
                               (i + 1 < size ? static_cast<uint32_t>(data[i + 1]) << 8 : 0) |
                               (i + 2 < size ? static_cast<uint32_t>(data[i + 2]) : 0);
        result.push_back(alphabet[(value >> 18) & 0x3f]);
        result.push_back(alphabet[(value >> 12) & 0x3f]);
        result.push_back(i + 1 < size ? alphabet[(value >> 6) & 0x3f] : '=');
        result.push_back(i + 2 < size ? alphabet[value & 0x3f] : '=');
    }
    return result;
}

bool sendSocketData(int fd, const unsigned char* data, size_t size) {
    size_t sent = 0;
    while (sent < size) {
        const ssize_t result = send(fd, data + sent, size - sent, MSG_NOSIGNAL);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) return false;
        sent += static_cast<size_t>(result);
    }
    return true;
}

bool sendWebSocketFrame(int fd, unsigned char opcode, const unsigned char* payload, size_t size) {
    std::vector<unsigned char> frame;
    frame.reserve(size + 10);
    frame.push_back(static_cast<unsigned char>(0x80 | opcode));
    if (size <= 125) {
        frame.push_back(static_cast<unsigned char>(size));
    } else if (size <= 65535) {
        frame.push_back(126);
        frame.push_back(static_cast<unsigned char>(size >> 8));
        frame.push_back(static_cast<unsigned char>(size));
    } else {
        frame.push_back(127);
        for (int shift = 56; shift >= 0; shift -= 8) frame.push_back(static_cast<unsigned char>(static_cast<uint64_t>(size) >> shift));
    }
    if (size > 0) frame.insert(frame.end(), payload, payload + size);
    return sendSocketData(fd, frame.data(), frame.size());
}

bool readSocketBytes(int fd, unsigned char* data, size_t size) {
    size_t received = 0;
    while (received < size) {
        const ssize_t result = recv(fd, data + received, size - received, 0);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) return false;
        received += static_cast<size_t>(result);
    }
    return true;
}

void runTerminalSocket(int clientFd, const std::string& request, const RemoteTarget& target, const std::string& password, const std::atomic<bool>& serverRunning) {
    const std::string clientKey = requestHeader(request, "Sec-WebSocket-Key");
    const std::string host = requestHeader(request, "Host");
    const std::string origin = requestHeader(request, "Origin");
    std::string upgrade = requestHeader(request, "Upgrade");
    std::string connection = requestHeader(request, "Connection");
    std::transform(upgrade.begin(), upgrade.end(), upgrade.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(connection.begin(), connection.end(), connection.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const bool localHost = host.rfind("localhost:", 0) == 0 || host.rfind("127.0.0.1:", 0) == 0;
    const bool localOrigin = origin.empty() || origin == "http://" + host;
    if (upgrade != "websocket" || connection.find("upgrade") == std::string::npos ||
        requestHeader(request, "Sec-WebSocket-Version") != "13" || clientKey.empty() || !localHost || !localOrigin) {
        sendResponse(clientFd, "400 Bad Request", "text/plain; charset=utf-8", "WebSocket upgrade required");
        close(clientFd);
        return;
    }

    winsize terminalSize{};
    terminalSize.ws_col = 100;
    terminalSize.ws_row = 30;

    std::vector<std::string> arguments = {
        "ssh", "-tt", "-o", "BatchMode=no",
        "-o", "StrictHostKeyChecking=accept-new", "-o", "ConnectTimeout=8", "-o", "NumberOfPasswordPrompts=1"
    };
    if (!password.empty()) arguments.insert(arguments.end(), {"-o", "PreferredAuthentications=publickey,password,keyboard-interactive"});
    arguments.insert(arguments.end(), {"-p", std::to_string(target.port), "-l", target.username, "--", target.ip});
    std::vector<char*> argumentPointers;
    for (auto& argument : arguments) argumentPointers.push_back(argument.data());
    argumentPointers.push_back(nullptr);

    std::vector<std::string> environment;
    for (char** current = environ; current && *current; ++current) {
        const std::string variable(*current);
        if (variable.rfind("SSH_ASKPASS=", 0) == 0 || variable.rfind("SSH_ASKPASS_REQUIRE=", 0) == 0 ||
            variable.rfind("DISPLAY=", 0) == 0 || variable.rfind("MONITORING_SSH_PASSWORD=", 0) == 0 ||
            variable.rfind("TERM=", 0) == 0) continue;
        environment.push_back(variable);
    }
    environment.emplace_back("TERM=xterm-256color");
    if (!password.empty()) {
        environment.emplace_back("SSH_ASKPASS=/opt/monitoring-linux/monitoring-linux-ssh-askpass");
        environment.emplace_back("SSH_ASKPASS_REQUIRE=force");
        environment.emplace_back("DISPLAY=monitoring-linux:0");
        environment.emplace_back("MONITORING_SSH_PASSWORD=" + password);
    }
    std::vector<char*> environmentPointers;
    for (auto& variable : environment) environmentPointers.push_back(variable.data());
    environmentPointers.push_back(nullptr);

    int terminalFd = -1;
    const pid_t child = forkpty(&terminalFd, nullptr, nullptr, &terminalSize);
    if (child == 0) {
        close(clientFd);
        execve("/usr/bin/ssh", argumentPointers.data(), environmentPointers.data());
        _exit(127);
    }
    if (child < 0) {
        sendResponse(clientFd, "500 Internal Server Error", "text/plain; charset=utf-8", "Unable to create SSH terminal");
        close(clientFd);
        return;
    }

    const auto digest = sha1(clientKey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    const std::string handshake = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + base64(digest.data(), digest.size()) + "\r\n\r\n";
    if (!sendSocketData(clientFd, reinterpret_cast<const unsigned char*>(handshake.data()), handshake.size())) {
        kill(child, SIGHUP);
        close(terminalFd);
        close(clientFd);
        waitpid(child, nullptr, 0);
        return;
    }

    bool connected = true;
    bool childExited = false;
    std::vector<unsigned char> pending;
    while (connected && serverRunning.load()) {
        pollfd descriptors[2] = {{clientFd, POLLIN, 0}, {terminalFd, POLLIN, 0}};
        const int ready = poll(descriptors, 2, 500);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (descriptors[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            unsigned char buffer[8192];
            const ssize_t count = read(terminalFd, buffer, sizeof(buffer));
            if (count > 0) connected = sendWebSocketFrame(clientFd, 1, buffer, static_cast<size_t>(count));
            else connected = false;
        }

        if (connected && (descriptors[0].revents & (POLLIN | POLLHUP | POLLERR))) {
            unsigned char buffer[8192];
            const ssize_t count = recv(clientFd, buffer, sizeof(buffer), 0);
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                continue;
            }
            if (count <= 0) {
                connected = false;
            } else {
                pending.insert(pending.end(), buffer, buffer + count);
                size_t consumed = 0;
                while (pending.size() - consumed >= 2) {
                    const unsigned char first = pending[consumed];
                    const unsigned char second = pending[consumed + 1];
                    const unsigned char opcode = first & 0x0f;
                    const bool masked = (second & 0x80) != 0;
                    uint64_t payloadSize = second & 0x7f;
                    size_t headerSize = 2;
                    if (payloadSize == 126) {
                        if (pending.size() - consumed < 4) break;
                        payloadSize = (static_cast<uint64_t>(pending[consumed + 2]) << 8) | pending[consumed + 3];
                        headerSize = 4;
                    } else if (payloadSize == 127) {
                        if (pending.size() - consumed < 10) break;
                        payloadSize = 0;
                        for (size_t i = 0; i < 8; ++i) payloadSize = (payloadSize << 8) | pending[consumed + 2 + i];
                        headerSize = 10;
                    }
                    if (!masked || payloadSize > 65536) { connected = false; break; }
                    if (pending.size() - consumed < headerSize + 4 + payloadSize) break;
                    const unsigned char* mask = pending.data() + consumed + headerSize;
                    std::string payload(static_cast<size_t>(payloadSize), '\0');
                    for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>(pending[consumed + headerSize + 4 + i] ^ mask[i % 4]);
                    if (opcode == 8) {
                        sendWebSocketFrame(clientFd, 8, nullptr, 0);
                        connected = false;
                    } else if (opcode == 9) {
                        connected = sendWebSocketFrame(clientFd, 10, reinterpret_cast<const unsigned char*>(payload.data()), payload.size());
                    } else if (opcode == 1) {
                        int columns = 0, rows = 0;
                        if (std::sscanf(payload.c_str(), "{\"type\":\"resize\",\"cols\":%d,\"rows\":%d}", &columns, &rows) == 2) {
                            terminalSize.ws_col = static_cast<unsigned short>(std::clamp(columns, 20, 300));
                            terminalSize.ws_row = static_cast<unsigned short>(std::clamp(rows, 5, 120));
                            ioctl(terminalFd, TIOCSWINSZ, &terminalSize);
                            kill(child, SIGWINCH);
                        } else if (!payload.empty()) {
                            size_t written = 0;
                            while (written < payload.size()) {
                                const ssize_t result = write(terminalFd, payload.data() + written, payload.size() - written);
                                if (result < 0 && errno == EINTR) continue;
                                if (result <= 0) { connected = false; break; }
                                written += static_cast<size_t>(result);
                            }
                        }
                    }
                    consumed += headerSize + 4 + static_cast<size_t>(payloadSize);
                    if (!connected) break;
                }
                if (consumed > 0) pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(consumed));
            }
        }

        int childStatus = 0;
        if (waitpid(child, &childStatus, WNOHANG) == child) {
            childExited = true;
            connected = false;
        }
    }

    if (!childExited) kill(child, SIGHUP);
    close(terminalFd);
    close(clientFd);
    int childStatus = 0;
    if (!childExited) {
        while (waitpid(child, &childStatus, 0) < 0 && errno == EINTR) {}
    }
}

void handleRequest(int clientFd, Database& db, SshCredentials& credentials, MonitorSignal& monitorSignal, const std::string& webRoot, const std::atomic<bool>& serverRunning) {
    constexpr size_t maxRequestSize = 65536;
    std::string request;
    request.reserve(4096);
    size_t expectedSize = 0;
    char buffer[4096];
    while (request.size() < maxRequestSize) {
        const ssize_t received = recv(clientFd, buffer, sizeof(buffer), 0);
        if (received < 0 && errno == EINTR) continue;
        if (received <= 0) {
            close(clientFd);
            return;
        }
        request.append(buffer, static_cast<size_t>(received));

        const auto headerEnd = request.find("\r\n\r\n");
        if (headerEnd != std::string::npos && expectedSize == 0) {
            size_t contentLength = 0;
            std::istringstream headers(request.substr(0, headerEnd));
            std::string headerLine;
            std::getline(headers, headerLine);
            while (std::getline(headers, headerLine)) {
                if (!headerLine.empty() && headerLine.back() == '\r') headerLine.pop_back();
                const auto colon = headerLine.find(':');
                if (colon == std::string::npos) continue;
                std::string headerName = headerLine.substr(0, colon);
                std::transform(headerName.begin(), headerName.end(), headerName.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (headerName == "content-length") {
                    try {
                        contentLength = static_cast<size_t>(std::stoul(headerLine.substr(colon + 1)));
                    } catch (...) {
                        sendResponse(clientFd, "400 Bad Request", "text/plain; charset=utf-8", "Invalid Content-Length");
                        close(clientFd);
                        return;
                    }
                }
            }
            expectedSize = headerEnd + 4 + contentLength;
            if (expectedSize > maxRequestSize) {
                sendResponse(clientFd, "413 Payload Too Large", "text/plain; charset=utf-8", "Request too large");
                close(clientFd);
                return;
            }
        }
        if (expectedSize > 0 && request.size() >= expectedSize) break;
    }

    if (expectedSize == 0 || request.size() < expectedSize) {
        sendResponse(clientFd, "400 Bad Request", "text/plain; charset=utf-8", "Incomplete request");
        close(clientFd);
        return;
    }

    const auto lineEnd = request.find("\r\n");
    if (lineEnd == std::string::npos) {
        close(clientFd);
        return;
    }

    const std::string firstLine = request.substr(0, lineEnd);
    std::istringstream iss(firstLine);
    std::string method, path, version;
    iss >> method >> path >> version;

    if (method == "GET" && path == "/api/targets") {
        const auto targets = db.listTargets();
        const std::string json = buildTargetsJson(targets);
        sendResponse(clientFd, "200 OK", "application/json; charset=utf-8", json);
        close(clientFd);
        return;
    }

    if (method == "GET" && path == "/api/checks") {
        const auto checks = db.getLatestChecks();
        const std::string json = buildChecksJson(checks);
        sendResponse(clientFd, "200 OK", "application/json; charset=utf-8", json);
        close(clientFd);
        return;
    }

    if (method == "GET" && path == "/api/health") {
        sendResponse(clientFd, "200 OK", "application/json; charset=utf-8", "{\"status\":\"ok\"}");
        close(clientFd);
        return;
    }

    if (method == "POST" && path == "/api/targets") {
        const auto fields = parseForm(readBody(request));
        const std::string name = fields.count("name") ? fields.at("name") : "";
        const std::string ip = fields.count("ip") ? fields.at("ip") : "";
        const std::string username = fields.count("username") ? fields.at("username") : "root";
        const std::string services = fields.count("services") ? fields.at("services") : "";
        const std::string password = fields.count("password") ? fields.at("password") : "";
        const std::string sudoPassword = fields.count("sudo_password") ? fields.at("sudo_password") : "";
        int port = 22;
        try {
            if (fields.count("port")) port = std::stoi(fields.at("port"));
        } catch (...) {
            port = 0;
        }

        if (ip.empty() || name.size() > 80 || services.empty() || port < 1 || port > 65535) {
            sendResponse(clientFd, "400 Bad Request", "application/json; charset=utf-8", "{\"status\":\"invalid_input\"}");
            close(clientFd);
            return;
        }

        const std::string effectiveUsername = username.empty() ? "root" : username;
        int previousTargetId = 0;
        bool connectionChanged = false;
        for (const auto& target : db.listTargets()) {
            if (target.ip == ip) {
                previousTargetId = target.id;
                connectionChanged = target.username != effectiveUsername || target.port != port;
                break;
            }
        }
        const bool ok = db.upsertTarget(name, ip, effectiveUsername, port, services);
        if (ok) {
            int targetId = previousTargetId;
            if (targetId == 0) {
                for (const auto& target : db.listTargets()) {
                    if (target.ip == ip) {
                        targetId = target.id;
                        break;
                    }
                }
            }
            if (previousTargetId > 0 && connectionChanged) credentials.erase(previousTargetId);
            if (targetId > 0 && !password.empty()) credentials.setPassword(targetId, password);
            if (targetId > 0 && !sudoPassword.empty()) credentials.setSudoPassword(targetId, sudoPassword);
            monitorSignal.notify();
        }
        if (!ok) {
            sendResponse(clientFd, "500 Internal Server Error", "application/json; charset=utf-8", "{\"status\":\"error\"}");
            close(clientFd);
            return;
        }

        sendResponse(clientFd, "200 OK", "application/json; charset=utf-8", "{\"status\":\"ok\"}");
        close(clientFd);
        return;
    }

    int targetId = 0;
    std::string resource;
    if (parseTargetRoute(path, targetId, resource)) {
        if (method == "GET" && resource == "terminal") {
            const auto targets = db.listTargets();
            const auto found = std::find_if(targets.begin(), targets.end(), [targetId](const RemoteTarget& target) { return target.id == targetId; });
            if (found == targets.end()) {
                sendResponse(clientFd, "404 Not Found", "text/plain; charset=utf-8", "Machine not found");
                close(clientFd);
                return;
            }
            const RemoteTarget target = *found;
            const std::string password = credentials.passwordFor(targetId);
            runTerminalSocket(clientFd, request, target, password, serverRunning);
            return;
        }

        if (method == "POST" && resource == "actions") {
            const auto fields = parseForm(readBody(request));
            const std::string action = fields.count("action") ? fields.at("action") : "";
            const std::string service = fields.count("service") ? fields.at("service") : "";
            const auto targets = db.listTargets();
            const auto found = std::find_if(targets.begin(), targets.end(), [targetId](const RemoteTarget& target) { return target.id == targetId; });
            if (found == targets.end()) {
                sendResponse(clientFd, "404 Not Found", "application/json; charset=utf-8", "{\"status\":\"machine_not_found\"}");
                close(clientFd);
                return;
            }

            const std::string sudoPassword = credentials.sudoPasswordFor(targetId);
            std::string remoteCommand;
            if (action == "restart-all-services" || action == "stop-all-services") {
                std::istringstream configuredServices(found->services);
                std::string serviceName;
                std::vector<std::string> serviceArguments;
                while (std::getline(configuredServices, serviceName, ',')) {
                    const auto start = serviceName.find_first_not_of(" \t");
                    const auto end = serviceName.find_last_not_of(" \t");
                    if (start == std::string::npos) continue;
                    serviceName = serviceName.substr(start, end - start + 1);
                    if (!targetHasService(*found, serviceName)) {
                        sendResponse(clientFd, "400 Bad Request", "application/json; charset=utf-8", "{\"status\":\"invalid_configured_service\"}");
                        close(clientFd);
                        return;
                    }
                    serviceArguments.push_back(shellQuote(serviceName));
                }
                if (serviceArguments.empty()) {
                    sendResponse(clientFd, "400 Bad Request", "application/json; charset=utf-8", "{\"status\":\"no_services_configured\"}");
                    close(clientFd);
                    return;
                }
                const std::string operation = action == "restart-all-services" ? "restart" : "stop";
                remoteCommand = std::string(sudoPassword.empty() ? "sudo -n -- systemctl " : "sudo -S -p '' -- systemctl ") +
                                operation + " --no-block ";
                for (size_t index = 0; index < serviceArguments.size(); ++index) {
                    if (index > 0) remoteCommand += " ";
                    remoteCommand += serviceArguments[index];
                }
            } else if (action == "restart-service" || action == "stop-service") {
                if (!targetHasService(*found, service)) {
                    sendResponse(clientFd, "400 Bad Request", "application/json; charset=utf-8", "{\"status\":\"service_not_configured\"}");
                    close(clientFd);
                    return;
                }
                const std::string operation = action == "restart-service" ? "restart" : "stop";
                remoteCommand = std::string(sudoPassword.empty() ? "sudo -n -- systemctl " : "sudo -S -p '' -- systemctl ") +
                                operation + " " + shellQuote(service);
            } else if (action == "reboot") {
                remoteCommand = sudoPassword.empty() ? "sudo -n -- systemctl reboot --no-block" :
                                                       "sudo -S -p '' -- systemctl reboot --no-block";
            } else if (action == "poweroff") {
                remoteCommand = sudoPassword.empty() ? "sudo -n -- systemctl poweroff --no-block" :
                                                       "sudo -S -p '' -- systemctl poweroff --no-block";
            } else {
                sendResponse(clientFd, "400 Bad Request", "application/json; charset=utf-8", "{\"status\":\"invalid_action\"}");
                close(clientFd);
                return;
            }

            const RemoteTarget target = *found;
            const std::string password = credentials.passwordFor(targetId);
            std::string output;
            const int exitCode = runRemoteAction(target, remoteCommand, password, sudoPassword, output);
            const bool ok = exitCode == 0;
            if (ok && (action == "restart-service" || action == "stop-service" ||
                       action == "restart-all-services" || action == "stop-all-services")) monitorSignal.notify();
            std::ostringstream response;
            response << "{\"status\":\"" << (ok ? "completed" : (exitCode == 124 ? "timeout" : "failed"))
                     << "\",\"exit_code\":" << exitCode << ",\"output\":\"" << jsonEscape(output) << "\"}";
            sendResponse(clientFd, ok ? "200 OK" : "502 Bad Gateway", "application/json; charset=utf-8", response.str());
            close(clientFd);
            return;
        }

        if (method == "PUT" && resource.empty()) {
            const auto fields = parseForm(readBody(request));
            const std::string name = fields.count("name") ? fields.at("name") : "";
            const std::string ip = fields.count("ip") ? fields.at("ip") : "";
            const std::string username = fields.count("username") ? fields.at("username") : "root";
            const std::string services = fields.count("services") ? fields.at("services") : "";
            const std::string password = fields.count("password") ? fields.at("password") : "";
            const std::string sudoPassword = fields.count("sudo_password") ? fields.at("sudo_password") : "";
            int port = 22;
            try {
                if (fields.count("port")) port = std::stoi(fields.at("port"));
            } catch (...) {
                port = 0;
            }
            const std::string effectiveUsername = username.empty() ? "root" : username;
            const bool valid = name.size() <= 80 && !ip.empty() && !services.empty() && port >= 1 && port <= 65535;
            bool connectionChanged = false;
            for (const auto& target : db.listTargets()) {
                if (target.id == targetId) {
                    connectionChanged = target.ip != ip || target.username != effectiveUsername || target.port != port;
                    break;
                }
            }
            const bool ok = valid && db.updateTarget(targetId, name, ip, effectiveUsername, port, services);
            if (ok) {
                if (connectionChanged) credentials.erase(targetId);
                if (!password.empty()) credentials.setPassword(targetId, password);
                if (!sudoPassword.empty()) credentials.setSudoPassword(targetId, sudoPassword);
                monitorSignal.notify();
            }
            sendResponse(clientFd, ok ? "200 OK" : (valid ? "404 Not Found" : "400 Bad Request"), "application/json; charset=utf-8", ok ? "{\"status\":\"updated\"}" : "{\"status\":\"error\"}");
            close(clientFd);
            return;
        }

        if (method == "POST" && resource == "password") {
            const auto targets = db.listTargets();
            const bool exists = std::any_of(targets.begin(), targets.end(), [targetId](const RemoteTarget& target) { return target.id == targetId; });
            const auto fields = parseForm(readBody(request));
            const auto password = fields.find("password");
            const bool ok = exists && password != fields.end() && !password->second.empty();
            if (ok) {
                credentials.setPassword(targetId, password->second);
                monitorSignal.notify();
            }
            sendResponse(clientFd, ok ? "200 OK" : "400 Bad Request", "application/json; charset=utf-8", ok ? "{\"status\":\"password_set\"}" : "{\"status\":\"invalid_password\"}");
            close(clientFd);
            return;
        }

        if (method == "DELETE" && resource.empty()) {
            const bool ok = db.deleteTarget(targetId);
            if (ok) {
                credentials.erase(targetId);
                monitorSignal.notify();
            }
            sendResponse(clientFd, ok ? "200 OK" : "404 Not Found", "application/json; charset=utf-8", ok ? "{\"status\":\"deleted\"}" : "{\"status\":\"not_found\"}");
            close(clientFd);
            return;
        }
    }

    serveStaticFile(clientFd, webRoot, path);
    close(clientFd);
}
}  // namespace

HttpServer::HttpServer(Database& db, SshCredentials& credentials, MonitorSignal& monitorSignal, const std::string& host, int port, const std::string& webRoot)
    : db_(db), credentials_(credentials), monitorSignal_(monitorSignal), host_(host), port_(port), webRoot_(webRoot) {
}

HttpServer::~HttpServer() {
    stop();
}

void HttpServer::stop() {
    running_ = false;
    if (socketFd_ >= 0) {
        shutdown(socketFd_, SHUT_RDWR);
        close(socketFd_);
        socketFd_ = -1;
    }

    {
        std::lock_guard<std::mutex> lock(clientsMutex_);
        stoppingWorkers_ = true;
        while (!pendingClients_.empty()) {
            close(pendingClients_.front());
            pendingClients_.pop_front();
        }
    }
    clientsReady_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) worker.join();
    }
    workers_.clear();
}

void HttpServer::workerLoop() {
    while (true) {
        int clientFd = -1;
        {
            std::unique_lock<std::mutex> lock(clientsMutex_);
            clientsReady_.wait(lock, [this] { return stoppingWorkers_ || !pendingClients_.empty(); });
            if (stoppingWorkers_ && pendingClients_.empty()) return;
            clientFd = pendingClients_.front();
            pendingClients_.pop_front();
        }
        handleRequest(clientFd, db_, credentials_, monitorSignal_, webRoot_, running_);
    }
}

void HttpServer::run() {
    socketFd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (socketFd_ < 0) {
        throw std::runtime_error("Unable to create HTTP socket");
    }

    int reuse = 1;
    setsockopt(socketFd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port_));
    if (inet_pton(AF_INET, host_.c_str(), &address.sin_addr) <= 0) {
        close(socketFd_);
        socketFd_ = -1;
        throw std::runtime_error("Invalid bind address: " + host_);
    }

    if (bind(socketFd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        close(socketFd_);
        socketFd_ = -1;
        throw std::runtime_error("Unable to bind HTTP server to " + host_ + ":" + std::to_string(port_));
    }

    if (listen(socketFd_, 8) < 0) {
        close(socketFd_);
        socketFd_ = -1;
        throw std::runtime_error("Unable to listen on HTTP port");
    }

    running_ = true;
    stoppingWorkers_ = false;
    constexpr size_t workerCount = 4;
    workers_.reserve(workerCount);
    for (size_t index = 0; index < workerCount; ++index) {
        workers_.emplace_back(&HttpServer::workerLoop, this);
    }
    std::cout << "MonitoringLinux web interface listening on http://" << host_ << ":" << port_ << std::endl;

    while (running_) {
        sockaddr_in clientAddress{};
        socklen_t clientLength = sizeof(clientAddress);
        const int clientFd = accept(socketFd_, reinterpret_cast<sockaddr*>(&clientAddress), &clientLength);
        if (clientFd < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        timeval timeout{2, 0};
        setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(clientFd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        bool queued = false;
        {
            std::lock_guard<std::mutex> lock(clientsMutex_);
            if (!stoppingWorkers_ && pendingClients_.size() < 64) {
                pendingClients_.push_back(clientFd);
                queued = true;
            }
        }
        if (queued) {
            clientsReady_.notify_one();
        } else {
            sendResponse(clientFd, "503 Service Unavailable", "text/plain; charset=utf-8", "Busy");
            close(clientFd);
        }
    }

    stop();
}
