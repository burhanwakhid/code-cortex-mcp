#include "knowledge/store_c.h"
#include "knowledge/store.h"
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>

extern "C" char* cbm_knowledge_get_implementation_guide_json(const char* db_path, const char* intent) {
    if (!db_path || !intent) return nullptr;
    cbm::knowledge::KnowledgeStore ks(db_path);
    auto items = ks.get_implementation_guide(intent);
    std::ostringstream ss;
    ss << "[";
    bool first = true;
    for (auto& it : items) {
        if (!first) ss << ",";
        first = false;
        ss << "{\"id\":" << it.id << ",\"type\":\"" << it.type << "\",\"title\":\"" << it.title << "\",\"status\":\"" << it.status << "\",\"sources\":[";
        bool sfirst = true;
        for (auto& s : it.sources) {
            if (!sfirst) ss << ",";
            sfirst = false;
            ss << "{\"file_path\":\"" << s.file_path << "\",\"line_start\":" << s.line_start << ",\"line_end\":" << s.line_end << ",\"confidence\":" << s.confidence << ",\"is_static_fact\":" << (s.is_static_fact?"true":"false") << "}";
        }
        ss << "]}";
    }
    ss << "]";
    std::string s = ss.str();
    char* out = (char*)std::malloc(s.size()+1);
    if (out) std::memcpy(out, s.c_str(), s.size()+1);
    return out;
}

extern "C" char* cbm_knowledge_search_approved_examples_json(const char* db_path, const char* intent) {
    if (!db_path || !intent) return nullptr;
    cbm::knowledge::KnowledgeStore ks(db_path);
    auto items = ks.search_approved_examples(intent);
    std::ostringstream ss;
    ss << "[";
    bool first = true;
    for (auto& it : items) {
        if (!first) ss << ",";
        first = false;
        ss << "{\"id\":" << it.id << ",\"title\":\"" << it.title << "\",\"status\":\"" << it.status << "\"}";
    }
    ss << "]";
    std::string s = ss.str();
    char* out = (char*)std::malloc(s.size()+1);
    if (out) std::memcpy(out, s.c_str(), s.size()+1);
    return out;
}
extern "C" int cbm_knowledge_validate_recipe(const char* db_path, int id) {
    if (!db_path) return 0;
    cbm::knowledge::KnowledgeStore ks(db_path);
    return ks.validate_recipe(id) ? 1 : 0;
}

extern "C" void cbm_knowledge_free(char* p) { std::free(p); }