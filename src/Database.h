#pragma once

#include <string>
#include <mutex>
#include <vector>

struct RemoteTarget {
    int id = 0;
    std::string name;
    std::string ip;
    std::string username;
    int port = 22;
    std::string services;
    std::string lastStatus;
    std::string lastCheckedAt;
};

struct ServiceCheckResult {
    int id = 0;
    int targetId = 0;
    std::string name;
    std::string ip;
    std::string username;
    std::string serviceName;
    bool active = false;
    std::string statusText;
    std::string checkedAt;
};

class Database {
public:
    explicit Database(const std::string& dbPath);
    ~Database();

    bool initialize();
    std::vector<RemoteTarget> listTargets() const;
    bool upsertTarget(const std::string& name, const std::string& ip, const std::string& username, int port, const std::string& services);
    bool updateTarget(int id, const std::string& name, const std::string& ip, const std::string& username, int port, const std::string& services);
    bool deleteTarget(int id);
    std::vector<ServiceCheckResult> getLatestChecks() const;
    bool saveCheckResult(const ServiceCheckResult& result);
    bool saveCheckResults(const std::vector<ServiceCheckResult>& results);

private:
    std::string dbPath_;
    void* db_ = nullptr;
    mutable std::mutex mutex_;
};

