#include "Database.h"
#include "HttpServer.h"
#include "SshCredentials.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <algorithm>
#include <vector>

extern "C" char** environ;

namespace {
struct Config {
    std::string host = "127.0.0.1";
    int port = 8080;
    std::string dbPath = "~/.local/share/monitoring-linux/monitoring.db";
    std::string webRoot = "/opt/monitoring-linux/web";
    int sshPort = 22;
    int checkIntervalSeconds = 15;
};

std::string expandHomePath(const std::string& path) {
    if (path != "~" && path.rfind("~/", 0) != 0) {
        return path;
    }

    const char* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') {
        return {};
    }
    return std::string(home) + path.substr(1);
}

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

std::string quoteRemoteArgument(const std::string& value) {
    std::string quoted = "'";
    for (char character : value) {
        if (character == '\'') {
            quoted += "'\\''";
        } else {
            quoted += character;
        }
    }
    quoted += "'";
    return quoted;
}

int runSsh(const RemoteTarget& target, const std::string& remoteCommand, const std::string& password, std::string& output) {
    int outputPipe[2];
    if (pipe(outputPipe) != 0) {
        return -1;
    }

    std::vector<std::string> arguments = {
        "ssh", "-o", password.empty() ? "BatchMode=yes" : "BatchMode=no",
        "-o", "StrictHostKeyChecking=accept-new",
        "-o", "ConnectTimeout=5",
        "-o", "NumberOfPasswordPrompts=1"
    };
    if (!password.empty()) {
        arguments.insert(arguments.end(), {"-o", "PreferredAuthentications=publickey,password,keyboard-interactive"});
    }
    arguments.insert(arguments.end(), {"-p", std::to_string(target.port), "-l", target.username, "--", target.ip, remoteCommand});

    std::vector<char*> argumentPointers;
    argumentPointers.reserve(arguments.size() + 1);
    for (auto& argument : arguments) {
        argumentPointers.push_back(argument.data());
    }
    argumentPointers.push_back(nullptr);

    std::vector<std::string> environment;
    for (char** current = environ; current && *current; ++current) {
        const std::string variable(*current);
        if (variable.rfind("LC_ALL=", 0) == 0 || variable.rfind("SSH_ASKPASS=", 0) == 0 ||
            variable.rfind("SSH_ASKPASS_REQUIRE=", 0) == 0 || variable.rfind("DISPLAY=", 0) == 0 ||
            variable.rfind("MONITORING_SSH_PASSWORD=", 0) == 0) {
            continue;
        }
        environment.push_back(variable);
    }
    environment.emplace_back("LC_ALL=C");
    if (!password.empty()) {
        environment.emplace_back("SSH_ASKPASS=/opt/monitoring-linux/monitoring-linux-ssh-askpass");
        environment.emplace_back("SSH_ASKPASS_REQUIRE=force");
        environment.emplace_back("DISPLAY=monitoring-linux:0");
        environment.emplace_back("MONITORING_SSH_PASSWORD=" + password);
    }
    std::vector<char*> environmentPointers;
    environmentPointers.reserve(environment.size() + 1);
    for (auto& variable : environment) {
        environmentPointers.push_back(variable.data());
    }
    environmentPointers.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    const int actionsInit = posix_spawn_file_actions_init(&actions);
    if (actionsInit != 0) {
        close(outputPipe[0]);
        close(outputPipe[1]);
        return -1;
    }
    const int actionsResult = posix_spawn_file_actions_adddup2(&actions, outputPipe[1], STDOUT_FILENO) ||
                              posix_spawn_file_actions_adddup2(&actions, outputPipe[1], STDERR_FILENO) ||
                              posix_spawn_file_actions_addclose(&actions, outputPipe[0]) ||
                              posix_spawn_file_actions_addclose(&actions, outputPipe[1]);
    if (actionsResult != 0) {
        posix_spawn_file_actions_destroy(&actions);
        close(outputPipe[0]);
        close(outputPipe[1]);
        return -1;
    }

    pid_t child = -1;
    const int spawnResult = posix_spawnp(&child, "ssh", &actions, nullptr, argumentPointers.data(), environmentPointers.data());
    posix_spawn_file_actions_destroy(&actions);
    close(outputPipe[1]);
    if (spawnResult != 0) {
        close(outputPipe[0]);
        return spawnResult == ENOENT ? 127 : -1;
    }

    char buffer[4096] = {};
    ssize_t bytesRead = 0;
    while ((bytesRead = read(outputPipe[0], buffer, sizeof(buffer))) > 0) {
        output.append(buffer, static_cast<size_t>(bytesRead));
    }
    close(outputPipe[0]);

    int childStatus = 0;
    while (waitpid(child, &childStatus, 0) < 0) {
        if (errno != EINTR) {
            return -1;
        }
    }
    return WIFEXITED(childStatus) ? WEXITSTATUS(childStatus) : 255;
}

std::vector<ServiceCheckResult> probeRemoteServices(const RemoteTarget& target, const std::vector<std::string>& serviceNames, const std::string& password, MemoryUsage& memory) {
    std::vector<ServiceCheckResult> results;

    const std::string checkedAt = nowIso();
    memory = {};
    memory.checkedAt = checkedAt;
    for (const auto& serviceName : serviceNames) {
        ServiceCheckResult result;
        result.name = target.name;
        result.ip = target.ip;
        result.username = target.username;
        result.serviceName = serviceName;
        result.checkedAt = checkedAt;
        result.targetId = target.id;
        results.push_back(std::move(result));
    }

    std::string remoteCommand;
    for (size_t index = 0; index < serviceNames.size(); ++index) {
        remoteCommand += "systemctl is-active --quiet " + quoteRemoteArgument(serviceNames[index]) +
                         "; check_status=$?; printf '\\n__MONITORING_RESULT__" + std::to_string(index) + ":%s\\n' \"$check_status\"; ";
    }
    remoteCommand += "awk '/^MemTotal:/ { total=$2; have_total=1 } /^MemAvailable:/ { available=$2; have_available=1 } END { if (have_total && have_available && total > 0 && available >= 0 && available <= total) printf \"\\n__MONITORING_MEMORY__:%.0f:%.0f\\n\", total-available, total; }' /proc/meminfo";
    std::string output;
    const int exitCode = runSsh(target, remoteCommand, password, output);

    const std::string memoryMarker = "__MONITORING_MEMORY__:";
    const size_t memoryPosition = output.find(memoryMarker);
    if (memoryPosition != std::string::npos) {
        const size_t lineEnd = output.find_first_of("\r\n", memoryPosition);
        const std::string memoryLine = output.substr(memoryPosition + memoryMarker.size(), lineEnd - memoryPosition - memoryMarker.size());
        const size_t separator = memoryLine.find(':');
        if (separator != std::string::npos) {
            try {
                size_t usedLength = 0;
                size_t totalLength = 0;
                const int64_t usedKb = std::stoll(memoryLine.substr(0, separator), &usedLength);
                const int64_t totalKb = std::stoll(memoryLine.substr(separator + 1), &totalLength);
                if (usedLength == separator && totalLength == memoryLine.size() - separator - 1 &&
                    usedKb >= 0 && totalKb > 0 && usedKb <= totalKb) {
                    memory.available = true;
                    memory.usedKb = usedKb;
                    memory.totalKb = totalKb;
                }
            } catch (const std::exception&) {
            }
        }
    }

    const std::string marker = "__MONITORING_RESULT__";
    size_t position = 0;
    size_t completed = 0;
    while ((position = output.find(marker, position)) != std::string::npos) {
        const size_t lineEnd = output.find_first_of("\r\n", position);
        const std::string resultLine = output.substr(position + marker.size(), lineEnd - position - marker.size());
        const size_t separator = resultLine.find(':');
        if (separator != std::string::npos) {
            try {
                const size_t resultIndex = static_cast<size_t>(std::stoul(resultLine.substr(0, separator)));
                if (resultIndex < results.size()) {
                    results[resultIndex].active = resultLine.substr(separator + 1) == "0";
                    results[resultIndex].statusText = results[resultIndex].active ? "active" : "inactive or not found";
                    ++completed;
                }
            } catch (...) {
            }
        }
        if (lineEnd == std::string::npos) break;
        position = lineEnd + 1;
    }

    if (completed != results.size()) {
        const bool denied = output.find("Permission denied") != std::string::npos || output.find("Authentication failed") != std::string::npos;
        const std::string errorStatus = denied ? (password.empty() ? "password required" : "authentication failed") :
                                        (exitCode == 127 ? "ssh command not found" : "ssh unavailable");
        for (auto& result : results) {
            if (result.statusText.empty()) result.statusText = errorStatus;
        }
    }

    return results;
}

void monitorLoop(Database& db, SshCredentials& credentials, MonitorSignal& monitorSignal, const Config& config, std::atomic<bool>& running) {
    while (running) {
        const auto targets = db.listTargets();
        std::atomic<size_t> nextTarget{0};
        const size_t workerCount = std::min<size_t>(4, targets.size());
        std::vector<std::thread> workers;
        workers.reserve(workerCount);
        for (size_t worker = 0; worker < workerCount; ++worker) {
            workers.emplace_back([&] {
                while (running) {
                    const size_t index = nextTarget.fetch_add(1);
                    if (index >= targets.size()) break;
                    const auto services = splitCsv(targets[index].services);
                    MemoryUsage memory;
                    const auto checks = probeRemoteServices(targets[index], services, credentials.passwordFor(targets[index].id), memory);
                    if (!checks.empty()) db.saveCheckResults(checks);
                    db.updateTargetMemory(targets[index].id, memory);
                }
            });
        }
        for (auto& worker : workers) {
            worker.join();
        }
        if (!monitorSignal.waitForNext(config.checkIntervalSeconds, running)) {
            break;
        }
    }
}

void printUsage(const char* programName) {
    std::cout << "Usage: " << programName << " [--config /path/to/config]" << std::endl;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--internal-ssh-askpass") {
        const char* password = std::getenv("MONITORING_SSH_PASSWORD");
        if (password == nullptr) {
            return 1;
        }
        std::cout << password << std::endl;
        return 0;
    }

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
    if (config.host != "127.0.0.1") {
        std::cerr << "Dashboard access is restricted to this machine; using 127.0.0.1." << std::endl;
        config.host = "127.0.0.1";
    }
    if (config.dbPath == "/var/lib/monitoring-linux/monitoring.db") {
        config.dbPath = "~/.local/share/monitoring-linux/monitoring.db";
    }
    if (config.webRoot == "/usr/share/monitoring-linux") {
        config.webRoot = "/opt/monitoring-linux/web";
    }
    config.dbPath = expandHomePath(config.dbPath);
    if (config.dbPath.empty()) {
        std::cerr << "Unable to resolve database path: HOME is not set" << std::endl;
        return 1;
    }

    const std::filesystem::path dbParent = std::filesystem::path(config.dbPath).parent_path();
    if (!dbParent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(dbParent, ec);
        if (ec) {
            std::cerr << "Unable to create database directory: " << ec.message() << std::endl;
            return 1;
        }
    }

    Database db(config.dbPath);
    if (!db.initialize()) {
        std::cerr << "Failed to initialize database" << std::endl;
        return 1;
    }

    SshCredentials credentials;
    MonitorSignal monitorSignal;
    std::atomic<bool> running{true};
    std::thread monitorThread(monitorLoop, std::ref(db), std::ref(credentials), std::ref(monitorSignal), std::cref(config), std::ref(running));

    try {
        HttpServer server(db, credentials, monitorSignal, config.host, config.port, config.webRoot);
        server.run();
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << std::endl;
        running = false;
    }

    running = false;
    monitorSignal.notify();
    if (monitorThread.joinable()) {
        monitorThread.join();
    }

    return 0;
}
