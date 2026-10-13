#include "HttpServer.h"

#include <arpa/inet.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
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

    send(clientFd, header.str().c_str(), header.str().size(), 0);
    if (!body.empty()) {
        send(clientFd, body.c_str(), body.size(), 0);
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

void handleRequest(int clientFd, Database& db, SshCredentials& credentials, const std::string& webRoot) {
    char buffer[8192] = {};
    const ssize_t received = recv(clientFd, buffer, sizeof(buffer) - 1, 0);
    if (received <= 0) {
        close(clientFd);
        return;
    }

    const std::string request(buffer, static_cast<size_t>(received));
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
        if (ok && previousTargetId > 0 && connectionChanged) credentials.erase(previousTargetId);
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
            if (ok && connectionChanged) credentials.erase(targetId);
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
            if (ok) credentials.setPassword(targetId, password->second);
            sendResponse(clientFd, ok ? "200 OK" : "400 Bad Request", "application/json; charset=utf-8", ok ? "{\"status\":\"password_set\"}" : "{\"status\":\"invalid_password\"}");
            close(clientFd);
            return;
        }

        if (method == "DELETE" && resource.empty()) {
            const bool ok = db.deleteTarget(targetId);
            if (ok) credentials.erase(targetId);
            sendResponse(clientFd, ok ? "200 OK" : "404 Not Found", "application/json; charset=utf-8", ok ? "{\"status\":\"deleted\"}" : "{\"status\":\"not_found\"}");
            close(clientFd);
            return;
        }
    }

    serveStaticFile(clientFd, webRoot, path);
    close(clientFd);
}
}  // namespace

HttpServer::HttpServer(Database& db, SshCredentials& credentials, const std::string& host, int port, const std::string& webRoot)
    : db_(db), credentials_(credentials), host_(host), port_(port), webRoot_(webRoot) {
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

        handleRequest(clientFd, db_, credentials_, webRoot_);
    }

    stop();
}

