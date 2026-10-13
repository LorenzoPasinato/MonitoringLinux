#include "HttpServer.h"

#include <arpa/inet.h>
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

void handleRequest(int clientFd, Database& db, const std::string& webRoot) {
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
        const std::string body = readBody(request);
        std::string ip, username, services;
        int port = 22;

        std::string token;
        std::istringstream bodyStream(body);
        while (std::getline(bodyStream, token, '&')) {
            const auto eq = token.find('=');
            if (eq == std::string::npos) {
                continue;
            }
            std::string key = urlDecode(token.substr(0, eq));
            std::string value = urlDecode(token.substr(eq + 1));
            if (key == "ip") ip = value;
            else if (key == "username") username = value;
            else if (key == "port") port = std::stoi(value);
            else if (key == "services") services = value;
        }

        const bool ok = db.upsertTarget(ip, username.empty() ? "root" : username, port, services);
        if (!ok) {
            sendResponse(clientFd, "500 Internal Server Error", "application/json; charset=utf-8", "{\"status\":\"error\"}");
            close(clientFd);
            return;
        }

        sendResponse(clientFd, "200 OK", "application/json; charset=utf-8", "{\"status\":\"ok\"}");
        close(clientFd);
        return;
    }

    if (method == "DELETE" && path.rfind("/api/targets/", 0) == 0) {
        const std::string ip = urlDecode(path.substr(strlen("/api/targets/")));
        const bool ok = db.deleteTarget(ip);
        sendResponse(clientFd, ok ? "200 OK" : "404 Not Found", "application/json; charset=utf-8", ok ? "{\"status\":\"deleted\"}" : "{\"status\":\"not_found\"}");
        close(clientFd);
        return;
    }

    serveStaticFile(clientFd, webRoot, path);
    close(clientFd);
}
}  // namespace

HttpServer::HttpServer(Database& db, const std::string& host, int port, const std::string& webRoot)
    : db_(db), host_(host), port_(port), webRoot_(webRoot) {
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

        handleRequest(clientFd, db_, webRoot_);
    }

    stop();
}

