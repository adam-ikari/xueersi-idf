#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Register render + physics native functions with the WAMR runtime. */
bool render_api_register(void);

#ifdef __cplusplus
}
#endif