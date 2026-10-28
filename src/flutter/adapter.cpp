#include "flutter/adapter.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <regex>

namespace fs = std::filesystem;
namespace cbm {
namespace flutter {

static bool ignored_path(const fs::path& p) {
    for (auto& part : p) {
        std::string s = part.string();
        if (s == ".dart_tool" || s == "build" || s == ".git" || s == "node_modules") return true;
    }
    std::string name = p.filename().string();
    if (name.ends_with(".g.dart") || name.ends_with(".freezed.dart") || name.ends_with(".gr.dart")) return true;
    return false;
}

static std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static std::string classify(const std::string& rel) {
    if (rel == "." || rel.empty()) return "root";
    if (rel.rfind("features/", 0) == 0) return "feature";
    if (rel.rfind("app/", 0) == 0) return "app";
    if (rel.rfind("packages/", 0) == 0) return "library";
    return "package";
}

struct Pubspec {
    std::string name;
    std::vector<std::string> workspace;
    std::vector<std::pair<std::string,std::string>> deps;
};

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static std::string strip_quotes(const std::string& s) {
    std::string v = trim(s);
    if (!v.empty() && (v.front()=='"'||v.front()=='\'')) v = v.substr(1);
    if (!v.empty() && (v.back()=='"'||v.back()=='\'')) v.pop_back();
    return v;
}

static Pubspec parse_pubspec(const std::string& text) {
    Pubspec p;
    std::istringstream in(text);
    std::string line;
    bool in_ws = false, in_deps = false, in_dev = false;
    std::string cur_dep;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string trimmed = trim(line);
        // indent = leading spaces count
        size_t indent = line.find_first_not_of(" \t");
        if (indent == std::string::npos) indent = line.size();
        if (trimmed.empty() || trimmed[0] == '#') continue;

        if (trimmed == "workspace:") { in_ws = true; in_deps = false; in_dev = false; continue; }
        if (trimmed == "dependencies:" || trimmed == "dev_dependencies:") {
            in_deps = (trimmed == "dependencies:");
            in_dev = (trimmed == "dev_dependencies:");
            in_ws = false;
            continue;
        }
        if (indent == 0 && trimmed.find(':') != std::string::npos) {
            in_ws = false; in_deps = false; in_dev = false;
        }
        if (trimmed.rfind("name:", 0) == 0) {
            p.name = strip_quotes(trimmed.substr(5));
            continue;
        }
        if (in_ws && trimmed.rfind("- ", 0) == 0) {
            p.workspace.push_back(strip_quotes(trimmed.substr(2)));
            continue;
        }
        if ((in_deps || in_dev) && indent == 2 && trimmed.find(':') != std::string::npos) {
            size_t colon = trimmed.find(':');
            std::string key = trim(trimmed.substr(0, colon));
            std::string val = strip_quotes(trimmed.substr(colon + 1));
            cur_dep = key;
            p.deps.push_back({key, val});
            continue;
        }
        if ((in_deps || in_dev) && indent >= 4 && !cur_dep.empty() && trimmed.rfind("path:", 0) == 0) {
            std::string val = strip_quotes(trimmed.substr(5));
            if (!p.deps.empty() && p.deps.back().first == cur_dep) {
                p.deps.back().second = val;
            }
            continue;
        }
    }
    return p;
}

MonorepoGraph analyze(const std::string& repo_root) {
    MonorepoGraph g;
    fs::path root(repo_root);

    std::vector<fs::path> pubspecs;
    for (auto& entry : fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied)) {
        if (!entry.is_regular_file()) continue;
        if (ignored_path(entry.path())) continue;
        if (entry.path().filename() == "pubspec.yaml") pubspecs.push_back(entry.path());
    }

    std::map<std::string,std::string> name_to_rel;
    struct Pkg { std::string name; std::string rel; Pubspec ps; };
    std::vector<Pkg> pkgs;
    for (auto& ps_path : pubspecs) {
        std::string rel = fs::relative(ps_path, root).parent_path().string();
        if (rel == ".") rel = "";
        if (rel.empty()) rel = ".";
        std::string text = read_file(ps_path);
        Pubspec ps = parse_pubspec(text);
        if (ps.name.empty()) continue;
        pkgs.push_back({ps.name, rel, ps});
        name_to_rel[ps.name] = rel;
    }

    for (auto& pk : pkgs) {
        if (pk.rel == ".") {
            for (auto& m : pk.ps.workspace) g.workspace_members.push_back(m);
        }
    }

    for (auto& pk : pkgs) {
        PackageInfo info;
        info.name = pk.name;
        info.path = pk.rel;
        info.kind = classify(pk.rel);
        fs::path dir = (pk.rel == "." ? root : root / pk.rel);
        int df = 0;
        std::set<std::string> sigset;
        if (fs::exists(dir)) {
            for (auto& e : fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied)) {
                if (!e.is_regular_file()) continue;
                if (ignored_path(e.path())) continue;
                if (e.path().extension() == ".dart") {
                    df++;
                    std::string txt = read_file(e.path());
                    if (txt.find("flutter_bloc") != std::string::npos || txt.find("riverpod") != std::string::npos || txt.find("ChangeNotifier") != std::string::npos) sigset.insert("state_management");
                    if (txt.find("get_it") != std::string::npos || txt.find("injectable") != std::string::npos) sigset.insert("di");
                    if (txt.find("go_router") != std::string::npos || txt.find("auto_route") != std::string::npos) sigset.insert("navigation");
                    if (txt.find("shared_preferences") != std::string::npos || txt.find("hive") != std::string::npos) sigset.insert("storage");
                    if (txt.find("dio") != std::string::npos) sigset.insert("network");
                }
            }
        }
        info.dart_files = df;
        info.signals.assign(sigset.begin(), sigset.end());
        g.packages.push_back(info);

        for (auto& dep : pk.ps.deps) {
            std::string target_rel;
            if (!dep.second.empty()) {
                fs::path base = (pk.rel == "." ? root : root / pk.rel);
                std::error_code ec;
                fs::path t = fs::weakly_canonical(base / dep.second, ec);
                if (!ec && fs::exists(t)) target_rel = fs::relative(t, root).string();
            } else if (name_to_rel.count(dep.first)) {
                target_rel = name_to_rel[dep.first];
            }
            DependencyEdge e;
            e.from_pkg = pk.name;
            e.to_pkg = dep.first;
            e.path_dep = !dep.second.empty();
            e.target_path = target_rel;
            g.dependencies.push_back(e);
        }
    }

    for (auto& entry : fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied)) {
        if (!entry.is_regular_file()) continue;
        std::string name = entry.path().filename().string();
        if (name.ends_with(".g.dart") || name.ends_with(".freezed.dart")) {
            g.generated_files.push_back(fs::relative(entry.path(), root).string());
        }
    }

    std::map<std::string,std::string> kind_of;
    for (auto& p : g.packages) kind_of[p.name] = p.kind;
    for (auto& e : g.dependencies) {
        std::string sk = kind_of.count(e.from_pkg) ? kind_of[e.from_pkg] : "";
        std::string dk = kind_of.count(e.to_pkg) ? kind_of[e.to_pkg] : (e.target_path.empty() ? "external" : "unknown");
        if (sk == "feature" && dk == "feature") {
            g.violations.push_back("no-feature-to-feature: " + e.from_pkg + " -> " + e.to_pkg);
        }
        if (sk == "app" && e.to_pkg.find("storage") != std::string::npos) {
            g.violations.push_back("app-no-direct-storage (warn): " + e.from_pkg + " -> " + e.to_pkg);
        }
    }

    return g;
}

} // namespace flutter
} // namespace cbm