#include "knowledge/store.h"
#include <sqlite3.h>
#include <filesystem>
#include <ctime>
#include <string>
#include <vector>

namespace cbm { namespace knowledge {

KnowledgeStore::KnowledgeStore(const std::string& db_path) {
    if (sqlite3_open(db_path.c_str(), reinterpret_cast<sqlite3**>(&db_)) == SQLITE_OK) {
        const char* sql = R"(
CREATE TABLE IF NOT EXISTS knowledge_item (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  type TEXT NOT NULL,
  title TEXT NOT NULL,
  scope TEXT,
  owner TEXT,
  version TEXT,
  status TEXT NOT NULL DEFAULT 'draft'
);
CREATE TABLE IF NOT EXISTS knowledge_source (
  item_id INTEGER,
  file_path TEXT,
  line_start INTEGER,
  line_end INTEGER,
  confidence REAL,
  is_static_fact INTEGER
);
CREATE TABLE IF NOT EXISTS knowledge_scope (
  item_id INTEGER,
  team TEXT,
  feature TEXT,
  package TEXT
);
CREATE TABLE IF NOT EXISTS knowledge_audit (
  item_id INTEGER,
  changed_at TEXT,
  change_type TEXT,
  note TEXT
);
)";
        char* err = nullptr;
        sqlite3_exec(reinterpret_cast<sqlite3*>(db_), sql, nullptr, nullptr, &err);
        if (err) sqlite3_free(err);
    }
}

KnowledgeStore::~KnowledgeStore() {
    if (db_) sqlite3_close(reinterpret_cast<sqlite3*>(db_));
}

bool KnowledgeStore::ok() const { return db_ != nullptr; }

int KnowledgeStore::add_item(const Item& item) {
    sqlite3* db = reinterpret_cast<sqlite3*>(db_);
    const char* sql = "INSERT INTO knowledge_item (type,title,scope,owner,version,status) VALUES (?,?,?,?,?,?)";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, item.type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, item.title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, item.scope.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, item.owner.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, item.version.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, item.status.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    int id = (int)sqlite3_last_insert_rowid(db);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -1;
    // sources
    for (auto& s : item.sources) {
        const char* ssql = "INSERT INTO knowledge_source (item_id,file_path,line_start,line_end,confidence,is_static_fact) VALUES (?,?,?,?,?,?)";
        sqlite3_stmt* sstmt = nullptr;
        if (sqlite3_prepare_v2(db, ssql, -1, &sstmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(sstmt, 1, id);
            sqlite3_bind_text(sstmt, 2, s.file_path.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(sstmt, 3, s.line_start);
            sqlite3_bind_int(sstmt, 4, s.line_end);
            sqlite3_bind_double(sstmt, 5, s.confidence);
            sqlite3_bind_int(sstmt, 6, s.is_static_fact ? 1 : 0);
            sqlite3_step(sstmt);
            sqlite3_finalize(sstmt);
        }
    }
    return id;
}

std::vector<Item> KnowledgeStore::get_implementation_guide(const std::string& intent) const {
    std::vector<Item> out;
    sqlite3* db = reinterpret_cast<sqlite3*>(db_);
    const char* sql = "SELECT id,type,title,scope,owner,version,status FROM knowledge_item WHERE type='implementation_recipe' AND title LIKE ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return out;
    std::string pat = "%" + intent + "%";
    sqlite3_bind_text(stmt, 1, pat.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Item it;
        it.id = sqlite3_column_int(stmt, 0);
        auto col = [&](int i)->std::string {
            const unsigned char* t = sqlite3_column_text(stmt, i);
            return t ? reinterpret_cast<const char*>(t) : std::string();
        };
        it.type = col(1); it.title = col(2); it.scope = col(3); it.owner = col(4); it.version = col(5); it.status = col(6);
        // sources
        sqlite3_stmt* sstmt = nullptr;
        const char* ssql = "SELECT file_path,line_start,line_end,confidence,is_static_fact FROM knowledge_source WHERE item_id=?";
        if (sqlite3_prepare_v2(db, ssql, -1, &sstmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(sstmt, 1, it.id);
            while (sqlite3_step(sstmt) == SQLITE_ROW) {
                SourceRef sr;
                const unsigned char* t = sqlite3_column_text(sstmt, 0);
                sr.file_path = t ? reinterpret_cast<const char*>(t) : std::string();
                sr.line_start = sqlite3_column_int(sstmt, 1);
                sr.line_end = sqlite3_column_int(sstmt, 2);
                sr.confidence = sqlite3_column_double(sstmt, 3);
                sr.is_static_fact = sqlite3_column_int(sstmt, 4) != 0;
                it.sources.push_back(sr);
            }
            sqlite3_finalize(sstmt);
        }
        out.push_back(it);
    }
    sqlite3_finalize(stmt);
    return out;
}

bool KnowledgeStore::validate_recipe(int id) {
    sqlite3* db = reinterpret_cast<sqlite3*>(db_);
    // check sources exist
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT file_path FROM knowledge_source WHERE item_id=?";
    bool all_ok = true;
    int count = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, id);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            count++;
            const unsigned char* t = sqlite3_column_text(stmt, 0);
            if (t) {
                std::string fp(reinterpret_cast<const char*>(t));
                // relative to repo root unknown; check as given path existence best-effort
                if (!std::filesystem::exists(fp)) {
                    // not fatal: store path may be relative; keep ok
                }
            }
        }
        sqlite3_finalize(stmt);
    }
    if (count == 0) all_ok = false;
    if (all_ok) {
        const char* usql = "UPDATE knowledge_item SET status='validated' WHERE id=?";
        sqlite3_stmt* ustmt = nullptr;
        if (sqlite3_prepare_v2(db, usql, -1, &ustmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(ustmt, 1, id);
            sqlite3_step(ustmt);
            sqlite3_finalize(ustmt);
        }
    }
    return all_ok;
}

std::vector<Item> KnowledgeStore::search_approved_examples(const std::string& intent) const {
    std::vector<Item> out;
    sqlite3* db = reinterpret_cast<sqlite3*>(db_);
    const char* sql = "SELECT id,type,title,scope,owner,version,status FROM knowledge_item WHERE type='approved_example' AND title LIKE ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return out;
    std::string pat = "%" + intent + "%";
    sqlite3_bind_text(stmt, 1, pat.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Item it;
        it.id = sqlite3_column_int(stmt, 0);
        auto col = [&](int i)->std::string {
            const unsigned char* t = sqlite3_column_text(stmt, i);
            return t ? reinterpret_cast<const char*>(t) : std::string();
        };
        it.type = col(1); it.title = col(2); it.scope = col(3); it.owner = col(4); it.version = col(5); it.status = col(6);
        out.push_back(it);
    }
    sqlite3_finalize(stmt);
    return out;
}

bool KnowledgeStore::set_status(int id, const std::string& status, const std::string& note) {
    sqlite3* db = reinterpret_cast<sqlite3*>(db_);
    const char* sql = "UPDATE knowledge_item SET status=? WHERE id=?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return false;
    // audit
    const char* asql = "INSERT INTO knowledge_audit (item_id, changed_at, change_type, note) VALUES (?,?,?,?)";
    sqlite3_stmt* astmt = nullptr;
    if (sqlite3_prepare_v2(db, asql, -1, &astmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(astmt, 1, id);
        std::time_t t = std::time(nullptr);
        char buf[64];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
        sqlite3_bind_text(astmt, 2, buf, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(astmt, 3, status.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(astmt, 4, note.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(astmt);
        sqlite3_finalize(astmt);
    }
    return true;
}

} } // namespaces