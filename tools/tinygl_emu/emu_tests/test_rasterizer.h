#ifndef _TEST_RASTERIZER_H_
#define _TEST_RASTERIZER_H_

// 测试结果
typedef struct {
    const char* name;
    int passed;      // 1 = pass, 0 = fail
    int diff_pixels; // 差异像素数（或错误计数）
    const char* error_msg;
} test_result_t;

// 运行所有 rasterizer 测试
// 返回：通过测试的数量（不是总数），results 数组由调用者提供（大小 >= 16）
int test_rasterizer_run(test_result_t* results, int max_results);

#endif
