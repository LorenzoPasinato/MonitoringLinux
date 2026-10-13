#pragma once

#include "Database.h"
#include "MonitorSignal.h"
#include "SshCredentials.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class HttpServer {
public:
    HttpServer(Database& db, SshCredentials& credentials, MonitorSignal& monitorSignal, const std::string& host, int port, const std::string& webRoot);
    ~HttpServer();

    void run();
    void stop();

private:
    void workerLoop();

    Database& db_;
    SshCredentials& credentials_;
    MonitorSignal& monitorSignal_;
    std::string host_;
    int port_ = 8080;
    std::string webRoot_;
    int socketFd_ = -1;
    std::atomic<bool> running_{false};
    std::mutex clientsMutex_;
    std::condition_variable clientsReady_;
    std::deque<int> pendingClients_;
    std::vector<std::thread> workers_;
    bool stoppingWorkers_ = false;
};

