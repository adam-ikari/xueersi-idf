/* WASM test: Canvas 2D verification
 * Draws colored rectangles filling the 160x128 screen
 */

extern void canvas_begin_frame(void);
extern void canvas_end_frame(void);
extern void canvas_set_fill_style(int r, int g, int b, int a);
extern void canvas_fill_rect(float x, float y, float w, float h);
extern void canvas_set_stroke_style(int r, int g, int b, int a);
extern void canvas_stroke_rect(float x, float y, float w, float h);
extern void canvas_set_font(const char *font);
extern void canvas_fill_text(const char *text, float x, float y);
extern void canvas_translate(float dx, float dy);
extern void canvas_save(void);
extern void canvas_restore(void);

/* The entry point called by WAMR */
void draw(void) {
    /* Clear to black */
    canvas_begin_frame();

    /* Draw a red rectangle (top-left quadrant) */
    canvas_set_fill_style(255, 0, 0, 255);
    canvas_fill_rect(0, 0, 80, 64);

    /* Draw a green rectangle (top-right quadrant) */
    canvas_set_fill_style(0, 255, 0, 255);
    canvas_fill_rect(80, 0, 80, 64);

    /* Draw a blue rectangle (bottom-left quadrant) */
    canvas_set_fill_style(0, 0, 255, 255);
    canvas_fill_rect(0, 64, 80, 64);

    /* Draw a yellow rectangle (bottom-right quadrant) */
    canvas_set_fill_style(255, 255, 0, 255);
    canvas_fill_rect(80, 64, 80, 64);

    /* Draw a white stroke border around the whole screen */
    canvas_set_stroke_style(255, 255, 255, 255);
    canvas_stroke_rect(0, 0, 160, 128);

    /* Draw "OK" text in center */
    canvas_set_fill_style(255, 255, 255, 255);
    canvas_set_font("5x7");
    canvas_fill_text("OK", 72, 60);

    /* Flush to display */
    canvas_end_frame();
}