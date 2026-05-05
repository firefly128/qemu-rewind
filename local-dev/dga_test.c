/*
 * dga_test.c — Direct framebuffer access test for Solaris 7 TCX
 *
 * Tests the DGA-relevant hardware path: open /dev/fb, mmap the
 * 8-bit framebuffer, write pixels directly, and manipulate the
 * colormap via FBIOPUTCMAP.  This exercises exactly what Solaris
 * DGA (libdga.so) and SDL 1.2 use for fullscreen rendering.
 *
 * Build (cross):
 *   sparc-sun-solaris2.7-gcc -O2 -o dga_test dga_test.c
 *
 * Build (native on Solaris):
 *   gcc -O2 -o dga_test dga_test.c
 *
 * Run (as root on Solaris 7, from console or SSH):
 *   ./dga_test
 *
 * The test runs 5 phases, each ~2 seconds, then restores the
 * original colormap and exits.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/fbio.h>

/* Solaris fbio.h defines:
 *   FBIOGTYPE    — get framebuffer type/geometry
 *   FBIOPUTCMAP  — set colormap entries
 *   FBIOGETCMAP  — get colormap entries
 */

#define FB_DEVICE "/dev/fb"

static int fb_fd = -1;
static unsigned char *framebuffer = NULL;
static int fb_width, fb_height, fb_linebytes, fb_size;

/* Saved colormap for restore on exit */
static unsigned char saved_red[256];
static unsigned char saved_green[256];
static unsigned char saved_blue[256];
static int colormap_saved = 0;

static void restore_and_exit(void)
{
    if (colormap_saved && fb_fd >= 0) {
        struct fbcmap restore_cmap;
        restore_cmap.index = 0;
        restore_cmap.count = 256;
        restore_cmap.red   = saved_red;
        restore_cmap.green = saved_green;
        restore_cmap.blue  = saved_blue;
        ioctl(fb_fd, FBIOPUTCMAP, &restore_cmap);
    }
    if (framebuffer && framebuffer != MAP_FAILED) {
        munmap(framebuffer, fb_size);
    }
    if (fb_fd >= 0) {
        close(fb_fd);
    }
}

static int save_colormap(void)
{
    struct fbcmap get_cmap;
    get_cmap.index = 0;
    get_cmap.count = 256;
    get_cmap.red   = saved_red;
    get_cmap.green = saved_green;
    get_cmap.blue  = saved_blue;

    if (ioctl(fb_fd, FBIOGETCMAP, &get_cmap) < 0) {
        perror("FBIOGETCMAP");
        return -1;
    }
    colormap_saved = 1;
    return 0;
}

static int set_colormap_entry(int index, unsigned char red_value,
                              unsigned char green_value,
                              unsigned char blue_value)
{
    struct fbcmap put_cmap;
    unsigned char red_buf   = red_value;
    unsigned char green_buf = green_value;
    unsigned char blue_buf  = blue_value;

    put_cmap.index = index;
    put_cmap.count = 1;
    put_cmap.red   = &red_buf;
    put_cmap.green = &green_buf;
    put_cmap.blue  = &blue_buf;

    if (ioctl(fb_fd, FBIOPUTCMAP, &put_cmap) < 0) {
        perror("FBIOPUTCMAP");
        return -1;
    }
    return 0;
}

static int set_colormap_range(int start_index, int entry_count,
                              unsigned char *red_values,
                              unsigned char *green_values,
                              unsigned char *blue_values)
{
    struct fbcmap put_cmap;
    put_cmap.index = start_index;
    put_cmap.count = entry_count;
    put_cmap.red   = red_values;
    put_cmap.green = green_values;
    put_cmap.blue  = blue_values;

    if (ioctl(fb_fd, FBIOPUTCMAP, &put_cmap) < 0) {
        perror("FBIOPUTCMAP bulk");
        return -1;
    }
    return 0;
}

/*
 * Phase 1: Fill screen with solid color bands (horizontal stripes).
 * Tests basic direct VRAM write + dirty tracking.
 */
static void test_color_bands(void)
{
    int row, column;
    unsigned char color_index;

    printf("Phase 1: Color bands (direct VRAM write)...\n");

    /* Set up a simple 8-color palette in entries 1-8 */
    set_colormap_entry(1, 255,   0,   0);  /* red */
    set_colormap_entry(2,   0, 255,   0);  /* green */
    set_colormap_entry(3,   0,   0, 255);  /* blue */
    set_colormap_entry(4, 255, 255,   0);  /* yellow */
    set_colormap_entry(5, 255,   0, 255);  /* magenta */
    set_colormap_entry(6,   0, 255, 255);  /* cyan */
    set_colormap_entry(7, 255, 255, 255);  /* white */
    set_colormap_entry(8, 128, 128, 128);  /* gray */

    for (row = 0; row < fb_height; row++) {
        color_index = (row / (fb_height / 8)) + 1;
        if (color_index > 8) color_index = 8;
        memset(framebuffer + row * fb_linebytes, color_index, fb_width);
    }

    printf("  OK — 8 horizontal color bands visible?\n");
    sleep(2);
}

/*
 * Phase 2: Animated colormap cycling.
 * Writes a gradient to VRAM once, then rotates the palette.
 * Tests that FBIOPUTCMAP updates are reflected on screen.
 */
static void test_palette_animation(void)
{
    int row, column, frame;
    unsigned char red_palette[64], green_palette[64], blue_palette[64];

    printf("Phase 2: Palette animation (colormap cycling)...\n");

    /* Fill screen with a vertical gradient using palette entries 64-127 */
    for (row = 0; row < fb_height; row++) {
        unsigned char gradient_index = 64 + ((row * 64) / fb_height);
        if (gradient_index > 127) gradient_index = 127;
        memset(framebuffer + row * fb_linebytes, gradient_index, fb_width);
    }

    /* Cycle palette 30 times (~2 seconds at 60 fps) */
    for (frame = 0; frame < 60; frame++) {
        int palette_entry;
        for (palette_entry = 0; palette_entry < 64; palette_entry++) {
            int shifted = (palette_entry + frame * 4) % 256;
            red_palette[palette_entry]   = shifted;
            green_palette[palette_entry] = (shifted + 85) & 0xff;
            blue_palette[palette_entry]  = (shifted + 170) & 0xff;
        }
        set_colormap_range(64, 64, red_palette, green_palette, blue_palette);
        usleep(33333);  /* ~30 fps */
    }

    printf("  OK — smooth color cycling animation?\n");
}

/*
 * Phase 3: Pixel-by-pixel write test.
 * Draws a checkerboard pattern one pixel at a time.
 * Tests that individual byte writes to VRAM are tracked.
 */
static void test_pixel_writes(void)
{
    int row, column;

    printf("Phase 3: Pixel-by-pixel checkerboard...\n");

    set_colormap_entry(10, 200, 200, 200);  /* light gray */
    set_colormap_entry(11,  40,  40,  40);  /* dark gray */

    for (row = 0; row < fb_height; row++) {
        for (column = 0; column < fb_width; column++) {
            int checker = ((row / 32) + (column / 32)) & 1;
            framebuffer[row * fb_linebytes + column] = checker ? 11 : 10;
        }
    }

    printf("  OK — 32x32 checkerboard visible?\n");
    sleep(2);
}

/*
 * Phase 4: Bulk memset fill (simulates DGA bulk rendering).
 * Tests that large memset writes are fully detected.
 */
static void test_bulk_fill(void)
{
    int pass;
    unsigned char fill_colors[] = {1, 3, 5, 7, 2, 4, 6, 8};

    printf("Phase 4: Bulk fill (rapid full-screen memset)...\n");

    for (pass = 0; pass < 8; pass++) {
        int row;
        for (row = 0; row < fb_height; row++) {
            memset(framebuffer + row * fb_linebytes, fill_colors[pass],
                   fb_width);
        }
        usleep(250000);  /* 250ms per color */
    }

    printf("  OK — 8 rapid full-screen color flashes?\n");
}

/*
 * Phase 5: Bouncing rectangle (simulates game rendering).
 * Clears screen and moves a filled rectangle around.
 */
static void test_bouncing_rect(void)
{
    int rect_x = 100, rect_y = 100;
    int velocity_x = 5, velocity_y = 3;
    int rect_width = 80, rect_height = 60;
    int frame;

    printf("Phase 5: Bouncing rectangle animation...\n");

    set_colormap_entry(20,   0,   0,   0);  /* background: black */
    set_colormap_entry(21, 255, 128,   0);  /* rectangle: orange */

    for (frame = 0; frame < 120; frame++) {
        int row;

        /* Clear to background */
        for (row = 0; row < fb_height; row++) {
            memset(framebuffer + row * fb_linebytes, 20, fb_width);
        }

        /* Draw rectangle */
        for (row = rect_y; row < rect_y + rect_height && row < fb_height;
             row++) {
            int draw_width = rect_width;
            if (rect_x + draw_width > fb_width) {
                draw_width = fb_width - rect_x;
            }
            if (draw_width > 0 && rect_x >= 0) {
                memset(framebuffer + row * fb_linebytes + rect_x, 21,
                       draw_width);
            }
        }

        /* Move */
        rect_x += velocity_x;
        rect_y += velocity_y;
        if (rect_x + rect_width >= fb_width || rect_x <= 0) {
            velocity_x = -velocity_x;
        }
        if (rect_y + rect_height >= fb_height || rect_y <= 0) {
            velocity_y = -velocity_y;
        }
        if (rect_x < 0) rect_x = 0;
        if (rect_y < 0) rect_y = 0;

        usleep(16667);  /* ~60 fps */
    }

    printf("  OK — smooth bouncing rectangle?\n");
}

int main(int argc, char **argv)
{
    struct fbtype fb_type;

    printf("=== TCX DGA / Direct Framebuffer Test ===\n\n");

    /* Open framebuffer device */
    fb_fd = open(FB_DEVICE, O_RDWR);
    if (fb_fd < 0) {
        perror("open " FB_DEVICE);
        printf("Run as root, or check that %s exists.\n", FB_DEVICE);
        return 1;
    }

    /* Get framebuffer geometry */
    if (ioctl(fb_fd, FBIOGTYPE, &fb_type) < 0) {
        perror("FBIOGTYPE");
        close(fb_fd);
        return 1;
    }

    fb_width     = fb_type.fb_width;
    fb_height    = fb_type.fb_height;
    fb_linebytes = fb_type.fb_width;  /* TCX: linebytes == width for 8-bit */
    fb_size      = fb_type.fb_size;

    /* fb_size from FBIOGTYPE may be 0 for some drivers; use geometry */
    if (fb_size <= 0) {
        fb_size = fb_linebytes * fb_height;
    }

    printf("Framebuffer: %dx%d, type=%d, depth=%d, size=%d\n",
           fb_width, fb_height, fb_type.fb_type, fb_type.fb_depth, fb_size);

    /* Save current colormap for restore */
    if (save_colormap() < 0) {
        printf("Warning: could not save colormap, will not restore.\n");
    }

    /* Register cleanup */
    atexit(restore_and_exit);

    /* mmap the framebuffer — the core DGA operation */
    framebuffer = mmap(NULL, fb_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                       fb_fd, 0);
    if (framebuffer == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    printf("Framebuffer mapped at %p (%d bytes)\n\n", framebuffer, fb_size);

    /* Run test phases */
    test_color_bands();
    test_palette_animation();
    test_pixel_writes();
    test_bulk_fill();
    test_bouncing_rect();

    printf("\n=== All phases complete. Restoring colormap. ===\n");

    /* restore_and_exit runs via atexit */
    return 0;
}
