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
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

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
             << ",\"last_checked_at\":\"" << jsonEscape(t.lastCheckedAt) << "\"" << "}";
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

void handleRequest(int clientFd, Database& db, SshCredentials& credentials, MonitorSignal& monitorSignal, const std::string& webRoot) {
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
        if (method == "PUT" && resource.empty()) {
            const auto fields = parseForm(readBody(request));
            const std::string name = fields.count("name") ? fields.at("name") : "";
            const std::string ip = fields.count("ip") ? fields.at("ip") : "";
            const std::string username = fields.count("username") ? fields.at("username") : "root";
            const std::string services = fields.count("services") ? fields.at("services") : "";
            const std::string password = fields.count("password") ? fields.at("password") : "";
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
        handleRequest(clientFd, db_, credentials_, monitorSignal_, webRoot_);
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

