#pragma once

#include "Database.h"
#include "SshCredentials.h"

#include <string>

class HttpServer {
public:
    HttpServer(Database& db, SshCredentials& credentials, const std::string& host, int port, const std::string& webRoot);
    ~HttpServer();

    void run();
    void stop();

private:
    Database& db_;
    SshCredentials& credentials_;
    std::string host_;
    int port_ = 8080;
    std::string webRoot_;
    int socketFd_ = -1;
    bool running_ = false;
};

