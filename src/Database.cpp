#include "Database.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <sqlite3.h>

namespace {
std::string utcNow() {
    const auto now = std::chrono::system_clock::now();
    const auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};

#ifdef _WIN32
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif

    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string(buffer);
}

std::string normalizeServices(const std::string& value) {
    std::string result;
    std::string tmp;
    for (char ch : value) {
        if (ch == ',' || ch == ';' || ch == '\n' || ch == '\r') {
            if (!tmp.empty()) {
                if (!result.empty()) {
                    result += ",";
                }
                result += tmp;
                tmp.clear();
            }
        } else if (ch == ' ' || ch == '\t') {
            if (!tmp.empty()) {
                tmp += ch;
            }
        } else {
            tmp += ch;
        }
    }
    if (!tmp.empty()) {
        if (!result.empty()) {
            result += ",";
        }
        result += tmp;
    }
    return result;
}
}  // namespace

Database::Database(const std::string& dbPath) : dbPath_(dbPath) {
    if (!dbPath_.empty()) {
        const auto parent = std::filesystem::path(dbPath_).parent_path();
        if (!parent.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(parent, ec);
        }
    }

    if (sqlite3_open(dbPath_.c_str(), reinterpret_cast<sqlite3**>(&db_)) != SQLITE_OK) {
        std::cerr << "Unable to open SQLite database: " << sqlite3_errmsg(reinterpret_cast<sqlite3*>(db_)) << std::endl;
    }
}

Database::~Database() {
    if (db_ != nullptr) {
        sqlite3_close(reinterpret_cast<sqlite3*>(db_));
        db_ = nullptr;
    }
}

bool Database::initialize() {
    auto* db = reinterpret_cast<sqlite3*>(db_);
    if (db == nullptr) {
        return false;
    }

    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS targets (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            ip TEXT NOT NULL UNIQUE,
            username TEXT NOT NULL,
            port INTEGER NOT NULL,
            services TEXT NOT NULL,
            last_status TEXT,
            last_checked_at TEXT
        );

        CREATE TABLE IF NOT EXISTS checks (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            target_id INTEGER NOT NULL,
            ip TEXT NOT NULL,
            username TEXT NOT NULL,
            service_name TEXT NOT NULL,
            active INTEGER NOT NULL,
            status_text TEXT NOT NULL,
            checked_at TEXT NOT NULL,
            FOREIGN KEY(target_id) REFERENCES targets(id)
        );
    )";

    char* err = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::cerr << "Failed to initialize database schema: " << (err ? err : "unknown") << std::endl;
        sqlite3_free(err);
        return false;
    }

    return true;
}

std::vector<RemoteTarget> Database::listTargets() const {
    std::vector<RemoteTarget> targets;
    auto* db = reinterpret_cast<sqlite3*>(db_);
    if (db == nullptr) {
        return targets;
    }

    const char* sql = "SELECT id, ip, username, port, services, last_status, last_checked_at FROM targets ORDER BY ip;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Unable to list targets: " << sqlite3_errmsg(db) << std::endl;
        return targets;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        RemoteTarget target;
        target.id = sqlite3_column_int(stmt, 0);
        target.ip = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        target.username = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        target.port = sqlite3_column_int(stmt, 3);
        target.services = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        const unsigned char* status = sqlite3_column_text(stmt, 5);
        if (status) {
            target.lastStatus = reinterpret_cast<const char*>(status);
        }
        const unsigned char* checked = sqlite3_column_text(stmt, 6);
        if (checked) {
            target.lastCheckedAt = reinterpret_cast<const char*>(checked);
        }
        targets.push_back(target);
    }
    sqlite3_finalize(stmt);
    return targets;
}

bool Database::upsertTarget(const std::string& ip, const std::string& username, int port, const std::string& services) {
    auto* db = reinterpret_cast<sqlite3*>(db_);
    if (db == nullptr) {
        return false;
    }

    const std::string normalized = normalizeServices(services);
    const std::string timestamp = utcNow();
    const char* sql = R"(
        INSERT INTO targets (ip, username, port, services, last_status, last_checked_at)
        VALUES (?, ?, ?, ?, 'pending', ?)
        ON CONFLICT(ip) DO UPDATE SET
            username = excluded.username,
            port = excluded.port,
            services = excluded.services,
            last_checked_at = excluded.last_checked_at;
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Unable to prepare insert target: " << sqlite3_errmsg(db) << std::endl;
        return false;
    }

    sqlite3_bind_text(stmt, 1, ip.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, port);
    sqlite3_bind_text(stmt, 4, normalized.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, timestamp.c_str(), -1, SQLITE_TRANSIENT);

    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool Database::deleteTarget(const std::string& ip) {
    auto* db = reinterpret_cast<sqlite3*>(db_);
    if (db == nullptr) {
        return false;
    }

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "DELETE FROM targets WHERE ip = ?;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(stmt, 1, ip.c_str(), -1, SQLITE_TRANSIENT);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool Database::saveCheckResult(const ServiceCheckResult& result) {
    auto* db = reinterpret_cast<sqlite3*>(db_);
    if (db == nullptr) {
        return false;
    }

    const char* sql = R"(
        INSERT INTO checks (target_id, ip, username, service_name, active, status_text, checked_at)
        VALUES (?, ?, ?, ?, ?, ?, ?);
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Unable to prepare check result insert: " << sqlite3_errmsg(db) << std::endl;
        return false;
    }

    sqlite3_bind_int(stmt, 1, result.targetId);
    sqlite3_bind_text(stmt, 2, result.ip.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, result.username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, result.serviceName.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, result.active ? 1 : 0);
    sqlite3_bind_text(stmt, 6, result.statusText.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, result.checkedAt.c_str(), -1, SQLITE_TRANSIENT);

    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return false;
    }

    const char* updateSql = "UPDATE targets SET last_status = ?, last_checked_at = ? WHERE ip = ?;";
    sqlite3_stmt* update = nullptr;
    if (sqlite3_prepare_v2(db, updateSql, -1, &update, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(update, 1, result.statusText.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(update, 2, result.checkedAt.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(update, 3, result.ip.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(update);
    sqlite3_finalize(update);
    return true;
}

std::vector<ServiceCheckResult> Database::getLatestChecks() const {
    std::vector<ServiceCheckResult> results;
    auto* db = reinterpret_cast<sqlite3*>(db_);
    if (db == nullptr) {
        return results;
    }

    const char* sql = R"(
        WITH ranked AS (
            SELECT id, target_id, ip, username, service_name, active, status_text, checked_at,
                   ROW_NUMBER() OVER (PARTITION BY ip, service_name ORDER BY id DESC) AS rn
            FROM checks
        )
        SELECT id, target_id, ip, username, service_name, active, status_text, checked_at
        FROM ranked
        WHERE rn = 1
        ORDER BY checked_at DESC;
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Unable to prepare latest checks: " << sqlite3_errmsg(db) << std::endl;
        return results;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ServiceCheckResult result;
        result.id = sqlite3_column_int(stmt, 0);
        result.targetId = sqlite3_column_int(stmt, 1);
        result.ip = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        result.username = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        result.serviceName = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        result.active = sqlite3_column_int(stmt, 5) == 1;
        result.statusText = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        result.checkedAt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
        results.push_back(result);
    }
    sqlite3_finalize(stmt);
    return results;
}

