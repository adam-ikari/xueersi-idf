#ifndef _TEST_FRAME_H_
#define _TEST_FRAME_H_

typedef struct {
    const char* name;
    int passed;
    int diff_pixels;
    float diff_percent;
    const char* ref_path;
    const char* error_msg;
} frame_test_result_t;

int test_frame_run(frame_test_result_t* results, int max_results);

#endif
