/*
 * vnctestserver - a small headless VNC server used to exercise vncsixel.
 *
 * It renders an animated test pattern (colour bars, a gradient, a moving
 * box and a frame counter) into a framebuffer and serves it over RFB.
 * No X server required: libvncserver can run without one.
 *
 *   usage: vnctestserver [-p port] [-w width] [-h height] [-f fps]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <rfb/rfb.h>
#include <rfb/keysym.h>

static int g_width = 640;
static int g_height = 480;
static int g_port = 5900;
static int g_fps = 10;

static unsigned char g_fb[4096 * 4096 * 4];
static int g_frame = 0;
static int g_last_x = -1, g_last_y = -1;
static const char *dump_ppm_path = NULL;

/* 3x5 bitmap font for the digits and a few letters. */
static const unsigned char DIGIT_3X5[10][5] = {
    { 0x7, 0x5, 0x5, 0x5, 0x7 }, /* 0 */
    { 0x2, 0x6, 0x2, 0x2, 0x7 }, /* 1 */
    { 0x7, 0x1, 0x7, 0x4, 0x7 }, /* 2 */
    { 0x7, 0x1, 0x7, 0x1, 0x7 }, /* 3 */
    { 0x5, 0x5, 0x7, 0x1, 0x1 }, /* 4 */
    { 0x7, 0x4, 0x7, 0x1, 0x7 }, /* 5 */
    { 0x7, 0x4, 0x7, 0x5, 0x7 }, /* 6 */
    { 0x7, 0x1, 0x2, 0x2, 0x2 }, /* 7 */
    { 0x7, 0x5, 0x7, 0x5, 0x7 }, /* 8 */
    { 0x7, 0x5, 0x7, 0x1, 0x7 }  /* 9 */
};

static void put_pixel(unsigned char *fb, int stride, int x, int y,
                      int r, int g, int b)
{
    unsigned char *p;
    if (x < 0 || y < 0 || x >= g_width || y >= g_height) {
        return;
    }
    p = fb + (size_t)y * stride + (size_t)x * 4;
    p[0] = (unsigned char)r;
    p[1] = (unsigned char)g;
    p[2] = (unsigned char)b;
    p[3] = 0;
}

static void fill_rect(unsigned char *fb, int stride, int x, int y,
                      int w, int h, int r, int g, int b)
{
    int i, j;
    for (j = y; j < y + h; j++) {
        for (i = x; i < x + w; i++) {
            put_pixel(fb, stride, i, j, r, g, b);
        }
    }
}

static void draw_digit(unsigned char *fb, int stride, int x, int y,
                       int digit, int r, int g, int b)
{
    int row, col;
    if (digit < 0 || digit > 9) {
        return;
    }
    for (row = 0; row < 5; row++) {
        unsigned char bits = DIGIT_3X5[digit][row];
        for (col = 0; col < 3; col++) {
            if (bits & (1 << (2 - col))) {
                put_pixel(fb, stride, x + col, y + row, r, g, b);
            }
        }
    }
}

static void draw_number(unsigned char *fb, int stride, int x, int y,
                        int value, int r, int g, int b)
{
    char buf[16];
    int i, n = 0;

    if (value == 0) {
        buf[n++] = '0';
    }
    while (value > 0 && n < (int)sizeof(buf)) {
        buf[n++] = (char)('0' + value % 10);
        value /= 10;
    }
    for (i = 0; i < n; i++) {
        draw_digit(fb, stride, x + i * 4, y, buf[n - 1 - i] - '0', r, g, b);
    }
}

static void render_frame(void)
{
    unsigned char *fb = g_fb;
    int stride = g_width * 4;
    int x, y;

    /* Background: horizontal gradient. */
    for (y = 0; y < g_height; y++) {
        for (x = 0; x < g_width; x++) {
            put_pixel(fb, stride, x, y,
                      (x * 255) / (g_width > 1 ? g_width - 1 : 1),
                      (y * 255) / (g_height > 1 ? g_height - 1 : 1),
                      32);
        }
    }

    /* Colour bars along the top. */
    {
        static const int bars[8][3] = {
            { 255, 255, 255 }, { 255, 255, 0 }, { 0, 255, 255 },
            { 0, 255, 0 },     { 255, 0, 255 }, { 255, 0, 0 },
            { 0, 0, 255 },     { 0, 0, 0 }
        };
        int bw = g_width / 8;
        for (x = 0; x < 8; x++) {
            fill_rect(fb, stride, x * bw, 0, bw, 40,
                      bars[x][0], bars[x][1], bars[x][2]);
        }
    }

    /* A box that moves, so consecutive frames always differ. */
    {
        int bx = (g_frame * 7) % (g_width > 64 ? g_width - 64 : 1);
        int by = 60 + ((g_frame * 5) % (g_height > 128 ? g_height - 128 : 1));

        if (g_last_x >= 0) {
            fill_rect(fb, stride, g_last_x, g_last_y, 64, 64, 0, 0, 0);
        }
        fill_rect(fb, stride, bx, by, 64, 64, 255, 128, 0);
        g_last_x = bx;
        g_last_y = by;
    }

    /* Grid. */
    for (x = 0; x < g_width; x += 40) {
        fill_rect(fb, stride, x, 40, 1, g_height - 40, 90, 90, 90);
    }
    for (y = 40; y < g_height; y += 40) {
        fill_rect(fb, stride, 0, y, g_width, 1, 90, 90, 90);
    }

    /* Frame counter. */
    draw_number(fb, stride, 8, g_height - 24, g_frame, 255, 255, 255);
}

static void client_gone(rfbClientPtr cl)
{
    (void)cl;
}

static void key_event(rfbBool down, rfbKeySym keySym, rfbClientPtr cl)
{
    (void)cl;
    fprintf(stderr, "vnctestserver: key %s keysym=0x%x\n",
            down ? "down" : "up", (unsigned int)keySym);
}

static void ptr_event(int buttonMask, int x, int y, rfbClientPtr cl)
{
    (void)cl;
    fprintf(stderr, "vnctestserver: pointer mask=%d at (%d,%d)\n",
            buttonMask, x, y);
}

static enum rfbNewClientAction new_client(rfbClientPtr cl)
{
    cl->clientGoneHook = client_gone;
    return RFB_CLIENT_ACCEPT;
}

static void write_ppm(const char *path)
{
    FILE *f;
    int x, y;

    f = fopen(path, "wb");
    if (f == NULL) {
        fprintf(stderr, "vnctestserver: cannot write %s\n", path);
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", g_width, g_height);
    for (y = 0; y < g_height; y++) {
        for (x = 0; x < g_width; x++) {
            const unsigned char *p = g_fb + (size_t)y * g_width * 4 + (size_t)x * 4;
            fputc(p[0], f);
            fputc(p[1], f);
            fputc(p[2], f);
        }
    }
    fclose(f);
}

static void usage(void)
{
    fprintf(stderr,
        "usage: vnctestserver [-p port] [-w width] [-h height] [-f fps]\n");
}

int main(int argc, char **argv)
{
    rfbScreenInfoPtr screen;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            g_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            g_width = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-h") == 0 && i + 1 < argc) {
            g_height = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            g_fps = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-P") == 0 && i + 1 < argc) {
            dump_ppm_path = argv[++i];
        } else {
            usage();
            return 1;
        }
    }

    /*
     * We consumed our own options, so hand libvncserver a clean argv.
     * (rfbGetScreen() parses argv for -rfbport and friends and bails out
     * on anything it does not recognise.)
     */
    argc = 1;

    if (g_width <= 0 || g_height <= 0 ||
        (size_t)g_width * (size_t)g_height * 4 > sizeof(g_fb)) {
        fprintf(stderr, "vnctestserver: bad geometry\n");
        return 1;
    }

    screen = rfbGetScreen(&argc, argv, g_width, g_height, 8, 3, 4);
    if (screen == NULL) {
        fprintf(stderr, "vnctestserver: failed to create the screen\n");
        return 1;
    }
    screen->port = g_port;
    screen->ipv6port = g_port;
    screen->desktopName = "vncsixel test server";
    screen->newClientHook = new_client;
    screen->kbdAddEvent = key_event;
    screen->ptrAddEvent = ptr_event;

    rfbNewFramebuffer(screen, (char *)g_fb, g_width, g_height, 8, 3, 4);

    render_frame();

    if (dump_ppm_path != NULL) {
        write_ppm(dump_ppm_path);
        fprintf(stderr, "vnctestserver: wrote %s\n", dump_ppm_path);
        return 0;
    }

    rfbInitServer(screen);

    fprintf(stderr, "vnctestserver: listening on port %d (%dx%d)\n",
            g_port, g_width, g_height);

    /*
     * rfbProcessEvents() returns FALSE while no client is connected, but it
     * still accepts new connections - so the return value must be ignored.
     * rfbIsActive() tells us when the server has been shut down.
     */
    while (rfbIsActive(screen)) {
        rfbProcessEvents(screen, 1000000L / (g_fps > 0 ? g_fps : 1));
        g_frame++;
        render_frame();
        rfbMarkRectAsModified(screen, 0, 0, g_width, g_height);
    }

    return 0;
}
