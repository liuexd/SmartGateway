#include "r3c_binding.h"
#include <stdio.h>
#include <string.h>

static int checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); } } while (0)

int main(void)
{
    r3c_binding_t expected = {0}, got;
    memset(expected.transaction_id, 'a', 64);
    memset(expected.package_sha256, 'b', 64);
    memset(expected.image_sha256, 'c', 64);
    memcpy(expected.boot_id, "a1234567-1234-1234-1234-123456789abc", 37);
    expected.target_slot = 1;
    expected.last_good_slot = 2;
    expected.bootcount = 1;
    got = expected;
    CHECK(r3c_binding_matches(&expected, &got, 100, 105, 5) == 0);
    CHECK(r3c_binding_matches(&expected, &got, 100, 106, 5) != 0);
    CHECK(r3c_binding_matches(&expected, &got, 100, 99, 5) != 0);
    CHECK(r3c_binding_matches(&expected, &got, 100, 100, 0) != 0);
    got.transaction_id[0] = 'd';
    CHECK(r3c_binding_matches(&expected, &got, 100, 100, 5) != 0);
    got = expected; got.package_sha256[0] = 'd';
    CHECK(r3c_binding_matches(&expected, &got, 100, 100, 5) != 0);
    got = expected; got.image_sha256[0] = 'd';
    CHECK(r3c_binding_matches(&expected, &got, 100, 100, 5) != 0);
    got = expected; got.boot_id[0] = 'd';
    CHECK(r3c_binding_matches(&expected, &got, 100, 100, 5) != 0);
    got = expected; got.bootcount = 2;
    CHECK(r3c_binding_matches(&expected, &got, 100, 100, 5) != 0);
    got = expected; got.target_slot = 2;
    CHECK(r3c_binding_matches(&expected, &got, 100, 100, 5) != 0);
    got = expected; got.last_good_slot = 1;
    CHECK(r3c_binding_matches(&expected, &got, 100, 100, 5) != 0);
    got = expected; got.transaction_id[0] = 'A';
    CHECK(r3c_binding_matches(&expected, &got, 100, 100, 5) != 0);
    got = expected; memset(got.transaction_id, '0', 64);
    CHECK(r3c_binding_matches(&expected, &got, 100, 100, 5) != 0);
    got = expected; memset(got.boot_id, '0', 36);
    CHECK(r3c_binding_matches(&expected, &got, 100, 100, 5) != 0);
    got = expected; memset(got.image_sha256, 'c', 65);
    CHECK(r3c_binding_matches(&expected, &got, 100, 100, 5) != 0);
    CHECK(r3c_binding_matches(NULL, &expected, 100, 100, 5) != 0);
    printf("r3c_binding_test: checks=%d failures=%d\n", checks, failures);
    return failures != 0;
}
