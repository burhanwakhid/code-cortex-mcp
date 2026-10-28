#pragma once

#include <string>
#include <vector>

namespace cbm {
namespace flutter {

struct PackageInfo {
    std::string name;
    std::string path;
    std::string kind; // app | feature | library | root
    int dart_files = 0;
    std::vector<std::string> signals;
};

struct DependencyEdge {
    std::string from_pkg;
    std::string to_pkg;
    bool path_dep = false;
    std::string target_path;
};

struct MonorepoGraph {
    std::vector<PackageInfo> packages;
    std::vector<DependencyEdge> dependencies;
    std::vector<std::string> workspace_members;
    std::vector<std::string> generated_files;
    std::vector<std::string> violations;
};

// Placeholder for future: parse pubspec.yaml workspace & deps, classify features,
// apply dependency rules, detect generated files. Returns empty graph for now.
MonorepoGraph analyze(const std::string& repo_root);

} // namespace flutter
} // namespace cbm