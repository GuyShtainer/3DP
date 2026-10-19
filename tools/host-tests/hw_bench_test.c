/* hw_bench_test.c -- bet 2.7a: the bench.txt parser, the counter and SHA-256 (NIST vectors). */
#include <stdio.h>
#include <string.h>

#include "hw_bench.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static void sha_hex(const void *d, size_t n, char hex[65])
{
    HwSha256 c;
    uint8_t out[32];

    hwsha256_init(&c);
    hwsha256_update(&c, d, n);
    hwsha256_final(&c, out);
    hwsha256_hex(out, hex);
}

int main(void)
{
    HwBenchEntry e[HWBENCH_MAX];
    uint32_t bad = 99;
    char hex[65], buf[16], big[1000];
    int n;
    static const char good[] = "# the session\nA render_core=2 hwcheck=1\n\n  B render_core=0   clock=frame \r\nC-3\n";
    HwSha256 c;
    uint8_t d1[32], d2[32];
    size_t i;

    n = hwbench_parse(e, HWBENCH_MAX, good, sizeof good - 1u, &bad);
    CHECK(n == 3 && bad == 0u, "good file: %d entries, bad %u", n, (unsigned)bad);
    CHECK(strcmp(e[0].tag, "A") == 0 && strcmp(e[0].keys, "render_core = 2\nhwcheck = 1\n") == 0, "entry A: '%s'", e[0].keys);
    CHECK(strcmp(e[1].tag, "B") == 0 && strcmp(e[1].keys, "render_core = 0\nclock = frame\n") == 0, "entry B: '%s'", e[1].keys);
    CHECK(strcmp(e[2].tag, "C-3") == 0 && e[2].keys[0] == '\0', "entry C-3 has no overrides");

    CHECK(hwbench_parse(e, HWBENCH_MAX, "A x\n", 4, &bad) == -1 && bad == 1u, "a token without = is refused (line %u)", (unsigned)bad);
    CHECK(hwbench_parse(e, HWBENCH_MAX, "A k=1\nA k=2\n", 12, &bad) == -1 && bad == 2u, "a repeated tag is refused (line %u)", (unsigned)bad);
    CHECK(hwbench_parse(e, HWBENCH_MAX, "A k=1\nB k=\n", 11, &bad) == -1 && bad == 2u, "an empty value is refused (line %u)", (unsigned)bad);
    CHECK(hwbench_parse(e, HWBENCH_MAX, "A Key=1\n", 8, &bad) == -1 && bad == 1u, "an upper-case key is refused");
    CHECK(hwbench_parse(e, HWBENCH_MAX, "ABCDEFGHIJKLMNOP k=1\n", 21, &bad) == -1, "a 16-character tag is refused");
    CHECK(hwbench_parse(e, 2, "A\nB\nC\n", 6, &bad) == -1 && bad == 3u, "a third entry over max 2 is refused (line %u)", (unsigned)bad);
    memset(big, 0, sizeof big);
    strcpy(big, "A ");
    for (i = 0; i < 20; i++) {
        strcat(big, "render_core=2 ");
    }
    CHECK(hwbench_parse(e, HWBENCH_MAX, big, strlen(big), &bad) == -1, "overrides that do not fit are refused");
    CHECK(hwbench_parse(e, HWBENCH_MAX, "", 0, &bad) == 0, "an empty file has no entries");

    CHECK(hwbench_counter_parse("7\n", 2) == 7u && hwbench_counter_parse("", 0) == 0u && hwbench_counter_parse("x", 1) == 0u && hwbench_counter_parse("  12", 4) == 12u, "counter parse");
    CHECK(hwbench_counter_format(0, buf, sizeof buf) == 2u && strcmp(buf, "0\n") == 0 && hwbench_counter_format(1234, buf, sizeof buf) == 5u && strcmp(buf, "1234\n") == 0, "counter format");

    sha_hex("", 0, hex);
    CHECK(strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0, "sha256 of empty: %s", hex);
    sha_hex("abc", 3, hex);
    CHECK(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0, "sha256 of abc: %s", hex);
    sha_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, hex);
    CHECK(strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") == 0, "sha256 of the 56-byte vector: %s", hex);
    {   /* one million 'a', fed in odd pieces */
        static char a[1000];

        memset(a, 'a', sizeof a);
        hwsha256_init(&c);
        for (i = 0; i < 1000u; i++) {
            hwsha256_update(&c, a, i % 2u == 0u ? 1000u : 1000u);
        }
        hwsha256_final(&c, d1);
        hwsha256_hex(d1, hex);
        CHECK(strcmp(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") == 0, "sha256 of a million a: %s", hex);
    }
    hwsha256_init(&c);
    hwsha256_update(&c, "abc", 3);
    hwsha256_final(&c, d2);
    CHECK(hwsha256_matches(d2, "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD  platinum.sav\n", 82) == 1, "a sha256sum line matches (upper case, with a name)");
    CHECK(hwsha256_matches(d2, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", 64) == 1, "the bare hex matches");
    CHECK(hwsha256_matches(d2, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ae", 64) == 0, "a different last digit does not match");
    CHECK(hwsha256_matches(d2, "ba78", 4) == 0, "a short text does not match");
    CHECK(hwsha256_matches(d2, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015adX", 65) == 0, "trailing junk glued to the hex does not match");

    printf("hw_bench_test: %d checks, %d failures\n", checks, fails);
    return fails != 0 ? 1 : 0;
}
