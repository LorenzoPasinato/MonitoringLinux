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
    std::lock_guard<std::mutex> lock(mutex_);
    auto* db = reinterpret_cast<sqlite3*>(db_);
    if (db == nullptr) {
        return false;
    }

    if (sqlite3_exec(db, "PRAGMA foreign_keys = ON;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        std::cerr << "Failed to enable SQLite foreign keys: " << sqlite3_errmsg(db) << std::endl;
        return false;
    }
    sqlite3_busy_timeout(db, 3000);
    if (sqlite3_exec(db, "PRAGMA journal_mode = WAL; PRAGMA synchronous = NORMAL; PRAGMA temp_store = MEMORY;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        std::cerr << "Failed to configure SQLite for responsive local access: " << sqlite3_errmsg(db) << std::endl;
        return false;
    }

    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS targets (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL DEFAULT '',
            ip TEXT NOT NULL UNIQUE,
            username TEXT NOT NULL,
            port INTEGER NOT NULL,
            services TEXT NOT NULL,
            last_status TEXT,
            last_checked_at TEXT,
            memory_used_kb INTEGER,
            memory_total_kb INTEGER,
            memory_checked_at TEXT
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

    if (sqlite3_exec(db, "CREATE INDEX IF NOT EXISTS idx_checks_latest ON checks(ip, service_name, id DESC);", nullptr, nullptr, nullptr) != SQLITE_OK) {
        std::cerr << "Failed to index service checks: " << sqlite3_errmsg(db) << std::endl;
        return false;
    }

    bool hasNameColumn = false;
    bool hasMemoryUsedColumn = false;
    bool hasMemoryTotalColumn = false;
    bool hasMemoryCheckedColumn = false;
    sqlite3_stmt* columns = nullptr;
    if (sqlite3_prepare_v2(db, "PRAGMA table_info(targets);", -1, &columns, nullptr) != SQLITE_OK) {
        return false;
    }
    while (sqlite3_step(columns) == SQLITE_ROW) {
        const auto* columnName = sqlite3_column_text(columns, 1);
        if (columnName) {
            const std::string name(reinterpret_cast<const char*>(columnName));
            if (name == "name") hasNameColumn = true;
            else if (name == "memory_used_kb") hasMemoryUsedColumn = true;
            else if (name == "memory_total_kb") hasMemoryTotalColumn = true;
            else if (name == "memory_checked_at") hasMemoryCheckedColumn = true;
        }
    }
    sqlite3_finalize(columns);

    if (!hasNameColumn) {
        char* migrationError = nullptr;
        if (sqlite3_exec(db, "ALTER TABLE targets ADD COLUMN name TEXT NOT NULL DEFAULT '';", nullptr, nullptr, &migrationError) != SQLITE_OK) {
            std::cerr << "Failed to add target names: " << (migrationError ? migrationError : "unknown") << std::endl;
            sqlite3_free(migrationError);
            return false;
        }
    }

    const auto addMemoryColumn = [db](bool present, const char* sql, const char* description) {
        if (present) return true;
        char* error = nullptr;
        if (sqlite3_exec(db, sql, nullptr, nullptr, &error) == SQLITE_OK) return true;
        std::cerr << "Failed to add " << description << ": " << (error ? error : sqlite3_errmsg(db)) << std::endl;
        sqlite3_free(error);
        return false;
    };
    if (!addMemoryColumn(hasMemoryUsedColumn, "ALTER TABLE targets ADD COLUMN memory_used_kb INTEGER;", "memory usage")) return false;
    if (!addMemoryColumn(hasMemoryTotalColumn, "ALTER TABLE targets ADD COLUMN memory_total_kb INTEGER;", "total memory")) return false;
    if (!addMemoryColumn(hasMemoryCheckedColumn, "ALTER TABLE targets ADD COLUMN memory_checked_at TEXT;", "memory check timestamp")) return false;

    if (sqlite3_exec(db, "UPDATE targets SET name = 'VM_' || id WHERE name IS NULL OR name = '';", nullptr, nullptr, nullptr) != SQLITE_OK) {
        std::cerr << "Failed to assign default target names: " << sqlite3_errmsg(db) << std::endl;
        return false;
    }

    return true;
}

std::vector<RemoteTarget> Database::listTargets() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<RemoteTarget> targets;
    auto* db = reinterpret_cast<sqlite3*>(db_);
    if (db == nullptr) {
        return targets;
    }

    const char* sql = "SELECT id, name, ip, username, port, services, last_status, last_checked_at, memory_used_kb, memory_total_kb, memory_checked_at FROM targets ORDER BY id;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Unable to list targets: " << sqlite3_errmsg(db) << std::endl;
        return targets;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        RemoteTarget target;
        target.id = sqlite3_column_int(stmt, 0);
        target.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        target.ip = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        target.username = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        target.port = sqlite3_column_int(stmt, 4);
        target.services = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        const unsigned char* status = sqlite3_column_text(stmt, 6);
        if (status) {
            target.lastStatus = reinterpret_cast<const char*>(status);
        }
        const unsigned char* checked = sqlite3_column_text(stmt, 7);
        if (checked) {
            target.lastCheckedAt = reinterpret_cast<const char*>(checked);
        }
        if (sqlite3_column_type(stmt, 8) != SQLITE_NULL && sqlite3_column_type(stmt, 9) != SQLITE_NULL) {
            target.memory.available = true;
            target.memory.usedKb = sqlite3_column_int64(stmt, 8);
            target.memory.totalKb = sqlite3_column_int64(stmt, 9);
        }
        const unsigned char* memoryChecked = sqlite3_column_text(stmt, 10);
        if (memoryChecked) {
            target.memory.checkedAt = reinterpret_cast<const char*>(memoryChecked);
        }
        targets.push_back(target);
    }
    sqlite3_finalize(stmt);
    return targets;
}

bool Database::upsertTarget(const std::string& name, const std::string& ip, const std::string& username, int port, const std::string& services) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* db = reinterpret_cast<sqlite3*>(db_);
    if (db == nullptr) {
        return false;
    }

    const std::string normalized = normalizeServices(services);
    const std::string timestamp = utcNow();
    const char* sql = R"(
        INSERT INTO targets (name, ip, username, port, services, last_status, last_checked_at)
        VALUES (?, ?, ?, ?, ?, 'pending', ?)
        ON CONFLICT(ip) DO UPDATE SET
            name = CASE WHEN excluded.name = '' THEN targets.name ELSE excluded.name END,
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

    sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, ip.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, port);
    sqlite3_bind_text(stmt, 5, normalized.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, timestamp.c_str(), -1, SQLITE_TRANSIENT);

    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return false;
    }

    sqlite3_stmt* lookup = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT id FROM targets WHERE ip = ?;", -1, &lookup, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(lookup, 1, ip.c_str(), -1, SQLITE_TRANSIENT);
    const bool found = sqlite3_step(lookup) == SQLITE_ROW;
    const int id = found ? sqlite3_column_int(lookup, 0) : 0;
    sqlite3_finalize(lookup);
    if (!found) {
        return false;
    }

    sqlite3_stmt* assignName = nullptr;
    if (sqlite3_prepare_v2(db, "UPDATE targets SET name = 'VM_' || id WHERE id = ? AND name = '';", -1, &assignName, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int(assignName, 1, id);
    const bool named = sqlite3_step(assignName) == SQLITE_DONE;
    sqlite3_finalize(assignName);
    return named;
}

bool Database::updateTarget(int id, const std::string& name, const std::string& ip, const std::string& username, int port, const std::string& services) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* db = reinterpret_cast<sqlite3*>(db_);
    if (db == nullptr) {
        return false;
    }

    const std::string normalized = normalizeServices(services);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE targets SET name = CASE WHEN ? = '' THEN 'VM_' || id ELSE ? END, ip = ?, username = ?, port = ?, services = ? WHERE id = ?;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, ip.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, port);
    sqlite3_bind_text(stmt, 6, normalized.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 7, id);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) == 1;
    sqlite3_finalize(stmt);
    return ok;
}

bool Database::deleteTarget(int id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* db = reinterpret_cast<sqlite3*>(db_);
    if (db == nullptr || sqlite3_exec(db, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_stmt* checks = nullptr;
    sqlite3_stmt* target = nullptr;
    bool ok = sqlite3_prepare_v2(db, "DELETE FROM checks WHERE target_id = ?;", -1, &checks, nullptr) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_int(checks, 1, id);
        ok = sqlite3_step(checks) == SQLITE_DONE;
    }
    sqlite3_finalize(checks);

    if (ok) {
        ok = sqlite3_prepare_v2(db, "DELETE FROM targets WHERE id = ?;", -1, &target, nullptr) == SQLITE_OK;
    }
    if (ok) {
        sqlite3_bind_int(target, 1, id);
        ok = sqlite3_step(target) == SQLITE_DONE && sqlite3_changes(db) == 1;
    }
    sqlite3_finalize(target);

    if (sqlite3_exec(db, ok ? "COMMIT;" : "ROLLBACK;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return false;
    }
    return ok;
}

bool Database::updateTargetMemory(int id, const MemoryUsage& memory) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* db = reinterpret_cast<sqlite3*>(db_);
    if (db == nullptr) return false;

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE targets SET memory_used_kb = ?, memory_total_kb = ?, memory_checked_at = ? WHERE id = ?;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Unable to prepare memory update: " << sqlite3_errmsg(db) << std::endl;
        return false;
    }

    if (memory.available) {
        sqlite3_bind_int64(stmt, 1, memory.usedKb);
        sqlite3_bind_int64(stmt, 2, memory.totalKb);
    } else {
        sqlite3_bind_null(stmt, 1);
        sqlite3_bind_null(stmt, 2);
    }
    sqlite3_bind_text(stmt, 3, memory.checkedAt.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, id);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) == 1;
    if (!ok) std::cerr << "Unable to update target memory: " << sqlite3_errmsg(db) << std::endl;
    sqlite3_finalize(stmt);
    return ok;
}

bool Database::saveCheckResult(const ServiceCheckResult& result) {
    return saveCheckResults({result});
}

bool Database::saveCheckResults(const std::vector<ServiceCheckResult>& results) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* db = reinterpret_cast<sqlite3*>(db_);
    if (db == nullptr || results.empty()) {
        return false;
    }

    if (sqlite3_exec(db, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return false;
    }

    const char* insertSql = R"(
        INSERT INTO checks (target_id, ip, username, service_name, active, status_text, checked_at)
        VALUES (?, ?, ?, ?, ?, ?, ?);
    )";
    const char* updateSql = "UPDATE targets SET last_status = ?, last_checked_at = ? WHERE ip = ?;";
    sqlite3_stmt* insert = nullptr;
    sqlite3_stmt* update = nullptr;
    if (sqlite3_prepare_v2(db, insertSql, -1, &insert, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(db, updateSql, -1, &update, nullptr) != SQLITE_OK) {
        std::cerr << "Unable to prepare check result insert: " << sqlite3_errmsg(db) << std::endl;
        sqlite3_finalize(insert);
        sqlite3_finalize(update);
        sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    bool ok = true;
    for (const auto& result : results) {
        sqlite3_reset(insert);
        sqlite3_clear_bindings(insert);
        sqlite3_bind_int(insert, 1, result.targetId);
        sqlite3_bind_text(insert, 2, result.ip.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert, 3, result.username.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert, 4, result.serviceName.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(insert, 5, result.active ? 1 : 0);
        sqlite3_bind_text(insert, 6, result.statusText.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert, 7, result.checkedAt.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(insert) != SQLITE_DONE) {
            ok = false;
            break;
        }

        sqlite3_reset(update);
        sqlite3_clear_bindings(update);
        sqlite3_bind_text(update, 1, result.statusText.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(update, 2, result.checkedAt.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(update, 3, result.ip.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(update) != SQLITE_DONE) {
            ok = false;
            break;
        }
    }
    sqlite3_finalize(insert);
    sqlite3_finalize(update);
    if (sqlite3_exec(db, ok ? "COMMIT;" : "ROLLBACK;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return false;
    }
    return ok;
}

std::vector<ServiceCheckResult> Database::getLatestChecks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ServiceCheckResult> results;
    auto* db = reinterpret_cast<sqlite3*>(db_);
    if (db == nullptr) {
        return results;
    }

    const char* sql = R"(
        SELECT checks.id, checks.target_id, targets.name, checks.ip, checks.username,
               checks.service_name, checks.active, checks.status_text, checks.checked_at
        FROM checks
        JOIN (
            SELECT MAX(id) AS id
            FROM checks
            GROUP BY ip, service_name
        ) latest ON latest.id = checks.id
        LEFT JOIN targets ON targets.id = checks.target_id
        ORDER BY checks.checked_at DESC;
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
        const auto* name = sqlite3_column_text(stmt, 2);
        if (name) result.name = reinterpret_cast<const char*>(name);
        result.ip = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        result.username = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        result.serviceName = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        result.active = sqlite3_column_int(stmt, 6) == 1;
        result.statusText = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
        result.checkedAt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));
        results.push_back(result);
    }
    sqlite3_finalize(stmt);
    return results;
}
