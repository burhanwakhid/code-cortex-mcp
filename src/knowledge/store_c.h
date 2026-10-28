#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
char* cbm_knowledge_get_implementation_guide_json(const char* db_path, const char* intent);
char* cbm_knowledge_search_approved_examples_json(const char* db_path, const char* intent);
int cbm_knowledge_validate_recipe(const char* db_path, int id);
void cbm_knowledge_free(char* p);
#ifdef __cplusplus
}
#endif