#pragma once
#include <string>
#include <vector>

namespace cbm { namespace knowledge {

struct SourceRef { std::string file_path; int line_start=0; int line_end=0; double confidence=0; bool is_static_fact=false; };
struct Item { int id=0; std::string type; std::string title; std::string scope; std::string owner; std::string version; std::string status; std::vector<SourceRef> sources; };

class KnowledgeStore {
public:
    explicit KnowledgeStore(const std::string& db_path);
    ~KnowledgeStore();
    bool ok() const;
    int add_item(const Item& item);
    std::vector<Item> get_implementation_guide(const std::string& intent) const;
    std::vector<Item> search_approved_examples(const std::string& intent) const;
    bool validate_recipe(int id);
    bool set_status(int id, const std::string& status, const std::string& note);
    void* db_ = nullptr;
};

} } // namespaces