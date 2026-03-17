/*
 * fb2ssd1306_scale.c – Oversample a larger region (e.g., 255x128) from /dev/fb0
 *                      and scale it down to 128x64 on an SSD1306 OLED.
 * 
 * Compile with: gcc -O2 -o fb2ssd1306_scale fb2ssd1306_scale.c
 * Run as root (required for /dev/mem and /dev/i2c access).
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

#define SSD1306_I2C_ADDR   0x3C        // typical SSD1306 address
#define I2C_DEVICE         "/dev/i2c-1"
#define FB_DEVICE          "/dev/fb0"

#define DISPLAY_WIDTH      128
#define DISPLAY_HEIGHT     64
#define DISPLAY_PAGES      (DISPLAY_HEIGHT / 8)

// Oversampling source region dimensions (change these as desired)
#define SRC_WIDTH          255
#define SRC_HEIGHT         128

static int fb_fd = -1;
static int i2c_fd = -1;
static void *fb_map = NULL;
static size_t fb_map_len = 0;
static volatile int keep_running = 1;

// ----------------------------------------------------------------------
// I2C helpers (same as before)
// ----------------------------------------------------------------------
static int i2c_open(void) {
    int fd = open(I2C_DEVICE, O_RDWR);
    if (fd < 0) {
        perror("open i2c");
        return -1;
    }
    if (ioctl(fd, I2C_SLAVE, SSD1306_I2C_ADDR) < 0) {
        perror("ioctl i2c slave");
        close(fd);
        return -1;
    }
    return fd;
}

static void i2c_close(int fd) {
    if (fd >= 0) close(fd);
}

static int ssd1306_command(int fd, uint8_t cmd) {
    uint8_t buf[2] = {0x00, cmd};
    if (write(fd, buf, 2) != 2) {
        perror("write i2c command");
        return -1;
    }
    return 0;
}

static int ssd1306_data(int fd, uint8_t *data, int len) {
    uint8_t *buf = malloc(len + 1);
    if (!buf) return -1;
    buf[0] = 0x40;
    memcpy(buf + 1, data, len);
    int ret = write(fd, buf, len + 1);
    free(buf);
    if (ret != len + 1) {
        perror("write i2c data");
        return -1;
    }
    return 0;
}

// ----------------------------------------------------------------------
// SSD1306 initialisation (unchanged)
// ----------------------------------------------------------------------
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
// Get luminance (0-255) from a framebuffer pixel
// ----------------------------------------------------------------------
static uint8_t pixel_luminance(uint8_t *fb, int x, int y,
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
        r = r * 255 / ((1 << vinfo->red.length)   - 1);
        g = g * 255 / ((1 << vinfo->green.length) - 1);
        b = b * 255 / ((1 << vinfo->blue.length)  - 1);
    } else if (bpp == 32) {
        uint32_t p32 = *(uint32_t*)p;
        r = (p32 >> vinfo->red.offset)   & 0xFF;
        g = (p32 >> vinfo->green.offset) & 0xFF;
        b = (p32 >> vinfo->blue.offset)  & 0xFF;
    } else {
        return 0;
    }

    // Luminance: Y = 0.299R + 0.587G + 0.114B
    return (77 * r + 150 * g + 29 * b) >> 8;
}

// ----------------------------------------------------------------------
// Cleanup on exit / signal
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
    int region_x = 0, region_y;

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
    printf("Oversampling source region: %dx%d\n", SRC_WIDTH, SRC_HEIGHT);

    if (vinfo.bits_per_pixel != 16 && vinfo.bits_per_pixel != 32) {
        fprintf(stderr, "Unsupported bits per pixel: %d\n", vinfo.bits_per_pixel);
        goto out_fb;
    }

    // Determine source region (bottom-left) – ensure it stays within framebuffer
    if (vinfo.yres < SRC_HEIGHT) {
        region_y = 0;
        fprintf(stderr, "Warning: framebuffer height < %d, using y=0\n", SRC_HEIGHT);
    } else {
        region_y = vinfo.yres - SRC_HEIGHT;
    }
    if (vinfo.xres < SRC_WIDTH) {
        fprintf(stderr, "Warning: framebuffer width < %d, will clamp\n", SRC_WIDTH);
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
        // Clear display buffer
        memset(display_buffer, 0, sizeof(display_buffer));

        // For each output pixel (x_out, y_out) determine the corresponding
        // source rectangle and compute average luminance.
        for (int y_out = 0; y_out < DISPLAY_HEIGHT; y_out++) {
            // Source y range for this output row
            int src_y_start = region_y + (y_out * SRC_HEIGHT) / DISPLAY_HEIGHT;
            int src_y_end   = region_y + ((y_out + 1) * SRC_HEIGHT + DISPLAY_HEIGHT - 1) / DISPLAY_HEIGHT;
            if (src_y_end > region_y + SRC_HEIGHT) src_y_end = region_y + SRC_HEIGHT;
            if (src_y_start >= vinfo.yres) continue; // out of bounds

            for (int x_out = 0; x_out < DISPLAY_WIDTH; x_out++) {
                // Source x range for this output column
                int src_x_start = region_x + (x_out * SRC_WIDTH) / DISPLAY_WIDTH;
                int src_x_end   = region_x + ((x_out + 1) * SRC_WIDTH + DISPLAY_WIDTH - 1) / DISPLAY_WIDTH;
                if (src_x_end > region_x + SRC_WIDTH) src_x_end = region_x + SRC_WIDTH;
                if (src_x_start >= vinfo.xres) continue; // out of bounds

                // Compute average luminance over the source rectangle
                uint32_t sum = 0;
                int count = 0;
                for (int sy = src_y_start; sy < src_y_end; sy++) {
                    if (sy >= vinfo.yres) break;
                    for (int sx = src_x_start; sx < src_x_end; sx++) {
                        if (sx >= vinfo.xres) break;
                        sum += pixel_luminance(fb_map, sx, sy, &vinfo, finfo.line_length);
                        count++;
                    }
                }

                uint8_t pixel_value = 0;
                if (count > 0) {
                    uint8_t avg = sum / count;
                    // Threshold at 128 (adjust if needed)
                    pixel_value = (avg > 128) ? 1 : 0;
                }

                // Place pixel in the display buffer (page-oriented)
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
