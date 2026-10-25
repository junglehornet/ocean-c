#include "../c-hashmap/map.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct iteration { unsigned next, limit; int result; };
static int visit(const void *key, size_t length, uintptr_t value, void *user) {
    struct iteration *state = user;
    unsigned number;
    assert(length == sizeof(number));
    memcpy(&number, key, sizeof(number));
    assert(number == state->next && value == number * 10);
    state->next++;
    return state->next == state->limit ? -1 : state->result;
}
static const void *replacedKey;
static uintptr_t replacedValue;
static int replacements;
static int replaced(const void *key, size_t size, uintptr_t value, void *user) {
    assert(key == replacedKey && size == 3 && value == replacedValue && user == &replacements);
    replacements++;
    return 0;
}
int main(void) {
    unsigned keys[128];
    for (unsigned i = 0; i < 128; i++) keys[i] = i;
    // Grow through each insertion API and verify order survives rehashing.
    for (int mode = 0; mode < 3; mode++) {
        hashmap *map = hashmap_create();
        assert(map && hashmap_size(map) == 0);
        struct iteration order = {0};
        assert(hashmap_iterate(map, visit, &order) == 0 && order.next == 0);
        for (unsigned i = 0; i < 128; i++) {
            uintptr_t value = i * 10;
            int result = mode == 0 ? hashmap_set(map, &keys[i], sizeof(keys[i]), value) :
                mode == 1 ? hashmap_get_set(map, &keys[i], sizeof(keys[i]), &value) :
                hashmap_set_free(map, &keys[i], sizeof(keys[i]), value, replaced, &replacements);
            assert(result == 0 && value == i * 10);
        }
        assert(hashmap_size(map) == 128 && replacements == 0);
        for (unsigned i = 0; i < 128; i++) {
            uintptr_t value = 999;
            assert(hashmap_get(map, &keys[i], sizeof(keys[i]), &value) == 1 && value == i * 10);
        }
        order = (struct iteration){.result = 7};
        assert(hashmap_iterate(map, visit, &order) == 7 && order.next == 128);
        order = (struct iteration){.limit = 3};
        assert(hashmap_iterate(map, visit, &order) == -1 && order.next == 3);
        uintptr_t value = 999;
        assert(hashmap_get_set(map, &keys[5], sizeof(keys[5]), &value) == 1 && value == 50);
        assert(hashmap_set(map, &keys[5], sizeof(keys[5]), 123) == 0 && hashmap_size(map) == 128);
        hashmap_clear(map);
        assert(hashmap_size(map) == 0);
        assert(hashmap_get(map, &keys[5], sizeof(keys[5]), &value) == 0 && value == 0);
        assert(hashmap_set(map, &keys[5], sizeof(keys[5]), 42) == 0);
        assert(hashmap_get(map, &keys[5], sizeof(keys[5]), &value) == 1 && value == 42);
        hashmap_free(map);
    }
    hashmap *map = hashmap_create();
    char oldKey[] = "key", newKey[] = "key";
    assert(hashmap_set_free(map, oldKey, 3, 10, replaced, &replacements) == 0);
    replacedKey = oldKey; replacedValue = 10;
    assert(hashmap_set_free(map, newKey, 3, 20, replaced, &replacements) == 0 && replacements == 1);
    replacedKey = newKey; replacedValue = 20;
    assert(hashmap_set_free(map, oldKey, 3, 30, replaced, &replacements) == 0 && replacements == 2);
    // Binary keys and zero-length keys do not rely on C-string terminators.
    unsigned char bytes[24];
    for (size_t i = 0; i < sizeof(bytes); i++) bytes[i] = (unsigned char)i;
    for (size_t length = 0; length < sizeof(bytes); length++)
        assert(hashmap_set(map, bytes, length, length + 100) == 0);
    for (size_t length = 0; length < sizeof(bytes); length++) {
        uintptr_t value;
        assert(hashmap_get(map, bytes, length, &value) == 1 && value == length + 100);
    }
    hashmap_free(map);
    puts("hashmap insertion, replacement, resize, iteration, and binary-key checks passed");
}
