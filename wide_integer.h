#ifndef OCEAN_WIDE_INTEGER_H
#define OCEAN_WIDE_INTEGER_H
#include "token.h"
#include <stdio.h>
#include <stdlib.h>

// Convert source digits to LLVM's exact unsigned hexadecimal literal spelling.
// Limbs are temporary; the returned spelling belongs to the token storage.
static inline char *wideIntegerLiteral(const char *digits, size_t length, unsigned base,
                                       unsigned *bits, int *powerOfTwo) {
    // Power-of-two bases can be packed directly, even at LLVM's maximum width.
    if (base != 10) {
        while (length && *digits == '0') { digits++; length--; }
        unsigned digitBits = base == 16 ? 4 : base == 8 ? 3 : 1;
        unsigned char first = length ? (unsigned char)*digits : '0';
        unsigned top = first <= '9' ? first - '0' : (first | 32) - 'a' + 10;
        unsigned topBits = 0; for (unsigned value = top; value; value >>= 1) topBits++;
        if (length && length - 1 > (OCEAN_MAX_INTEGER_BITS - topBits) / digitBits) return NULL;
        *bits = length ? (unsigned)((length - 1) * digitBits + topBits) : 0;
        *powerOfTwo = top && !(top & (top - 1));
        size_t nibbles = *bits ? (*bits + 3) / 4 : 1;
        char *result = malloc(nibbles + 4);
        if (!result) exit(1);
        result[0] = 'u'; result[1] = '0'; result[2] = 'x';
        unsigned pending = 0, pendingBits = 0;
        size_t output = nibbles + 3;
        result[output] = 0;
        for (size_t i = length; i; i--) {
            unsigned char c = digits[i - 1];
            unsigned digit = c <= '9' ? c - '0' : (c | 32) - 'a' + 10;
            if (i != 1 && digit) *powerOfTwo = 0;
            pending |= digit << pendingBits; pendingBits += digitBits;
            while (pendingBits >= 4 && output > 3) {
                result[--output] = "0123456789abcdef"[pending & 15];
                pending >>= 4; pendingBits -= 4;
            }
        }
        if (output > 3) result[--output] = "0123456789abcdef"[pending & 15];
        return result;
    }
    size_t capacity = 4, count = 1;
    uint32_t *words = calloc(capacity, sizeof(*words));
    if (!words) exit(1);
    for (size_t i = 0; i < length; i++) {
        unsigned char c = digits[i];
        unsigned digit = c <= '9' ? c - '0' : (c | 32) - 'a' + 10;
        uint64_t carry = digit;
        for (size_t j = 0; j < count; j++) {
            uint64_t value = (uint64_t)words[j] * base + carry;
            words[j] = (uint32_t)value; carry = value >> 32;
        }
        if (carry) {
            if (count == OCEAN_MAX_INTEGER_BITS / 32) { free(words); return NULL; }
            if (count == capacity) {
                capacity *= 2;
                uint32_t *grown = realloc(words, capacity * sizeof(*words));
                if (!grown) exit(1);
                words = grown;
            }
            words[count++] = (uint32_t)carry;
        }
    }
    unsigned topBits = 0; uint32_t top = words[count - 1];
    for (uint32_t value = top; value; value >>= 1) topBits++;
    *bits = (unsigned)((count - 1) * 32 + topBits);
    *powerOfTwo = top && !(top & (top - 1));
    for (size_t i = 0; i + 1 < count; i++) if (words[i]) *powerOfTwo = 0;
    char *result = malloc(4 + count * 8);
    if (!result) exit(1);
    size_t cursor = (size_t)sprintf(result, "u0x%x", top);
    for (size_t i = count - 1; i; i--) cursor += (size_t)sprintf(result + cursor, "%08x", words[i - 1]);
    free(words); return result;
}
#endif
