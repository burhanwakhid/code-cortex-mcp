#include "flutter/adapter_c.h"
#include "flutter/adapter.h"
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>

extern "C" char* cbm_flutter_analyze_json(const char* repo_root) {
    if (!repo_root) return nullptr;
    auto g = cbm::flutter::analyze(repo_root);
    std::ostringstream ss;
    ss << "{\"packages\":[";
    bool first = true;
    for (auto& p : g.packages) {
        if (!first) ss << ",";
        first = false;
        ss << "{\"name\":\"" << p.name << "\",\"path\":\"" << p.path << "\",\"kind\":\"" << p.kind << "\",\"dart_files\":" << p.dart_files << ",\"signals\":[";
        for (size_t i=0;i<p.signals.size();++i){ if(i)ss<<","; ss<<"\""<<p.signals[i]<<"\""; }
        ss << "]}";
    }
    ss << "],\"dependencies\":[";
    first = true;
    for (auto& e : g.dependencies) {
        if (!first) ss << ",";
        first = false;
        ss << "{\"from\":\"" << e.from_pkg << "\",\"to\":\"" << e.to_pkg << "\",\"path_dep\":" << (e.path_dep?"true":"false") << ",\"target_path\":\"" << e.target_path << "\"}";
    }
    ss << "],\"generated_files\":[";
    first = true;
    for (auto& f : g.generated_files) { if(!first) ss<<","; first=false; ss<<"\""<<f<<"\""; }
    ss << "],\"violations\":[";
    first = true;
    for (auto& v : g.violations) { if(!first) ss<<","; first=false; ss<<"\""<<v<<"\""; }
    ss << "],\"workspace_members\":[";
    first = true;
    for (auto& m : g.workspace_members) { if(!first) ss<<","; first=false; ss<<"\""<<m<<"\""; }
    ss << "]}";
    std::string s = ss.str();
    char* out = (char*)std::malloc(s.size()+1);
    if (out) { std::memcpy(out, s.c_str(), s.size()+1); }
    return out;
}

extern "C" char* cbm_flutter_feature_context_json(const char* repo_root, const char* feature) {
    if (!repo_root || !feature) return nullptr;
    auto g = cbm::flutter::analyze(repo_root);
    std::ostringstream ss;
    // find package
    const cbm::flutter::PackageInfo* pkg = nullptr;
    for (auto& p : g.packages) if (p.name == feature) { pkg = &p; break; }
    ss << "{";
    if (pkg) {
        ss << "\"name\":\"" << pkg->name << "\",\"kind\":\"" << pkg->kind << "\",\"path\":\"" << pkg->path << "\",\"dart_files\":" << pkg->dart_files << ",\"signals\":[";
        for (size_t i=0;i<pkg->signals.size();++i){ if(i)ss<<","; ss<<"\""<<pkg->signals[i]<<"\""; }
        ss << "],";
    } else {
        ss << "\"name\":\"" << feature << "\",\"found\":false,";
    }
    ss << "\"dependencies\":[";
    bool first=true;
    for (auto& e : g.dependencies) {
        if (e.from_pkg != feature) continue;
        if (!first) ss << ",";
        first=false;
        ss << "{\"to\":\"" << e.to_pkg << "\",\"path_dep\":" << (e.path_dep?"true":"false") << ",\"target_path\":\"" << e.target_path << "\"}";
    }
    ss << "],\"violations\":[";
    first=true;
    for (auto& v : g.violations) {
        if (v.find(feature) == std::string::npos) continue;
        if (!first) ss << ",";
        first=false;
        ss << "\"" << v << "\"";
    }
    ss << "]}";
    std::string s = ss.str();
    char* out = (char*)std::malloc(s.size()+1);
    if (out) std::memcpy(out, s.c_str(), s.size()+1);
    return out;
}

extern "C" char* cbm_flutter_change_impact_json(const char* repo_root, const char* package) {
    if (!repo_root || !package) return nullptr;
    auto g = cbm::flutter::analyze(repo_root);
    std::ostringstream ss;
    ss << "{\"package\":\"" << package << "\",\"dependents\":[";
    bool first=true;
    for (auto& e : g.dependencies) {
        if (e.to_pkg != package) continue;
        if (!first) ss << ",";
        first=false;
        ss << "\"" << e.from_pkg << "\"";
    }
    ss << "],\"violations\":[";
    first=true;
    for (auto& v : g.violations) {
        if (v.find(package) == std::string::npos) continue;
        if (!first) ss << ",";
        first=false;
        ss << "\"" << v << "\"";
    }
    ss << "]}";
    std::string s = ss.str();
    char* out = (char*)std::malloc(s.size()+1);
    if (out) std::memcpy(out, s.c_str(), s.size()+1);
    return out;
}


extern "C" void cbm_flutter_free(char* p) { std::free(p); }
