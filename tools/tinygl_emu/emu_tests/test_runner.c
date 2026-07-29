#include <stdio.h>
#include <string.h>
#include "test_rasterizer.h"
#include "test_frame.h"

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    printf("========================================\n");
    printf("TinyGL Emulator Test Runner\n");
    printf("========================================\n\n");

    /* L1 unit tests */
    printf("--- L1 Rasterizer Tests ---\n");
    test_result_t l1_results[16];
    int l1_count = test_rasterizer_run(l1_results, 16);
    int l1_passed = 0;
    for (int i = 0; i < l1_count; i++) {
        printf("[%s] %s: diff=%d pixels\n",
               l1_results[i].passed ? "PASS" : "FAIL",
               l1_results[i].name,
               l1_results[i].diff_pixels);
        if (l1_results[i].passed) l1_passed++;
    }
    printf("L1: %d/%d passed\n\n", l1_passed, l1_count);

    /* L2 frame-level tests */
    printf("--- L2 Frame Tests ---\n");
    frame_test_result_t l2_results[16];
    int l2_count = test_frame_run(l2_results, 16);
    int l2_passed = 0;
    for (int i = 0; i < l2_count; i++) {
        printf("[%s] %s: diff=%d (%.2f%%) ref=%s\n",
               l2_results[i].passed ? "PASS" : "FAIL",
               l2_results[i].name,
               l2_results[i].diff_pixels,
               l2_results[i].diff_percent,
               l2_results[i].ref_path);
        if (l2_results[i].error_msg) {
            printf("      note: %s\n", l2_results[i].error_msg);
        }
        if (l2_results[i].passed) l2_passed++;
    }
    printf("L2: %d/%d passed\n\n", l2_passed, l2_count);

    /* Summary */
    int total_passed = l1_passed + l2_passed;
    int total = l1_count + l2_count;
    printf("========================================\n");
    printf("TOTAL: %d/%d passed\n", total_passed, total);
    printf("========================================\n");

    return (total_passed == total) ? 0 : 1;
}
