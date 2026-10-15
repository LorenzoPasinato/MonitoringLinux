#pragma once

#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

class SshCredentials {
public:
    ~SshCredentials() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& entry : passwords_) {
            wipe(entry.second);
        }
        for (auto& entry : sudoPasswords_) {
            wipe(entry.second);
        }
    }

    void setPassword(int targetId, std::string password) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& stored = passwords_[targetId];
        wipe(stored);
        stored = std::move(password);
    }

    std::string passwordFor(int targetId) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = passwords_.find(targetId);
        return found == passwords_.end() ? std::string{} : found->second;
    }

    void setSudoPassword(int targetId, std::string password) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& stored = sudoPasswords_[targetId];
        wipe(stored);
        stored = std::move(password);
    }

    std::string sudoPasswordFor(int targetId) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = sudoPasswords_.find(targetId);
        return found == sudoPasswords_.end() ? std::string{} : found->second;
    }

    void erase(int targetId) {
        std::lock_guard<std::mutex> lock(mutex_);
        erasePassword(passwords_, targetId);
        erasePassword(sudoPasswords_, targetId);
    }

private:
    static void wipe(std::string& secret) {
        volatile char* data = secret.empty() ? nullptr : secret.data();
        for (size_t index = 0; index < secret.size(); ++index) {
            data[index] = '\0';
        }
    }

    static void erasePassword(std::unordered_map<int, std::string>& passwords, int targetId) {
        const auto found = passwords.find(targetId);
        if (found != passwords.end()) {
            wipe(found->second);
            passwords.erase(found);
        }
    }

    mutable std::mutex mutex_;
    std::unordered_map<int, std::string> passwords_;
    std::unordered_map<int, std::string> sudoPasswords_;
};