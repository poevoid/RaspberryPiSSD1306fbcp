/*
 * fb2ssd1306_center.c – Display a 128x64 area centered on the text cursor.
 * Uses /dev/vcsa0 to read cursor position AND console dimensions.
 * The cursor is always kept at the center of the OLED; out‑of‑bounds areas become black.
 * 
 * DISPLAY LOGIC: Any pixel that is NOT pure black appears white.
 * 
 * Compile: gcc -O2 -o fb2ssd1306_center fb2ssd1306_center.c
 * Run as root: sudo ./fb2ssd1306_center
 * 
 * To enable debug output, compile with -DDEBUG
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <linux/i2c-dev.h>
#include <sys/stat.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>

// SSD1306 settings
#define SSD1306_I2C_ADDR   0x3C
#define I2C_DEVICE         "/dev/i2c-1"
#define FB_DEVICE          "/dev/fb0"
#define DISPLAY_WIDTH      128
#define DISPLAY_HEIGHT     64
#define DISPLAY_PAGES      (DISPLAY_HEIGHT / 8)

static int fb_fd = -1;
static int i2c_fd = -1;
static void *fb_map = NULL;
static size_t fb_map_len = 0;
static volatile int keep_running = 1;

// ----------------------------------------------------------------------
// I2C helpers (unchanged)
// ----------------------------------------------------------------------
static int i2c_open(void) {
    int fd = open(I2C_DEVICE, O_RDWR);
    if (fd < 0) { perror("open i2c"); return -1; }
    if (ioctl(fd, I2C_SLAVE, SSD1306_I2C_ADDR) < 0) {
        perror("ioctl i2c slave"); close(fd); return -1;
    }
    return fd;
}

static void i2c_close(int fd) {
    if (fd >= 0) close(fd);
}

static int ssd1306_command(int fd, uint8_t cmd) {
    uint8_t buf[2] = {0x00, cmd};
    if (write(fd, buf, 2) != 2) { perror("write i2c command"); return -1; }
    return 0;
}

static int ssd1306_data(int fd, uint8_t *data, int len) {
    uint8_t *buf = malloc(len + 1);
    if (!buf) return -1;
    buf[0] = 0x40;
    memcpy(buf + 1, data, len);
    int ret = write(fd, buf, len + 1);
    free(buf);
    if (ret != len + 1) { perror("write i2c data"); return -1; }
    return 0;
}

static int ssd1306_init(int fd) {
    ssd1306_command(fd, 0xAE);
    ssd1306_command(fd, 0xD5); ssd1306_command(fd, 0x80);
    ssd1306_command(fd, 0xA8); ssd1306_command(fd, 0x3F);
    ssd1306_command(fd, 0xD3); ssd1306_command(fd, 0x00);
    ssd1306_command(fd, 0x40);
    ssd1306_command(fd, 0x8D); ssd1306_command(fd, 0x14);
    ssd1306_command(fd, 0x20); ssd1306_command(fd, 0x02); // page addressing
    ssd1306_command(fd, 0xA1);
    ssd1306_command(fd, 0xC8);
    ssd1306_command(fd, 0xDA); ssd1306_command(fd, 0x12);
    ssd1306_command(fd, 0x81); ssd1306_command(fd, 0xCF);
    ssd1306_command(fd, 0xD9); ssd1306_command(fd, 0xF1);
    ssd1306_command(fd, 0xDB); ssd1306_command(fd, 0x40);
    ssd1306_command(fd, 0xA4);
    ssd1306_command(fd, 0xA6);
    ssd1306_command(fd, 0xAF);
    return 0;
}

// ----------------------------------------------------------------------
// Check if a framebuffer pixel is pure black
// Returns 1 if black, 0 otherwise.
// ----------------------------------------------------------------------
static int pixel_is_black(uint8_t *fb, int x, int y,
                          struct fb_var_screeninfo *vinfo, int stride) {
    int bpp = vinfo->bits_per_pixel;
    int bytes_per_pixel = bpp / 8;
    uint8_t *p = fb + y * stride + x * bytes_per_pixel;
    uint32_t r, g, b;

    if (bpp == 16) {
        uint16_t p16 = *(uint16_t*)p;
        r = (p16 >> vinfo->red.offset)   & ((1 << vinfo->red.length)   - 1);
        g = (p16 >> vinfo->green.offset) & ((1 << vinfo->green.length) - 1);
        b = (p16 >> vinfo->blue.offset)  & ((1 << vinfo->blue.length)  - 1);
        // For 16-bit, black means all components zero
        return (r == 0 && g == 0 && b == 0);
    } else if (bpp == 32) {
        uint32_t p32 = *(uint32_t*)p;
        r = (p32 >> vinfo->red.offset)   & 0xFF;
        g = (p32 >> vinfo->green.offset) & 0xFF;
        b = (p32 >> vinfo->blue.offset)  & 0xFF;
        return (r == 0 && g == 0 && b == 0);
    } else {
        // Unsupported depth: treat as black? Safer to return 1 (black) to avoid artifacts.
        return 1;
    }
}

// ----------------------------------------------------------------------
// Get cursor position AND console dimensions using /dev/vcsa0
// Returns 1 on success, 0 on failure.
// Coordinates are 1-based (row, col).
// ----------------------------------------------------------------------
static int get_console_info(int *cursor_row, int *cursor_col,
                            int *console_rows, int *console_cols) {
    int vcsa_fd = open("/dev/vcsa0", O_RDONLY);
    if (vcsa_fd < 0) {
        // Fallback to /dev/vcsa (might work if only one console)
        vcsa_fd = open("/dev/vcsa", O_RDONLY);
        if (vcsa_fd < 0) return 0;
    }

    unsigned char header[4];
    if (read(vcsa_fd, header, 4) != 4) {
        close(vcsa_fd);
        return 0;
    }
    close(vcsa_fd);

    *console_rows = header[0];
    *console_cols = header[1];
    *cursor_col   = header[2];
    *cursor_row   = header[3];
    return 1;
}

// ----------------------------------------------------------------------
// Cleanup on signal
// ----------------------------------------------------------------------
static void cleanup(int sig) {
    (void)sig;
    keep_running = 0;
}

// ----------------------------------------------------------------------
// Main
// ----------------------------------------------------------------------
int main(int argc, char **argv) {
    struct fb_fix_screeninfo finfo;
    struct fb_var_screeninfo vinfo;
    uint8_t display_buffer[DISPLAY_WIDTH * DISPLAY_PAGES];
    int region_x = 0, region_y = 0;          // top-left of source region (may be negative)
    int last_cursor_row = -1, last_cursor_col = -1;
    int frame_count = 0;

    // Font size (pixels per character) – will be set from console info
    int font_w = 8, font_h = 16; // fallback defaults
    int console_rows = 0, console_cols = 0;

    signal(SIGINT, cleanup);
    signal(SIGTERM, cleanup);

    // ---------- Open framebuffer ----------
    fb_fd = open(FB_DEVICE, O_RDONLY);
    if (fb_fd < 0) {
        perror("open fb0");
        return 1;
    }

    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        perror("FBIOGET_VSCREENINFO");
        goto out_fb;
    }
    if (ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        perror("FBIOGET_FSCREENINFO");
        goto out_fb;
    }

    printf("Framebuffer: %dx%d, %d bpp, line length %d bytes\n",
           vinfo.xres, vinfo.yres, vinfo.bits_per_pixel, finfo.line_length);

    if (vinfo.bits_per_pixel != 16 && vinfo.bits_per_pixel != 32) {
        fprintf(stderr, "Unsupported bits per pixel: %d\n", vinfo.bits_per_pixel);
        goto out_fb;
    }

    // Map framebuffer
    fb_map_len = finfo.smem_len;
    fb_map = mmap(NULL, fb_map_len, PROT_READ, MAP_SHARED, fb_fd, 0);
    if (fb_map == MAP_FAILED) {
        perror("mmap fb");
        goto out_fb;
    }

    // ---------- Open I2C ----------
    i2c_fd = i2c_open();
    if (i2c_fd < 0) {
        goto out_unmap;
    }

    // ---------- Initialise SSD1306 ----------
    if (ssd1306_init(i2c_fd) < 0) {
        fprintf(stderr, "SSD1306 init failed\n");
        goto out_i2c;
    }

    // ---------- Main loop ----------
    while (keep_running) {
        // Update cursor position every 5 frames (~0.5 seconds at 10 fps)
        if (frame_count % 5 == 0) {
            int cursor_row, cursor_col;
            int new_console_rows, new_console_cols;
            if (get_console_info(&cursor_row, &cursor_col,
                                 &new_console_rows, &new_console_cols)) {
                // If console dimensions changed, update font size
                if (new_console_rows != console_rows || new_console_cols != console_cols) {
                    console_rows = new_console_rows;
                    console_cols = new_console_cols;
                    if (console_rows > 0 && console_cols > 0) {
                        font_w = vinfo.xres / console_cols;
                        font_h = vinfo.yres / console_rows;
                        printf("Console size: %dx%d chars, font size: %dx%d pixels\n",
                               console_cols, console_rows, font_w, font_h);
                    }
                }

                // Sanity check: row and col should be within console bounds
                if (cursor_row >= 1 && cursor_row <= console_rows &&
                    cursor_col >= 1 && cursor_col <= console_cols) {
                    if (cursor_row != last_cursor_row || cursor_col != last_cursor_col) {
                        last_cursor_row = cursor_row;
                        last_cursor_col = cursor_col;

                        // Convert cursor to pixel coordinates (center of character cell)
                        int cursor_pixel_x = (cursor_col - 1) * font_w + font_w / 2;
                        int cursor_pixel_y = (cursor_row - 1) * font_h + font_h / 2;

                        // Center the 128x64 source region on the cursor (NO CLAMPING)
                        region_x = cursor_pixel_x - DISPLAY_WIDTH / 2;
                        region_y = cursor_pixel_y - DISPLAY_HEIGHT / 2-16;

#ifdef DEBUG
                        fprintf(stderr, "Cursor: (%d,%d) -> pixel (%d,%d) -> region (%d,%d)\n",
                                cursor_row, cursor_col, cursor_pixel_x, cursor_pixel_y,
                                region_x, region_y);
#endif
                    }
                } else {
                    fprintf(stderr, "Cursor position out of console bounds: (%d,%d) vs (%d,%d)\n",
                            cursor_row, cursor_col, console_rows, console_cols);
                }
            }
        }

        // Build display buffer: directly map each output pixel to one source pixel
        // Pixels outside the framebuffer become black (0)
        // Any pixel that is not pure black becomes white (1)
        memset(display_buffer, 0, sizeof(display_buffer));
        for (int y_out = 0; y_out < DISPLAY_HEIGHT; y_out++) {
            int src_y = region_y + y_out;
            for (int x_out = 0; x_out < DISPLAY_WIDTH; x_out++) {
                int src_x = region_x + x_out;
                uint8_t pixel_value = 0; // default black

                // Only read if within framebuffer bounds
                if (src_x >= 0 && src_x < vinfo.xres && src_y >= 0 && src_y < vinfo.yres) {
                    int is_black = pixel_is_black(fb_map, src_x, src_y, &vinfo, finfo.line_length);
                    pixel_value = is_black ? 0 : 1; // non-black => white
                }

                int page = y_out / 8;
                int bit  = y_out % 8;
                if (pixel_value) {
                    display_buffer[page * DISPLAY_WIDTH + x_out] |= (1 << bit);
                } else {
                    display_buffer[page * DISPLAY_WIDTH + x_out] &= ~(1 << bit);
                }
            }
        }

        // Send to SSD1306
        for (int page = 0; page < DISPLAY_PAGES; page++) {
            ssd1306_command(i2c_fd, 0xB0 | page);
            ssd1306_command(i2c_fd, 0x00);
            ssd1306_command(i2c_fd, 0x10);
            ssd1306_data(i2c_fd, &display_buffer[page * DISPLAY_WIDTH], DISPLAY_WIDTH);
        }

        frame_count++;
        usleep(100000);   // ~10 fps
    }

    // ---------- Clean exit ----------
    ssd1306_command(i2c_fd, 0xAE);   // display off
    i2c_close(i2c_fd);
    munmap(fb_map, fb_map_len);
    close(fb_fd);
    printf("Exited cleanly.\n");
    return 0;

out_i2c:
    i2c_close(i2c_fd);
out_unmap:
    munmap(fb_map, fb_map_len);
out_fb:
    close(fb_fd);
    return 1;
}
