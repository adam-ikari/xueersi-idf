/**
 * Lock-free single-producer single-consumer render command queue.
 *
 * Core 0 pushes (game logic) → Core 1 drains (render).  Uses volatile
 * head/tail with a compiler barrier — Xtensa LX6 guarantees word-aligned
 * 32-bit stores are single-copy atomic.
 */
#include "render_queue.h"
#include <string.h>

static render_cmd_t s_queue[RENDER_QUEUE_SIZE];
static volatile int s_head = 0;  /* producer (core 0) writes here */
static volatile int s_tail = 0;  /* consumer (core 1) reads here */

bool render_queue_push(render_cmd_t cmd)
{
    int next = (s_head + 1) % RENDER_QUEUE_SIZE;
    if (next == s_tail) return false;  /* full */

    s_queue[s_head] = cmd;
    __sync_synchronize();   /* ensure write of cmd before head update */
    s_head = next;
    return true;
}

int render_queue_drain(void (*draw_fn)(const render_cmd_t *cmd))
{
    int count = 0;
    while (s_tail != s_head) {
        __sync_synchronize();   /* ensure cmd read after tail check */
        render_cmd_t cmd = s_queue[s_tail];  /* copy out */
        s_tail = (s_tail + 1) % RENDER_QUEUE_SIZE;
        draw_fn(&cmd);
        count++;
    }
    return count;
}