#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns a malloc'd JSON string (caller must free with cbm_flutter_free). */
char* cbm_flutter_analyze_json(const char* repo_root);
char* cbm_flutter_feature_context_json(const char* repo_root, const char* feature);
char* cbm_flutter_change_impact_json(const char* repo_root, const char* package);
void cbm_flutter_free(char* p);

#ifdef __cplusplus
}
#endif