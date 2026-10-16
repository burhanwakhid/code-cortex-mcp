#include "scope.h"
#include <string.h>

CBMScope* cbm_scope_push(CBMArena* a, CBMScope* current) {
    CBMScope* scope = (CBMScope*)cbm_arena_alloc(a, sizeof(CBMScope));
    if (!scope) {
        return current;
    }
    memset(scope, 0, sizeof(CBMScope));
    scope->parent = current;
    scope->arena = a;
    return scope;
}

CBMScope* cbm_scope_pop(CBMScope* scope) {
    if (!scope) {
        return NULL;
    }
    return scope->parent;
}

static CBMScopeChunk* alloc_chunk(CBMScope* scope) {
    if (!scope->arena) {
        return NULL;
    }
    CBMScopeChunk* c = (CBMScopeChunk*)cbm_arena_alloc(scope->arena, sizeof(CBMScopeChunk));
    if (!c) {
        return NULL;
    }
    memset(c, 0, sizeof(CBMScopeChunk));
    c->next = scope->chunks;
    scope->chunks = c;
    return c;
}

/* Returns false when the binding could NOT be recorded in THIS frame (arena
 * exhaustion in alloc_chunk). */
bool cbm_scope_bind_checked(CBMScope* scope, const char* name, const CBMType* type) {
    if (!scope || !name) {
        return false;
    }
    for (CBMScopeChunk* c = scope->chunks; c != NULL; c = c->next) {
        for (int i = 0; i < c->used; i++) {
            if (c->bindings[i].name && strcmp(c->bindings[i].name, name) == 0) {
                c->bindings[i].type = type;
                return true;
            }
        }
    }
    CBMScopeChunk* head = scope->chunks;
    if (!head || head->used >= CBM_SCOPE_CHUNK_BINDINGS) {
        head = alloc_chunk(scope);
        if (!head) {
            return false; /* arena exhausted: the shadow did NOT take effect */
        }
    }
    head->bindings[head->used].name = name;
    head->bindings[head->used].type = type;
    head->used++;
    return true;
}

void cbm_scope_bind(CBMScope* scope, const char* name, const CBMType* type) {
    (void)cbm_scope_bind_checked(scope, name, type);
}

const CBMType* cbm_scope_lookup(const CBMScope* scope, const char* name) {
    const CBMVarBinding* binding = cbm_scope_lookup_binding(scope, name);
    if (binding) {
        return binding->type;
    }
    return cbm_type_unknown();
}

bool cbm_scope_contains(const CBMScope* scope, const char* name) {
    return cbm_scope_lookup_binding(scope, name) != NULL;
}
