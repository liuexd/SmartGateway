#include "r3c_binding.h"
#include <stddef.h>
#include <string.h>

static int hex_lower(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

static int digest_valid(const char value[65])
{
    size_t i;
    int nonzero = 0;
    if (strnlen(value, 65) != 64) return 0;
    for (i = 0; i < 64; ++i) {
        if (!hex_lower(value[i])) return 0;
        nonzero |= value[i] != '0';
    }
    return nonzero;
}

static int boot_id_valid(const char value[37])
{
    size_t i;
    int nonzero = 0;
    if (strnlen(value, 37) != 36) return 0;
    for (i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (value[i] != '-') return 0;
        } else {
            if (!hex_lower(value[i])) return 0;
            nonzero |= value[i] != '0';
        }
    }
    return nonzero;
}

static int valid(const r3c_binding_t *p)
{
    return p && digest_valid(p->transaction_id) &&
           digest_valid(p->package_sha256) && digest_valid(p->image_sha256) &&
           boot_id_valid(p->boot_id) && (p->target_slot == 1 || p->target_slot == 2) &&
           (p->last_good_slot == 1 || p->last_good_slot == 2) &&
           p->target_slot != p->last_good_slot && p->bootcount > 0;
}

int r3c_binding_matches(const r3c_binding_t *a, const r3c_binding_t *b,
                        uint64_t observed_ms, uint64_t now_ms, uint64_t max_age_ms)
{
    if (!valid(a) || !valid(b) || max_age_ms == 0 || now_ms < observed_ms ||
        now_ms - observed_ms > max_age_ms) return -1;
    return strcmp(a->transaction_id, b->transaction_id) == 0 &&
           strcmp(a->package_sha256, b->package_sha256) == 0 &&
           strcmp(a->image_sha256, b->image_sha256) == 0 &&
           strcmp(a->boot_id, b->boot_id) == 0 &&
           a->target_slot == b->target_slot &&
           a->last_good_slot == b->last_good_slot &&
           a->bootcount == b->bootcount ? 0 : -1;
}
