/* hello.c — A simple test program */
#include <survival.h>

int main(void) {
    fb_print("Hello from TinyCC!\n", COLOR_GREEN);
    fb_print("This was compiled in memory.\n", COLOR_WHITE);

    fb_rect(100, 200, 200, 100, COLOR_BLUE);
    fb_rect(110, 210, 180, 80, COLOR_CYAN);

    fb_print("\nDrew a rectangle!\n", COLOR_YELLOW);
    return 0;
}
