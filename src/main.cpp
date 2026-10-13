#include "Database.h"
#include "HttpServer.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {
struct Config {
    std::string host = "0.0.0.0";
    int port = 8080;
    std::string dbPath = "/var/lib/monitoring-linux/monitoring.db";
    std::string webRoot = "/usr/share/monitoring-linux";
    int sshPort = 22;
    int checkIntervalSeconds = 15;
};

std::string trim(const std::string& value) {
    const auto start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return "";
    }
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

std::vector<std::string> splitCsv(const std::string& text) {
    std::vector<std::string> values;
    std::stringstream ss(text);
    std::string item;
    while (std::getline(ss, item, ',')) {
        std::string cleaned = trim(item);
        if (!cleaned.empty()) {
            values.push_back(cleaned);
        }
    }
    return values;
}

Config loadConfig(const std::string& path) {
    Config cfg;
    std::ifstream in(path);
    if (!in) {
        return cfg;
    }

    std::string line;
    while (std::getline(in, line)) {
        const auto comment = line.find('#');
        if (comment != std::string::npos) {
            line = line.substr(0, comment);
        }
        if (line.empty()) {
            continue;
        }

        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }

        const std::string key = trim(line.substr(0, eq));
        const std::string value = trim(line.substr(eq + 1));
        if (key == "host") cfg.host = value;
        else if (key == "port") cfg.port = std::stoi(value);
        else if (key == "db_path") cfg.dbPath = value;
        else if (key == "web_root") cfg.webRoot = value;
        else if (key == "ssh_port") cfg.sshPort = std::stoi(value);
        else if (key == "check_interval_seconds") cfg.checkIntervalSeconds = std::stoi(value);
    }

    return cfg;
}

std::string nowIso() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};

#ifdef _WIN32
    gmtime_s(&tm, &time);
#else
    gmtime_r(&time, &tm);
#endif

    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string(buffer);
}

bool runCommand(const std::string& command, std::string& output) {
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        return false;
    }

    char buffer[4096] = {};
    std::string result;
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }

    const int exitCode = pclose(pipe);
    output = result;
    return exitCode == 0 || exitCode == 3;
}

ServiceCheckResult probeRemoteService(const RemoteTarget& target, const std::string& serviceName) {
    ServiceCheckResult result;
    result.ip = target.ip;
    result.username = target.username;
    result.serviceName = serviceName;
    result.checkedAt = nowIso();
    result.targetId = target.id;

    const std::string sshOptions = "-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 -p " + std::to_string(target.port);
    const std::string shellCommand = "ssh " + sshOptions + " " + target.username + "@" + target.ip + " \"systemctl is-active --quiet " + serviceName + "; echo __MONITORING_STATUS:$?\" 2>/dev/null";

    std::string output;
    FILE* pipe = popen(shellCommand.c_str(), "r");
    if (!pipe) {
        result.active = false;
        result.statusText = "ssh command failed";
        return result;
    }

    char buffer[4096] = {};
    std::string stdOut;
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        stdOut += buffer;
    }
    const int exitCode = pclose(pipe);
    result.statusText = stdOut;

    const std::string marker = "__MONITORING_STATUS:";
    const auto pos = stdOut.rfind(marker);
    if (pos != std::string::npos) {
        const std::string statusCode = trim(stdOut.substr(pos + marker.size()));
        result.active = statusCode == "0";
        if (result.active) {
            result.statusText = "active";
        } else {
            result.statusText = "inactive or not found";
        }
    } else {
        result.active = exitCode == 0;
        result.statusText = exitCode == 0 ? "active" : "inactive or ssh unavailable";
    }

    return result;
}

void monitorLoop(Database& db, const Config& config, std::atomic<bool>& running) {
    while (running) {
        const auto targets = db.listTargets();
        for (const auto& target : targets) {
            auto serviceList = splitCsv(target.services);
            for (const auto& serviceName : serviceList) {
                auto check = probeRemoteService(target, serviceName);
                db.saveCheckResult(check);
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(config.checkIntervalSeconds));
    }
}

void printUsage(const char* programName) {
    std::cout << "Usage: " << programName << " [--config /path/to/config]" << std::endl;
}
}  // namespace

int main(int argc, char** argv) {
    std::string configPath = "/etc/monitoring-linux/monitoring.conf";
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            configPath = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown argument: " << arg << std::endl;
            printUsage(argv[0]);
            return 1;
        }
    }

    Config config = loadConfig(configPath);
    const std::filesystem::path dbParent = std::filesystem::path(config.dbPath).parent_path();
    if (!dbParent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(dbParent, ec);
    }

    Database db(config.dbPath);
    if (!db.initialize()) {
        std::cerr << "Failed to initialize database" << std::endl;
        return 1;
    }

    std::atomic<bool> running{true};
    std::thread monitorThread(monitorLoop, std::ref(db), std::cref(config), std::ref(running));

    try {
        HttpServer server(db, config.host, config.port, config.webRoot);
        server.run();
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << std::endl;
        running = false;
    }

    running = false;
    if (monitorThread.joinable()) {
        monitorThread.join();
    }

    return 0;
}

