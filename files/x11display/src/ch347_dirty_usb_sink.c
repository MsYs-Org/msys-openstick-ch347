#define _GNU_SOURCE

#include <errno.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <linux/usbdevice_fs.h>
#include <linux/uhid.h>
#include <linux/input.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "frame_mailbox.h"
#include "frame_rotation.h"

#define USB_DEV "/dev/bus/usb/001/003"
#define USB_IFACE 4
#define EP_OUT 0x06
#define EP_IN 0x86

#define LCD_WIDTH 320
#define LCD_HEIGHT 480
#define FRAME_BYTES ((size_t)LCD_WIDTH * LCD_HEIGHT * 2)
#define FRAME_PIXELS ((size_t)LCD_WIDTH * LCD_HEIGHT)
#define STRIDE_BYTES (LCD_WIDTH * 2)

#define PAYLOAD 480
#define PACKET (PAYLOAD + 3)
#define DEFAULT_DEPTH 8
#define DEFAULT_URB_TIMEOUT_MS 1500
#define CAL_POS_POINTS 5
#define CAL_PRESS_CYCLES 3
#define CAL_PRESS_POINTS (CAL_PRESS_CYCLES * 2)
#define TOUCH_PRESSURE_FLOOR 8

enum touch_input_mode {
    TOUCH_MODE_TOUCH = 0,
    TOUCH_MODE_MOUSE = 1,
};

#define GPIO_DC 0
#define GPIO_RESET 1
#define GPIO_LED 2
#define GPIO_TOUCH_IRQ 3

#define GPIO_MASK(bit) (1u << (bit))
#define GPIO_OUTPUT_MASK \
    (GPIO_MASK(GPIO_DC) | GPIO_MASK(GPIO_RESET) | GPIO_MASK(GPIO_LED))
#define GPIO_BASE_STATE (GPIO_MASK(GPIO_RESET) | GPIO_MASK(GPIO_LED))

#define CH347_SPI_CFG_LEN 26
#define CH347_SPI_BAUD_OFFSET 12
#define CH347_SPI_GET_CFG 0xCA
#define CH347_SPI_SET_CFG 0xC0
#define RECT_MAGIC "XDR1"
#define RECT_HDR_LEN 16
#define RECT_ITEM_HDR_LEN 12

/*
 * 32x15 is deliberate for this CH347 link: one tile is 32*15*2 = 960 bytes,
 * exactly two 480-byte C4 payloads. Any merged tile rectangle therefore avoids
 * short tail data packets, which were the main source of color/stripe errors.
 */
#define TILE_W 32
#define TILE_H 15
#define TILES_X ((LCD_WIDTH + TILE_W - 1) / TILE_W)
#define TILES_Y ((LCD_HEIGHT + TILE_H - 1) / TILE_H)
#define MAX_TILE_RECTS (TILES_X * TILES_Y)

struct slot {
    struct usbdevfs_urb urb;
    uint8_t buf[PACKET];
    unsigned int index;
};

struct rect {
    unsigned int x0;
    unsigned int y0;
    unsigned int x1;
    unsigned int y1;
};

#define DEBUG_OVERLAY_FPS (1u << 0)
#define DEBUG_OVERLAY_DIRTY (1u << 1)
#define DEBUG_OVERLAY_BYTES (1u << 2)
#define DEBUG_OVERLAY_BBOX (1u << 3)
#define DEBUG_OVERLAY_MEMORY (1u << 4)
#define DEBUG_OVERLAY_CPU (1u << 5)
#define DEBUG_OVERLAY_ALL_ITEMS ((1u << 6) - 1u)
#define DEBUG_OVERLAY_DEFAULT_ITEMS \
    (DEBUG_OVERLAY_FPS | DEBUG_OVERLAY_DIRTY | DEBUG_OVERLAY_BYTES | \
        DEBUG_OVERLAY_CPU)
#define DEBUG_OVERLAY_MAX_LINES 6
#define DEBUG_OVERLAY_LINE_CAPACITY 48

static volatile sig_atomic_t runtime_reload_requested;

static void runtime_reload_signal(int signal_number)
{
    if (signal_number == SIGUSR1)
        runtime_reload_requested = 1;
}

struct debug_overlay_metrics {
    double capture_fps;
    double panel_fps;
    unsigned int rects;
    double dirty_pct;
    size_t last_sent_pixels;
    unsigned long long sent_pixels;
    struct rect bbox;
    int bbox_valid;
};

struct cpu_usage_state {
    uint64_t previous_total;
    uint64_t previous_idle;
    double percent;
    unsigned int sample_count;
    int baseline_valid;
    int percent_valid;
    int log_state;
};

struct debug_overlay_state {
    int enabled;
    unsigned int alpha;
    unsigned int scale;
    unsigned int items;
    unsigned int interval_ms;
    double last_sample;
    unsigned int line_count;
    char lines[DEBUG_OVERLAY_MAX_LINES][DEBUG_OVERLAY_LINE_CAPACITY];
    struct rect bounds;
    struct cpu_usage_state cpu;
    const char *cpu_stat_path;
};

struct reader_state {
    int fd;
    int repeat_input;
    int stop;
    const char *input_path;
    uint8_t *latest;
    uint8_t *tmp;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    uint64_t seq;
    uint64_t acked_seq;
    unsigned int frames;
    int eof;
    int error;
};

struct touch_uhid {
    int fd;
    int active;
};

struct x11_touch_api {
    void *x11_lib;
    void *xtst_lib;
    void *display;
    int screen;
    void *(*XOpenDisplay)(const char *display_name);
    int (*XCloseDisplay)(void *display);
    int (*XFlush)(void *display);
    int (*XDefaultScreen)(void *display);
    int (*XTestFakeMotionEvent)(void *display, int screen, int x, int y,
            unsigned long delay);
    int (*XTestFakeButtonEvent)(void *display, unsigned int button,
            int is_press, unsigned long delay);
};

struct touch_state {
    int enabled;
    int use_irq;
    int down;
    int cursor_enabled;
    int cursor_visible;
    int calibrate;
    int cal_wait_release;
    int cal_done;
    int cal_exit;
    int swap_xy;
    int invert_x;
    int invert_y;
    int debug;
    int disable_on_errors;
    enum touch_input_mode input_mode;
    unsigned int poll_ms;
    unsigned int width;
    unsigned int height;
    unsigned int x_min;
    unsigned int x_max;
    unsigned int y_min;
    unsigned int y_max;
    unsigned int move_thresh;
    unsigned int jump_thresh;
    unsigned int filter_weight;
    unsigned int timeout_ms;
    unsigned int errors;
    unsigned int max_errors;
    unsigned int z_min;
    unsigned int pressure_min;
    unsigned int touch_clock;
    unsigned int lcd_clock;
    unsigned int release_samples;
    unsigned int release_count;
    int z_strict;
    enum frame_rotation rotation;
    unsigned int logical_width;
    unsigned int logical_height;
    int last_x;
    int last_y;
    int cursor_x;
    int cursor_y;
    unsigned int cal_index;
    unsigned int cal_press_index;
    unsigned int cal_margin;
    int cal_sx[CAL_POS_POINTS];
    int cal_sy[CAL_POS_POINTS];
    unsigned int cal_raw_x[CAL_POS_POINTS];
    unsigned int cal_raw_y[CAL_POS_POINTS];
    unsigned int cal_raw_z1[CAL_POS_POINTS];
    unsigned int cal_raw_z2[CAL_POS_POINTS];
    unsigned int cal_raw_pressure[CAL_POS_POINTS];
    unsigned int cal_press_pressure[CAL_PRESS_POINTS];
    unsigned int cal_press_z1[CAL_PRESS_POINTS];
    unsigned int cal_press_z2[CAL_PRESS_POINTS];
    unsigned int cal_samples_required;
    unsigned int cal_sample_count;
    uint64_t cal_sum_x;
    uint64_t cal_sum_y;
    uint64_t cal_sum_z1;
    uint64_t cal_sum_z2;
    uint64_t cal_sum_pressure;
    unsigned int last_raw_x;
    unsigned int last_raw_y;
    unsigned int last_raw_z1;
    unsigned int last_raw_z2;
    unsigned int last_pressure;
    int last_screen_x;
    int last_screen_y;
    int raw_valid;
    int filter_valid;
    int filtered_x;
    int filtered_y;
    double last_poll;
    double last_mode_check;
    double cal_done_time;
    const char *display_name;
    const char *cal_file;
    const char *mode_file;
    struct touch_uhid uhid;
    struct x11_touch_api x11;
};

struct gpio_overlay_state {
    int enabled;
    unsigned int poll_ms;
    uint8_t pins;
    uint8_t raw[8];
    int valid;
    double last_poll;
};

static int hold_cs = 1;
static int gpio_dc_high = 1;
static unsigned int usb_debug = 0;
static uint8_t spi_cfg_cache[CH347_SPI_CFG_LEN];
static int spi_cfg_valid = 0;
static int spi_clock_index = -1;

static int usb_bulk_sync(int fd, unsigned char ep, void *data, int len,
        unsigned int timeout_ms);
static int tile_changed(const uint8_t *frame, const uint8_t *prev,
        unsigned int tx, unsigned int ty);
static size_t tile_pixels(unsigned int tx, unsigned int ty);

static int ch347_read_expected(int fd, uint8_t expect, uint8_t *reply,
        size_t reply_len, int min_len, unsigned int timeout_ms,
        const char *what)
{
    unsigned int stale = 0;
    unsigned int stale_c4 = 0;
    int ret;

    for (; stale < 128; stale++) {
        ret = usb_bulk_sync(fd, EP_IN, reply, (int)reply_len, timeout_ms);
        if (ret <= 0)
            return -1;
        if (reply[0] == expect) {
            if (ret < min_len)
                return -1;
            if (usb_debug && stale)
                fprintf(stderr, "%s skipped stale=%u c4=%u\n",
                        what, stale, stale_c4);
            return ret;
        }
        if (reply[0] == 0xC4)
            stale_c4++;
        else if (usb_debug && stale < 4)
            fprintf(stderr, "%s skipped stale first=0x%02x len=%d\n",
                    what, reply[0], ret);
    }

    if (usb_debug)
        fprintf(stderr, "%s too many stale replies c4=%u\n", what, stale_c4);
    return -1;
}

static double now_sec(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}
static uint64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}



static void realtime_after_ms(struct timespec *deadline, unsigned int ms)
{

    clock_gettime(CLOCK_REALTIME, deadline);
    deadline->tv_sec += ms / 1000;
    deadline->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (deadline->tv_nsec >= 1000000000L) {
        deadline->tv_sec++;
        deadline->tv_nsec -= 1000000000L;
    }
}

static unsigned int env_u32(const char *name, unsigned int def)
{
    const char *v = getenv(name);

    if (!v || !*v)
        return def;
    return (unsigned int)strtoul(v, NULL, 0);
}

static unsigned int env_u32_range(const char *name, unsigned int def,
        unsigned int minimum, unsigned int maximum)
{
    const char *value = getenv(name);
    char *end = NULL;
    unsigned long parsed;

    if (!value || !*value)
        return def;
    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno || !end || *end || parsed < minimum || parsed > maximum)
        return def;
    return (unsigned int)parsed;
}

static unsigned int debug_overlay_items_parse(const char *value)
{
    char copy[128];
    char *cursor;
    char *token;
    unsigned int items = 0;

    if (!value || !*value)
        return DEBUG_OVERLAY_DEFAULT_ITEMS;
    if (strlen(value) >= sizeof(copy))
        return DEBUG_OVERLAY_DEFAULT_ITEMS;
    memcpy(copy, value, strlen(value) + 1u);
    cursor = copy;
    while ((token = strsep(&cursor, ",")) != NULL) {
        char *end;

        while (*token == ' ' || *token == '\t')
            token++;
        end = token + strlen(token);
        while (end > token && (end[-1] == ' ' || end[-1] == '\t'))
            *--end = '\0';
        if (!*token)
            return DEBUG_OVERLAY_DEFAULT_ITEMS;
        if (strcmp(token, "all") == 0)
            items |= DEBUG_OVERLAY_ALL_ITEMS;
        else if (strcmp(token, "none") == 0)
            items = 0;
        else if (strcmp(token, "fps") == 0)
            items |= DEBUG_OVERLAY_FPS;
        else if (strcmp(token, "dirty") == 0)
            items |= DEBUG_OVERLAY_DIRTY;
        else if (strcmp(token, "bytes") == 0)
            items |= DEBUG_OVERLAY_BYTES;
        else if (strcmp(token, "bbox") == 0)
            items |= DEBUG_OVERLAY_BBOX;
        else if (strcmp(token, "memory") == 0)
            items |= DEBUG_OVERLAY_MEMORY;
        else if (strcmp(token, "cpu") == 0)
            items |= DEBUG_OVERLAY_CPU;
        else
            return DEBUG_OVERLAY_DEFAULT_ITEMS;
    }
    return items;
}

static double env_double(const char *name, double def)
{
    const char *v = getenv(name);

    if (!v || !*v)
        return def;
    return atof(v);
}

static int abs_int(int v)
{
    return v < 0 ? -v : v;
}

static enum touch_input_mode touch_mode_parse(const char *value,
        enum touch_input_mode fallback)
{
    if (!value)
        return fallback;
    while (*value == 32 || *value == 9 || *value == 13 || *value == 10)
        value++;
    if (!strncasecmp(value, "touch", 5) || !strncmp(value, "0", 1))
        return TOUCH_MODE_TOUCH;
    if (!strncasecmp(value, "mouse", 5) || !strncmp(value, "1", 1))
        return TOUCH_MODE_MOUSE;
    return fallback;
}

static const char *touch_mode_name(enum touch_input_mode mode)
{
    return mode == TOUCH_MODE_MOUSE ? "mouse" : "touch";
}

static unsigned int touch_pressure(unsigned int z1, unsigned int z2)
{
    (void)z2;
    return z1;
}

static void sort_u16(uint16_t *values, unsigned int count)
{
    for (unsigned int i = 1; i < count; i++) {
        uint16_t value = values[i];
        unsigned int j = i;

        while (j > 0 && values[j - 1] > value) {
            values[j] = values[j - 1];
            j--;
        }
        values[j] = value;
    }
}

static int read_full_fd(int fd, uint8_t *buf, size_t len)
{
    size_t got = 0;

    while (got < len) {
        ssize_t ret = read(fd, buf + got, len - got);

        if (ret == 0)
            return got ? -1 : 0;
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        got += (size_t)ret;
    }

    return 1;
}

static int reset_usb_device(const char *path)
{
    int fd;
    int result;

    if (path == NULL || path[0] != '/') {
        fprintf(stderr, "usb reset requires an absolute usbfs device path\n");
        return 64;
    }
    fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "usb reset open %s failed: %s\n", path,
                strerror(errno));
        return 1;
    }
    result = ioctl(fd, USBDEVFS_RESET, 0);
    if (result < 0)
        fprintf(stderr, "USBDEVFS_RESET %s failed: %s\n", path,
                strerror(errno));
    else
        fprintf(stderr, "USBDEVFS_RESET %s succeeded\n", path);
    close(fd);
    return result < 0 ? 1 : 0;
}

static uint16_t get_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int mailbox_copy_latest(void *mapping, size_t mapping_size,
        uint8_t *frame, uint64_t *consumed_seq, unsigned int *captured_frames,
        unsigned int *dropped_frames)
{
    struct frame_mailbox_header *header = mapping;
    uint64_t seq;

    if (mapping_size < frame_mailbox_size(FRAME_BYTES) ||
            header->magic != FRAME_MAILBOX_MAGIC ||
            header->version != FRAME_MAILBOX_VERSION ||
            header->width != LCD_WIDTH || header->height != LCD_HEIGHT ||
            header->frame_bytes != FRAME_BYTES ||
            header->slot_count != FRAME_MAILBOX_SLOTS)
        return -1;

    for (unsigned int attempt = 0; attempt < 4; attempt++) {
        unsigned int slot;
        uint64_t slot_seq;

        seq = atomic_load_explicit(&header->published_seq, memory_order_acquire);
        if (!seq || seq == *consumed_seq)
            return 0;
        slot = (unsigned int)(seq % FRAME_MAILBOX_SLOTS);
        slot_seq = atomic_load_explicit(&header->slot_seq[slot],
                memory_order_acquire);
        if (slot_seq != seq)
            continue;
        memcpy(frame, frame_mailbox_slot(mapping, FRAME_BYTES, slot), FRAME_BYTES);
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&header->slot_seq[slot],
                    memory_order_acquire) != seq)
            continue;
        if (*consumed_seq && seq > *consumed_seq + 1)
            *dropped_frames += (unsigned int)(seq - *consumed_seq - 1);
        *consumed_seq = seq;
        *captured_frames = (unsigned int)seq;
        return 1;
    }
    return 0;
}

static int mailbox_copy_next(void *mapping, size_t mapping_size,
        uint8_t *frame, uint64_t *consumed_seq, unsigned int *captured_frames,
        unsigned int *dropped_frames, int prefetched_frame)
{
    if (prefetched_frame)
        return 1;
    return mailbox_copy_latest(mapping, mapping_size, frame, consumed_seq,
            captured_frames, dropped_frames);
}

static void *reader_thread(void *arg)
{
    struct reader_state *rs = arg;

    for (;;) {
        int ret = read_full_fd(rs->fd, rs->tmp, FRAME_BYTES);

        if (ret == 0) {
            if (rs->repeat_input && rs->input_path &&
                    lseek(rs->fd, 0, SEEK_SET) >= 0)
                continue;

            pthread_mutex_lock(&rs->lock);
            rs->eof = 1;
            pthread_cond_signal(&rs->cond);
            pthread_mutex_unlock(&rs->lock);
            return NULL;
        }

        if (ret < 0) {
            pthread_mutex_lock(&rs->lock);
            rs->error = 1;
            pthread_cond_signal(&rs->cond);
            pthread_mutex_unlock(&rs->lock);
            return NULL;
        }

        pthread_mutex_lock(&rs->lock);
        if (rs->stop) {
            pthread_mutex_unlock(&rs->lock);
            return NULL;
        }
        {
            uint8_t *swap = rs->latest;
            rs->latest = rs->tmp;
            rs->tmp = swap;
        }
        rs->seq++;
        rs->frames++;
        pthread_cond_signal(&rs->cond);
        pthread_mutex_unlock(&rs->lock);

    }
}

static void fill_slot(struct slot *s, const uint8_t *data, size_t off,
        size_t len, unsigned int index)
{
    size_t chunk = len - off;

    if (chunk > PAYLOAD)
        chunk = PAYLOAD;

    s->index = index;
    s->buf[0] = 0xC4;
    s->buf[1] = chunk & 0xff;
    s->buf[2] = (chunk >> 8) & 0xff;
    memcpy(s->buf + 3, data + off, chunk);

    memset(&s->urb, 0, sizeof(s->urb));
    s->urb.type = USBDEVFS_URB_TYPE_BULK;
    s->urb.endpoint = EP_OUT;
    s->urb.buffer = s->buf;
    s->urb.buffer_length = (int)chunk + 3;
    s->urb.usercontext = s;
}

static int submit_slot(int fd, struct slot *s)
{
    if (ioctl(fd, USBDEVFS_SUBMITURB, &s->urb) < 0) {
        fprintf(stderr, "SUBMITURB %u failed: %s\n", s->index, strerror(errno));
        return -1;
    }
    return 0;
}

static int send_bytes(int fd, struct slot *slots, unsigned int depth,
        const uint8_t *data, size_t len, unsigned int packet_delay_us)
{
    size_t next_off = 0;
    unsigned int next_index = 0;
    unsigned int completed = 0;
    unsigned int in_flight = 0;
    unsigned int total = (unsigned int)((len + PAYLOAD - 1) / PAYLOAD);
    unsigned int timeout_ms = env_u32("CH347_URB_TIMEOUT_MS",
            DEFAULT_URB_TIMEOUT_MS);
    double last_progress = now_sec();

    if (!len)
        return 0;

    while (next_off < len && in_flight < depth) {
        fill_slot(&slots[in_flight], data, next_off, len, next_index++);
        next_off += (size_t)slots[in_flight].urb.buffer_length - 3;
        if (submit_slot(fd, &slots[in_flight]) < 0)
            return -1;
        in_flight++;
    }

    while (completed < total) {
        struct usbdevfs_urb *done = NULL;
        struct slot *s;

        if (ioctl(fd, USBDEVFS_REAPURBNDELAY, &done) < 0) {
            if (errno == EAGAIN) {
                if ((now_sec() - last_progress) * 1000.0 > timeout_ms) {
                    fprintf(stderr,
                            "URB timeout after %.0fms completed=%u/%u in_flight=%u\n",
                            (now_sec() - last_progress) * 1000.0,
                            completed, total, in_flight);
                    for (unsigned int i = 0; i < depth; i++)
                        (void)ioctl(fd, USBDEVFS_DISCARDURB, &slots[i].urb);
                    return -1;
                }
                usleep(1000);
                continue;
            }

            fprintf(stderr, "REAPURBNDELAY failed: %s\n", strerror(errno));
            for (unsigned int i = 0; i < depth; i++)
                (void)ioctl(fd, USBDEVFS_DISCARDURB, &slots[i].urb);
            return -1;
        }
        if (!done || done->status) {
            fprintf(stderr, "URB failed status %d actual %d\n",
                    done ? done->status : -999, done ? done->actual_length : -1);
            for (unsigned int i = 0; i < depth; i++)
                (void)ioctl(fd, USBDEVFS_DISCARDURB, &slots[i].urb);
            return -1;
        }

        last_progress = now_sec();
        completed++;
        in_flight--;
        if (packet_delay_us)
            usleep(packet_delay_us);

        s = (struct slot *)done->usercontext;
        if (next_off < len) {
            fill_slot(s, data, next_off, len, next_index++);
            next_off += (size_t)s->urb.buffer_length - 3;
            if (submit_slot(fd, s) < 0)
                return -1;
            in_flight++;
        }
    }

    return 0;
}

static int usb_bulk_sync(int fd, unsigned char ep, void *data, int len,
        unsigned int timeout_ms)
{
    struct usbdevfs_bulktransfer bulk;
    int ret;

    memset(&bulk, 0, sizeof(bulk));
    bulk.ep = ep;
    bulk.len = len;
    bulk.timeout = timeout_ms;
    bulk.data = data;

    ret = ioctl(fd, USBDEVFS_BULK, &bulk);
    if (ret < 0)
        return -errno;
    return ret;
}

static int gpio_dc(int fd, int high)
{
    uint8_t packet[11] = {0};
    uint8_t reply[64] = {0};
    uint8_t state = GPIO_BASE_STATE;
    int ret;

    if (gpio_dc_high == !!high)
        return 0;

    if (high)
        state |= GPIO_MASK(GPIO_DC);

    packet[0] = 0xCC;
    packet[1] = 0x08;
    packet[2] = 0x00;

    for (unsigned int i = 0; i < 8; i++) {
        if (!(GPIO_OUTPUT_MASK & GPIO_MASK(i)))
            continue;
        packet[3 + i] = 0x80 | 0x40 | 0x20 | 0x10;
        if (state & GPIO_MASK(i))
            packet[3 + i] |= 0x08;
    }

    ret = usb_bulk_sync(fd, EP_OUT, packet, sizeof(packet), 1000);
    if (ret != (int)sizeof(packet)) {
        fprintf(stderr, "GPIO DC write failed: %d\n", ret);
        return -1;
    }

    ret = usb_bulk_sync(fd, EP_IN, reply, sizeof(reply), 1000);
    if (ret <= 0) {
        fprintf(stderr, "GPIO DC ack failed: %d\n", ret);
        return -1;
    }

    gpio_dc_high = !!high;
    return 0;
}

static int lcd_cs(int fd, int active)
{
    uint8_t packet[13] = {0};
    int ret;

    if (hold_cs)
        return 0;

    packet[0] = 0xC1;
    packet[1] = 0x0A;
    packet[2] = 0x00;
    packet[3] = active ? 0x80 : 0xC0;
    packet[8] = 0xC0;

    ret = usb_bulk_sync(fd, EP_OUT, packet, sizeof(packet), 1000);
    return ret == (int)sizeof(packet) ? 0 : -1;
}

static int lcd_cs_raw(int fd, int active)
{
    uint8_t packet[13] = {0};
    int ret;

    packet[0] = 0xC1;
    packet[1] = 0x0A;
    packet[2] = 0x00;
    packet[3] = active ? 0x80 : 0xC0;
    packet[8] = 0xC0;

    ret = usb_bulk_sync(fd, EP_OUT, packet, sizeof(packet), 1000);
    return ret == (int)sizeof(packet) ? 0 : -1;
}

static int spi_small(int fd, const uint8_t *data, size_t len)
{
    uint8_t packet[PAYLOAD + 3];
    int ret;

    if (len > PAYLOAD)
        return -1;

    packet[0] = 0xC4;
    packet[1] = len & 0xff;
    packet[2] = (len >> 8) & 0xff;
    memcpy(packet + 3, data, len);

    ret = usb_bulk_sync(fd, EP_OUT, packet, (int)len + 3, 1000);
    return ret == (int)len + 3 ? 0 : -1;
}

static int lcd_cmd(int fd, uint8_t cmd)
{
    if (gpio_dc(fd, 0) < 0)
        return -1;
    if (lcd_cs(fd, 1) < 0)
        return -1;
    if (spi_small(fd, &cmd, 1) < 0)
        return -1;
    return lcd_cs(fd, 0);
}

static int lcd_data_small(int fd, const uint8_t *data, size_t len)
{
    if (gpio_dc(fd, 1) < 0)
        return -1;
    if (lcd_cs(fd, 1) < 0)
        return -1;
    if (spi_small(fd, data, len) < 0)
        return -1;
    return lcd_cs(fd, 0);
}

static int lcd_cmd_data(int fd, uint8_t cmd, const uint8_t *data, size_t len)
{
    if (lcd_cmd(fd, cmd) < 0)
        return -1;
    return lcd_data_small(fd, data, len);
}

static int ch347_spi_read_cfg(int fd)
{
    uint8_t packet[4] = {CH347_SPI_GET_CFG, 0x01, 0x00, 0x01};
    uint8_t reply[64] = {0};
    int ret;

    ret = usb_bulk_sync(fd, EP_OUT, packet, sizeof(packet), 100);
    if (ret != (int)sizeof(packet))
        return -1;

    ret = ch347_read_expected(fd, CH347_SPI_GET_CFG, reply, sizeof(reply),
            CH347_SPI_CFG_LEN + 3, 100, "spi cfg");
    if (ret < 0)
        return -1;

    memcpy(spi_cfg_cache, reply + 3, CH347_SPI_CFG_LEN);
    spi_cfg_valid = 1;
    spi_clock_index = spi_cfg_cache[CH347_SPI_BAUD_OFFSET] >> 3;
    return 0;
}

static int ch347_spi_set_clock(int fd, unsigned int clock_index)
{
    uint8_t packet[3 + CH347_SPI_CFG_LEN] = {0};
    uint8_t reply[64] = {0};
    int ret;

    if (clock_index > 7)
        clock_index = 7;
    if (spi_clock_index == (int)clock_index)
        return 0;

    if (!spi_cfg_valid && ch347_spi_read_cfg(fd) < 0)
        return -1;

    spi_cfg_cache[CH347_SPI_BAUD_OFFSET] = (uint8_t)(clock_index << 3);
    packet[0] = CH347_SPI_SET_CFG;
    packet[1] = CH347_SPI_CFG_LEN;
    packet[2] = 0;
    memcpy(packet + 3, spi_cfg_cache, CH347_SPI_CFG_LEN);

    ret = usb_bulk_sync(fd, EP_OUT, packet, sizeof(packet), 100);
    if (ret != (int)sizeof(packet))
        return -1;

    ret = ch347_read_expected(fd, CH347_SPI_SET_CFG, reply, sizeof(reply),
            4, 100, "spi set cfg");
    if (ret < 0)
        return -1;

    spi_clock_index = (int)clock_index;
    return reply[3] == 0 ? 0 : -1;
}

static int lcd_set_window_for_data(int fd, unsigned int x0, unsigned int y0,
        unsigned int x1, unsigned int y1)
{
    uint8_t col[4];
    uint8_t row[4];

    col[0] = x0 >> 8;
    col[1] = x0 & 0xff;
    col[2] = x1 >> 8;
    col[3] = x1 & 0xff;
    row[0] = y0 >> 8;
    row[1] = y0 & 0xff;
    row[2] = y1 >> 8;
    row[3] = y1 & 0xff;

    if (lcd_cmd_data(fd, 0x2A, col, sizeof(col)) < 0)
        return -1;
    if (lcd_cmd_data(fd, 0x2B, row, sizeof(row)) < 0)
        return -1;
    if (lcd_cmd(fd, 0x2C) < 0)
        return -1;
    if (gpio_dc(fd, 1) < 0)
        return -1;
    return lcd_cs(fd, 1);
}

static int ch347_read_gpio_full(int fd, uint8_t *state, uint8_t raw[8])
{
    uint8_t packet[11] = {0};
    uint8_t reply[64] = {0};
    uint8_t pins = 0;
    int ret;

    packet[0] = 0xCC;
    packet[1] = 0x08;
    packet[2] = 0x00;

    ret = usb_bulk_sync(fd, EP_OUT, packet, sizeof(packet), 100);
    if (ret != (int)sizeof(packet))
        return -1;

    ret = ch347_read_expected(fd, 0xCC, reply, sizeof(reply), 11, 100,
            "touch gpio");
    if (ret < 0)
        return -1;

    for (unsigned int i = 0; i < 8; i++) {
        if (raw)
            raw[i] = reply[3 + i];
        if (reply[3 + i] & 0x40)
            pins |= GPIO_MASK(i);
    }

    if (usb_debug) {
        fprintf(stderr,
                "touch gpio reply=%02x %02x %02x %02x %02x %02x %02x %02x pins=0x%02x\n",
                reply[3], reply[4], reply[5], reply[6], reply[7], reply[8],
                reply[9], reply[10], pins);
    }

    *state = pins;
    return 0;
}

static int ch347_read_gpio(int fd, uint8_t *state)
{
    return ch347_read_gpio_full(fd, state, NULL);
}

static int ch347_touch_select(int fd, int active)
{
    uint8_t packet[13] = {0};
    int ret;

    packet[0] = 0xC1;
    packet[1] = 0x0A;
    packet[2] = 0x00;
    packet[3] = 0xC0;
    packet[8] = active ? 0x80 : 0xC0;

    ret = usb_bulk_sync(fd, EP_OUT, packet, sizeof(packet), 100);
    return ret == (int)sizeof(packet) ? 0 : -1;
}

static int ch347_touch_spi_read(int fd, uint8_t cmd, uint16_t *value)
{
    uint8_t packet[6] = {0xC2, 0x03, 0x00, cmd, 0x00, 0x00};
    uint8_t reply[64] = {0};
    int ret;

    ret = usb_bulk_sync(fd, EP_OUT, packet, sizeof(packet), 100);
    if (ret != (int)sizeof(packet))
        return -1;

    ret = ch347_read_expected(fd, 0xC2, reply, sizeof(reply), 6, 100,
            "touch spi");
    if (ret < 0)
        return -1;

    *value = (uint16_t)(((reply[4] << 8) | reply[5]) >> 3);
    return 0;
}

static int touch_probe_pressure(int fd, unsigned int *pressure,
        const struct touch_state *ts)
{
    uint16_t z1;

    if (gpio_dc(fd, 1) < 0)
        return -1;
    if (ch347_spi_set_clock(fd, ts->touch_clock) < 0)
        return -1;
    if (ch347_touch_select(fd, 1) < 0)
        return -1;
    if (ch347_touch_spi_read(fd, 0xB0, &z1) < 0)
        goto fail;
    if (ch347_touch_select(fd, 0) < 0)
        return -1;
    if (ch347_spi_set_clock(fd, ts->lcd_clock) < 0)
        return -1;
    if (hold_cs && lcd_cs_raw(fd, 1) < 0)
        return -1;

    *pressure = z1;
    return 0;

fail:
    (void)ch347_touch_select(fd, 0);
    (void)ch347_spi_set_clock(fd, ts->lcd_clock);
    if (hold_cs)
        (void)lcd_cs_raw(fd, 1);
    return -1;
}

static int touch_read_raw(int fd, unsigned int *raw_x, unsigned int *raw_y,
        unsigned int *raw_z1, unsigned int *raw_z2,
        const struct touch_state *ts)
{
    uint16_t x[7];
    uint16_t y[7];
    uint16_t z1[7];
    uint16_t z2[7];
    uint16_t valid_x[7];
    uint16_t valid_y[7];
    uint16_t valid_z1[7];
    uint16_t valid_z2[7];
    uint32_t sx = 0;
    uint32_t sy = 0;
    uint32_t sz1 = 0;
    uint32_t sz2 = 0;
    unsigned int valid = 0;
    unsigned int first;
    unsigned int last;
    unsigned int kept;

    if (gpio_dc(fd, 1) < 0)
        return -1;
    if (ch347_spi_set_clock(fd, ts->touch_clock) < 0)
        return -1;
    if (ch347_touch_select(fd, 1) < 0)
        return -1;

    for (unsigned int i = 0; i < 7; i++) {
        if (ch347_touch_spi_read(fd, 0xD0, &x[i]) < 0)
            goto fail;
        if (ch347_touch_spi_read(fd, 0x90, &y[i]) < 0)
            goto fail;
        if (ch347_touch_spi_read(fd, 0xB0, &z1[i]) < 0)
            goto fail;
        if (ch347_touch_spi_read(fd, 0xC0, &z2[i]) < 0)
            goto fail;
    }

    if (ch347_touch_select(fd, 0) < 0)
        return -1;
    if (ch347_spi_set_clock(fd, ts->lcd_clock) < 0)
        return -1;
    if (hold_cs && lcd_cs_raw(fd, 1) < 0)
        return -1;

    for (unsigned int i = 0; i < 7; i++) {
        if (!ts->calibrate && z1[i] < ts->z_min)
            continue;
        valid_x[valid] = x[i];
        valid_y[valid] = y[i];
        valid_z1[valid] = z1[i];
        valid_z2[valid] = z2[i];
        valid++;
    }
    if (!valid)
        return 0;

    sort_u16(valid_x, valid);
    sort_u16(valid_y, valid);
    sort_u16(valid_z1, valid);
    sort_u16(valid_z2, valid);

    /* Trim both extremes. A single bad USB/ADC sample must not move the pointer. */
    if (valid >= 7) {
        first = 2;
        last = 5;
    } else {
        first = valid >= 5 ? 1 : 0;
        last = valid >= 5 ? valid - 1 : valid;
    }

    for (unsigned int i = first; i < last; i++) {
        sx += valid_x[i];
        sy += valid_y[i];
        sz1 += valid_z1[i];
        sz2 += valid_z2[i];
    }
    kept = last - first;

    *raw_x = (sx + kept / 2) / kept;
    *raw_y = (sy + kept / 2) / kept;
    *raw_z1 = (sz1 + kept / 2) / kept;
    *raw_z2 = (sz2 + kept / 2) / kept;
    if (usb_debug) {
        fprintf(stderr, "touch raw sample x=%u y=%u z1=%u z2=%u valid=%u\n",
                *raw_x, *raw_y, *raw_z1, *raw_z2, valid);
    }
    return 1;

fail:
    (void)ch347_touch_select(fd, 0);
    (void)ch347_spi_set_clock(fd, ts->lcd_clock);
    if (hold_cs)
        (void)lcd_cs_raw(fd, 1);
    return -1;
}

static void touch_map(const struct touch_state *ts, unsigned int raw_x,
        unsigned int raw_y, int *out_x, int *out_y)
{
    unsigned int rx = raw_x;
    unsigned int ry = raw_y;
    unsigned int xmin = ts->x_min;
    unsigned int xmax = ts->x_max;
    unsigned int ymin = ts->y_min;
    unsigned int ymax = ts->y_max;
    long x;
    long y;

    if (ts->swap_xy) {
        unsigned int tmp = rx;

        rx = ry;
        ry = tmp;
    }

    if (xmax <= xmin)
        xmax = xmin + 1;
    if (ymax <= ymin)
        ymax = ymin + 1;

    if (rx < xmin)
        rx = xmin;
    if (rx > xmax)
        rx = xmax;
    if (ry < ymin)
        ry = ymin;
    if (ry > ymax)
        ry = ymax;

    x = (long)(rx - xmin) * (long)(ts->width - 1) / (long)(xmax - xmin);
    y = (long)(ry - ymin) * (long)(ts->height - 1) / (long)(ymax - ymin);

    if (ts->invert_x)
        x = (long)(ts->width - 1) - x;
    if (ts->invert_y)
        y = (long)(ts->height - 1) - y;

    *out_x = (int)x;
    *out_y = (int)y;
}

static void touch_config_cal_points(struct touch_state *ts)
{
    unsigned int m = ts->cal_margin;
    unsigned int max_x = ts->width - 1;
    unsigned int max_y = ts->height - 1;

    if (m >= ts->width / 2)
        m = ts->width / 5;
    if (m >= ts->height / 2)
        m = ts->height / 5;

    ts->cal_sx[0] = (int)m;
    ts->cal_sy[0] = (int)m;
    ts->cal_sx[1] = (int)(max_x - m);
    ts->cal_sy[1] = (int)m;
    ts->cal_sx[2] = (int)(max_x - m);
    ts->cal_sy[2] = (int)(max_y - m);
    ts->cal_sx[3] = (int)m;
    ts->cal_sy[3] = (int)(max_y - m);
    ts->cal_sx[4] = (int)(max_x / 2);
    ts->cal_sy[4] = (int)(max_y / 2);
}

static int touch_calibrating_pressure(const struct touch_state *ts)
{
    return ts->cal_index >= CAL_POS_POINTS && ts->cal_press_index < CAL_PRESS_POINTS;
}

static int touch_cal_complete(const struct touch_state *ts)
{
    return ts->cal_index >= CAL_POS_POINTS &&
        ts->cal_press_index >= CAL_PRESS_POINTS;
}

static unsigned int clamp_raw(long v)
{
    if (v < 0)
        return 0;
    if (v > 4095)
        return 4095;
    return (unsigned int)v;
}

static long extrapolate_raw_edge(long raw_a, long raw_b,
        long screen_a, long screen_b, long screen_edge)
{
    long den = screen_b - screen_a;

    if (!den)
        return raw_a;

    return raw_a + (raw_b - raw_a) * (screen_edge - screen_a) / den;
}

static void touch_solve_calibration(struct touch_state *ts)
{
    long hdx = labs((long)(ts->cal_raw_x[1] + ts->cal_raw_x[2]) / 2 -
            (long)(ts->cal_raw_x[0] + ts->cal_raw_x[3]) / 2);
    long hdy = labs((long)(ts->cal_raw_y[1] + ts->cal_raw_y[2]) / 2 -
            (long)(ts->cal_raw_y[0] + ts->cal_raw_y[3]) / 2);
    long vdx = labs((long)(ts->cal_raw_x[2] + ts->cal_raw_x[3]) / 2 -
            (long)(ts->cal_raw_x[0] + ts->cal_raw_x[1]) / 2);
    long vdy = labs((long)(ts->cal_raw_y[2] + ts->cal_raw_y[3]) / 2 -
            (long)(ts->cal_raw_y[0] + ts->cal_raw_y[1]) / 2);
    unsigned int ax0;
    unsigned int ax1;
    unsigned int ax2;
    unsigned int ax3;
    unsigned int ay0;
    unsigned int ay1;
    unsigned int ay2;
    unsigned int ay3;
    unsigned int left;
    unsigned int right;
    unsigned int top;
    unsigned int bottom;
    long left_edge;
    long right_edge;
    long top_edge;
    long bottom_edge;
    long max_x = (long)ts->width - 1;
    long max_y = (long)ts->height - 1;

    ts->swap_xy = hdy > hdx && vdx > vdy;

    ax0 = ts->swap_xy ? ts->cal_raw_y[0] : ts->cal_raw_x[0];
    ax1 = ts->swap_xy ? ts->cal_raw_y[1] : ts->cal_raw_x[1];
    ax2 = ts->swap_xy ? ts->cal_raw_y[2] : ts->cal_raw_x[2];
    ax3 = ts->swap_xy ? ts->cal_raw_y[3] : ts->cal_raw_x[3];
    ay0 = ts->swap_xy ? ts->cal_raw_x[0] : ts->cal_raw_y[0];
    ay1 = ts->swap_xy ? ts->cal_raw_x[1] : ts->cal_raw_y[1];
    ay2 = ts->swap_xy ? ts->cal_raw_x[2] : ts->cal_raw_y[2];
    ay3 = ts->swap_xy ? ts->cal_raw_x[3] : ts->cal_raw_y[3];

    left = (ax0 + ax3) / 2;
    right = (ax1 + ax2) / 2;
    top = (ay0 + ay1) / 2;
    bottom = (ay2 + ay3) / 2;

    /*
     * The corner targets are intentionally inset from the physical edge.
     * Extrapolate those raw readings back to screen edge 0..max, otherwise
     * the inset target gets incorrectly treated as the actual corner.
     */
    left_edge = extrapolate_raw_edge(left, right,
            ts->cal_sx[0], ts->cal_sx[1], 0);
    right_edge = extrapolate_raw_edge(left, right,
            ts->cal_sx[0], ts->cal_sx[1], max_x);
    top_edge = extrapolate_raw_edge(top, bottom,
            ts->cal_sy[0], ts->cal_sy[3], 0);
    bottom_edge = extrapolate_raw_edge(top, bottom,
            ts->cal_sy[0], ts->cal_sy[3], max_y);

    ts->invert_x = left_edge > right_edge;
    ts->invert_y = top_edge > bottom_edge;
    ts->x_min = clamp_raw(ts->invert_x ? right_edge : left_edge);
    ts->x_max = clamp_raw(ts->invert_x ? left_edge : right_edge);
    ts->y_min = clamp_raw(ts->invert_y ? bottom_edge : top_edge);
    ts->y_max = clamp_raw(ts->invert_y ? top_edge : bottom_edge);
}

static void touch_write_calibration(struct touch_state *ts)
{
    FILE *fp;
    unsigned int z_min = ts->z_min;
    unsigned int pressure_min = ts->pressure_min;
    unsigned int pressure_max = ts->pressure_min;

    if (!ts->cal_file || !*ts->cal_file)
        return;

    fp = fopen(ts->cal_file, "w");
    if (!fp) {
        fprintf(stderr, "touch calibration write %s failed: %s\n",
                ts->cal_file, strerror(errno));
        return;
    }

    fprintf(fp, "# Generated by ch347_dirty_usb_sink touch calibration\n");
    fprintf(fp, "CH347_TOUCH=1\n");
    fprintf(fp, "CH347_TOUCH_SWAP_XY=%d\n", ts->swap_xy);
    fprintf(fp, "CH347_TOUCH_INVERT_X=%d\n", ts->invert_x);
    fprintf(fp, "CH347_TOUCH_INVERT_Y=%d\n", ts->invert_y);
    fprintf(fp, "CH347_TOUCH_X_MIN=%u\n", ts->x_min);
    fprintf(fp, "CH347_TOUCH_X_MAX=%u\n", ts->x_max);
    fprintf(fp, "CH347_TOUCH_Y_MIN=%u\n", ts->y_min);
    fprintf(fp, "CH347_TOUCH_Y_MAX=%u\n", ts->y_max);
    fprintf(fp, "CH347_TOUCH_WIDTH=%u\n", ts->width);
    fprintf(fp, "CH347_TOUCH_HEIGHT=%u\n", ts->height);
    if (touch_cal_complete(ts)) {
        z_min = ts->cal_raw_z1[0];
        pressure_min = ts->cal_press_pressure[0];
        pressure_max = ts->cal_press_pressure[1];

        for (unsigned int i = 1; i < CAL_POS_POINTS; i++) {
            if (ts->cal_raw_z1[i] < z_min)
                z_min = ts->cal_raw_z1[i];
        }
        for (unsigned int i = 0; i < CAL_PRESS_POINTS; i++) {
            if (ts->cal_press_z1[i] < z_min)
                z_min = ts->cal_press_z1[i];
            if ((i & 1) == 0 && ts->cal_press_pressure[i] < pressure_min)
                pressure_min = ts->cal_press_pressure[i];
            if ((i & 1) == 1 && ts->cal_press_pressure[i] > pressure_max)
                pressure_max = ts->cal_press_pressure[i];
        }
        z_min = z_min > TOUCH_PRESSURE_FLOOR ? z_min : TOUCH_PRESSURE_FLOOR;
        if (z_min > TOUCH_PRESSURE_FLOOR * 2)
            pressure_min = z_min * 3 / 4;
        else
            pressure_min = TOUCH_PRESSURE_FLOOR;
    }
    ts->z_min = z_min;
    ts->pressure_min = pressure_min;
    fprintf(fp, "CH347_TOUCH_Z_MIN=%u\n", z_min);
    fprintf(fp, "CH347_TOUCH_PRESSURE_MIN=%u\n", pressure_min);
    fprintf(fp, "CH347_TOUCH_PRESSURE_MAX=%u\n", pressure_max);
    fclose(fp);
}

static int x11_touch_init(struct touch_state *ts)
{
    struct x11_touch_api *x11 = &ts->x11;

    x11->x11_lib = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
    x11->xtst_lib = dlopen("libXtst.so.6", RTLD_NOW | RTLD_LOCAL);
    if (!x11->x11_lib || !x11->xtst_lib)
        return -1;

    x11->XOpenDisplay = dlsym(x11->x11_lib, "XOpenDisplay");
    x11->XCloseDisplay = dlsym(x11->x11_lib, "XCloseDisplay");
    x11->XFlush = dlsym(x11->x11_lib, "XFlush");
    x11->XDefaultScreen = dlsym(x11->x11_lib, "XDefaultScreen");
    x11->XTestFakeMotionEvent = dlsym(x11->xtst_lib, "XTestFakeMotionEvent");
    x11->XTestFakeButtonEvent = dlsym(x11->xtst_lib, "XTestFakeButtonEvent");
    if (!x11->XOpenDisplay || !x11->XCloseDisplay || !x11->XFlush ||
            !x11->XDefaultScreen || !x11->XTestFakeMotionEvent ||
            !x11->XTestFakeButtonEvent)
        return -1;

    x11->display = x11->XOpenDisplay(ts->display_name);
    if (!x11->display)
        return -1;

    x11->screen = x11->XDefaultScreen(x11->display);
    return 0;
}

static void x11_touch_close(struct touch_state *ts)
{
    struct x11_touch_api *x11 = &ts->x11;

    if (x11->display && x11->XCloseDisplay)
        x11->XCloseDisplay(x11->display);
    if (x11->xtst_lib)
        dlclose(x11->xtst_lib);
    if (x11->x11_lib)
        dlclose(x11->x11_lib);
    memset(x11, 0, sizeof(*x11));
}

static void x11_touch_motion(struct touch_state *ts, int x, int y)
{
    struct x11_touch_api *x11 = &ts->x11;
    int logical_x;
    int logical_y;

    if (!x11->display)
        return;
    frame_rotation_unmap_point(ts->rotation, ts->width, ts->height, x, y,
            &logical_x, &logical_y);
    x11->XTestFakeMotionEvent(x11->display, x11->screen, logical_x,
            logical_y, 0);
    x11->XFlush(x11->display);
}

static void x11_touch_button(struct touch_state *ts, int press)
{
    struct x11_touch_api *x11 = &ts->x11;

    if (!x11->display)
        return;
    x11->XTestFakeButtonEvent(x11->display, 1, press, 0);
    x11->XFlush(x11->display);
}

static const uint8_t touch_hid_descriptor[] = {
    0x05, 0x0d, 0x09, 0x04, 0xa1, 0x01,
    0x09, 0x22, 0xa1, 0x02,
    0x09, 0x42, 0x09, 0x32, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x02, 0x81, 0x02,
    0x95, 0x06, 0x81, 0x03,
    0x09, 0x51, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31,
    0x16, 0x00, 0x00, 0x26, 0xff, 0x7f,
    0x75, 0x10, 0x95, 0x02, 0x81, 0x02,
    0xc0,
    0x05, 0x0d, 0x09, 0x54, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x08, 0x95, 0x01, 0x81, 0x02,
    0xc0,
};

static int touch_uhid_write(int fd, const struct uhid_event *event)
{
    ssize_t done;

    do {
        done = write(fd, event, sizeof(*event));
    } while (done < 0 && errno == EINTR);
    return done == (ssize_t)sizeof(*event) ? 0 : -1;
}

static int touch_uhid_init(struct touch_state *ts)
{
    struct uhid_event event;

    ts->uhid.fd = open("/dev/uhid", O_RDWR | O_CLOEXEC);
    if (ts->uhid.fd < 0)
        return -1;

    memset(&event, 0, sizeof(event));
    event.type = UHID_CREATE2;
    snprintf((char *)event.u.create2.name, sizeof(event.u.create2.name),
            "CH347 XPT2046 Touchscreen");
    snprintf((char *)event.u.create2.phys, sizeof(event.u.create2.phys),
            "ch347/xpt2046");
    snprintf((char *)event.u.create2.uniq, sizeof(event.u.create2.uniq),
            "CH347-XPT2046-1");
    event.u.create2.rd_size = sizeof(touch_hid_descriptor);
    event.u.create2.bus = BUS_USB;
    event.u.create2.vendor = 0x1a86;
    event.u.create2.product = 0x3470;
    event.u.create2.version = 1;
    memcpy(event.u.create2.rd_data, touch_hid_descriptor,
            sizeof(touch_hid_descriptor));
    if (touch_uhid_write(ts->uhid.fd, &event) < 0) {
        close(ts->uhid.fd);
        ts->uhid.fd = -1;
        return -1;
    }
    return 0;
}

static void touch_uhid_report(struct touch_state *ts, int x, int y, int down)
{
    struct uhid_event event;
    uint16_t hx;
    uint16_t hy;
    int logical_x;
    int logical_y;

    if (ts->uhid.fd < 0)
        return;
    frame_rotation_unmap_point(ts->rotation, ts->width, ts->height, x, y,
            &logical_x, &logical_y);
    if (ts->debug)
        fprintf(stderr,
                "touch UHID report down=%d physical=%d,%d logical=%d,%d rotation=%s\n",
                down, x, y, logical_x, logical_y,
                frame_rotation_name(ts->rotation));
    x = logical_x;
    y = logical_y;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if ((unsigned int)x >= ts->logical_width)
        x = (int)ts->logical_width - 1;
    if ((unsigned int)y >= ts->logical_height)
        y = (int)ts->logical_height - 1;
    hx = ts->logical_width > 1 ?
        (uint16_t)((uint32_t)x * 32767 / (ts->logical_width - 1)) : 0;
    hy = ts->logical_height > 1 ?
        (uint16_t)((uint32_t)y * 32767 / (ts->logical_height - 1)) : 0;

    memset(&event, 0, sizeof(event));
    event.type = UHID_INPUT2;
    event.u.input2.size = 6;
    event.u.input2.data[0] = down ? 0x03 : 0x00;
    event.u.input2.data[1] = down ? 1 : 0;
    event.u.input2.data[2] = hx & 0xff;
    event.u.input2.data[3] = hx >> 8;
    event.u.input2.data[4] = hy & 0xff;
    event.u.input2.data[5] = hy >> 8;
    if (touch_uhid_write(ts->uhid.fd, &event) < 0 && ts->debug)
        fprintf(stderr, "touch UHID report failed: %s\n", strerror(errno));
    ts->uhid.active = down;
}

static void touch_uhid_close(struct touch_state *ts)
{
    struct uhid_event event;

    if (ts->uhid.fd < 0)
        return;
    if (ts->uhid.active)
        touch_uhid_report(ts, ts->last_x, ts->last_y, 0);
    memset(&event, 0, sizeof(event));
    event.type = UHID_DESTROY;
    (void)touch_uhid_write(ts->uhid.fd, &event);
    close(ts->uhid.fd);
    ts->uhid.fd = -1;
    ts->uhid.active = 0;
}

static void touch_set_input_mode(struct touch_state *ts,
        enum touch_input_mode mode)
{
    if (mode == ts->input_mode)
        return;

    if (ts->down) {
        if (ts->input_mode == TOUCH_MODE_TOUCH)
            touch_uhid_report(ts, ts->last_x, ts->last_y, 0);
        else
            x11_touch_button(ts, 0);
    }
    ts->down = 0;
    ts->filter_valid = 0;
    ts->release_count = 0;
    if (mode == TOUCH_MODE_MOUSE && !ts->x11.display &&
            x11_touch_init(ts) < 0) {
        fprintf(stderr, "touch mode switch to mouse failed: no XTest display\n");
        mode = TOUCH_MODE_TOUCH;
    }
    if (mode == TOUCH_MODE_TOUCH && ts->uhid.fd < 0 &&
            touch_uhid_init(ts) < 0) {
        fprintf(stderr, "touch mode switch to touch failed: %s\n",
                strerror(errno));
        mode = TOUCH_MODE_MOUSE;
    }
    ts->input_mode = mode;
    if (mode == TOUCH_MODE_TOUCH)
        ts->cursor_visible = 0;
    fprintf(stderr, "touch input mode=%s\n", touch_mode_name(mode));
}

static void touch_check_input_mode(struct touch_state *ts, double now)
{
    char value[32];
    int fd;
    ssize_t len;

    if (!ts->mode_file || !*ts->mode_file ||
            now - ts->last_mode_check < 0.10)
        return;
    ts->last_mode_check = now;

    fd = open(ts->mode_file, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    len = read(fd, value, sizeof(value) - 1);
    close(fd);
    if (len <= 0)
        return;
    value[len] = 0;
    touch_set_input_mode(ts, touch_mode_parse(value, ts->input_mode));
}

static void touch_release(struct touch_state *ts)
{
    if (ts->cal_wait_release) {
        if (++ts->release_count >= ts->release_samples) {
            ts->cal_wait_release = 0;
            ts->cal_sample_count = 0;
            ts->cal_sum_x = 0;
            ts->cal_sum_y = 0;
            ts->cal_sum_z1 = 0;
            ts->cal_sum_z2 = 0;
            ts->cal_sum_pressure = 0;
        }
    }
    if (ts->down) {
        if (!ts->calibrate) {
            if (ts->input_mode == TOUCH_MODE_TOUCH)
                touch_uhid_report(ts, ts->last_x, ts->last_y, 0);
            else
                x11_touch_button(ts, 0);
        }
        ts->down = 0;
        ts->filter_valid = 0;
        if (ts->input_mode == TOUCH_MODE_TOUCH)
            ts->cursor_visible = 0;
    }
}

static int touch_poll(int fd, struct touch_state *ts, double now)
{
    unsigned int raw_x;
    unsigned int raw_y;
    unsigned int raw_z1;
    unsigned int raw_z2;
    unsigned int pressure;
    int x;
    int y;
    int irq_active = 1;
    int ret;

    if (!ts->enabled)
        return 0;
    touch_check_input_mode(ts, now);
    if (now - ts->last_poll < (double)ts->poll_ms / 1000.0)
        return 0;
    ts->last_poll = now;

    if (ts->use_irq) {
        uint8_t gpio = 0xff;

        if (ch347_read_gpio(fd, &gpio) < 0) {
            if (++ts->errors >= ts->max_errors && ts->disable_on_errors)
                ts->enabled = 0;
            if (ts->debug)
                fprintf(stderr, "touch gpio read failed errors=%u/%u enabled=%d\n",
                        ts->errors, ts->max_errors, ts->enabled);
            return 0;
        }

        irq_active = !(gpio & GPIO_MASK(GPIO_TOUCH_IRQ));
        if (ts->debug)
            fprintf(stderr, "touch irq gpio=0x%02x active=%d down=%d wait=%d\n",
                    gpio, irq_active, ts->down, ts->cal_wait_release);
        if (!irq_active) {
            int was_down = ts->down;
            int was_wait = ts->cal_wait_release;

            touch_release(ts);
            if (ts->calibrate && (was_down || was_wait))
                return 1;
            return 0;
        }
    }

    if (!ts->down && !ts->calibrate) {
        unsigned int probe_pressure;

        ret = touch_probe_pressure(fd, &probe_pressure, ts);
        if (ret < 0) {
            if (++ts->errors >= ts->max_errors && ts->disable_on_errors)
                ts->enabled = 0;
            return 0;
        }
        if (ts->z_strict && probe_pressure < ts->pressure_min) {
            touch_release(ts);
            return 0;
        }
    }

    ret = touch_read_raw(fd, &raw_x, &raw_y, &raw_z1, &raw_z2, ts);
    if (ret < 0) {
        if (++ts->errors >= ts->max_errors && ts->disable_on_errors)
            ts->enabled = 0;
        if (ts->debug)
            fprintf(stderr, "touch raw read failed errors=%u/%u enabled=%d\n",
                    ts->errors, ts->max_errors, ts->enabled);
        return 0;
    }
    if (ret == 0) {
        int was_down = ts->down;

        touch_release(ts);
        if (was_down)
            return 1;
        if (ts->debug)
            fprintf(stderr, "touch no valid sample\n");
        return 0;
    }

    pressure = touch_pressure(raw_z1, raw_z2);
    touch_map(ts, raw_x, raw_y, &x, &y);

    if (!ts->calibrate) {
        if (!ts->filter_valid || !ts->down) {
            ts->filtered_x = x;
            ts->filtered_y = y;
            ts->filter_valid = 1;
        } else {
            int dx = x - ts->filtered_x;
            int dy = y - ts->filtered_y;
            unsigned int distance = (unsigned int)(abs_int(dx) + abs_int(dy));

            /* Isolated large jumps are typical XPT2046 edge/SPI noise. */
            if (distance > ts->jump_thresh) {
                /* Re-anchor instead of rejecting forever; this preserves fast drags. */
                ts->filtered_x = x;
                ts->filtered_y = y;
            } else {
                ts->filtered_x = (ts->filtered_x * (int)ts->filter_weight + x) /
                    (int)(ts->filter_weight + 1);
                ts->filtered_y = (ts->filtered_y * (int)ts->filter_weight + y) /
                    (int)(ts->filter_weight + 1);
            }
        }
        x = ts->filtered_x;
        y = ts->filtered_y;
    }
    ts->raw_valid = 1;
    ts->last_raw_x = raw_x;
    ts->last_raw_y = raw_y;
    ts->last_raw_z1 = raw_z1;
    ts->last_raw_z2 = raw_z2;
    ts->last_pressure = pressure;
    ts->last_screen_x = x;
    ts->last_screen_y = y;

    if (ts->z_strict && pressure < ts->pressure_min) {
        int was_down = ts->down;

        touch_release(ts);
        if (was_down)
            return 1;
        if (ts->debug) {
            fprintf(stderr, "touch no press z1=%u z2=%u p=%u min=%u\n",
                    raw_z1, raw_z2, pressure, ts->pressure_min);
        }
        return ts->calibrate ? 1 : 0;
    }

    if (ts->z_strict && ts->cal_wait_release) {
        if (ts->debug) {
            fprintf(stderr, "touch wait release before next point z1=%u z2=%u p=%u min=%u\n",
                    raw_z1, raw_z2, pressure, ts->pressure_min);
        }
        return ts->calibrate ? 1 : 0;
    }

    ts->errors = 0;
    ts->release_count = 0;

    if (ts->calibrate) {
        if (!ts->cal_done && !ts->cal_wait_release &&
                (ts->cal_index < CAL_POS_POINTS ||
                 ts->cal_press_index < CAL_PRESS_POINTS)) {
            ts->cal_sum_x += raw_x;
            ts->cal_sum_y += raw_y;
            ts->cal_sum_z1 += raw_z1;
            ts->cal_sum_z2 += raw_z2;
            ts->cal_sum_pressure += pressure;
            ts->cal_sample_count++;
            if (ts->cal_sample_count < ts->cal_samples_required) {
                if (ts->debug) {
                    fprintf(stderr,
                            "touch_cal sampling pos=%u/%u pressure=%u/%u sample=%u/%u raw=%u,%u z=%u,%u p=%u\n",
                            ts->cal_index + 1, CAL_POS_POINTS,
                            ts->cal_press_index + 1, CAL_PRESS_POINTS,
                            ts->cal_sample_count,
                            ts->cal_samples_required, raw_x, raw_y,
                            raw_z1, raw_z2, pressure);
                }
                return 1;
            }

            if (ts->cal_index < CAL_POS_POINTS) {
                ts->cal_raw_x[ts->cal_index] =
                    (unsigned int)((ts->cal_sum_x + ts->cal_sample_count / 2) /
                            ts->cal_sample_count);
                ts->cal_raw_y[ts->cal_index] =
                    (unsigned int)((ts->cal_sum_y + ts->cal_sample_count / 2) /
                            ts->cal_sample_count);
                ts->cal_raw_z1[ts->cal_index] =
                    (unsigned int)((ts->cal_sum_z1 + ts->cal_sample_count / 2) /
                            ts->cal_sample_count);
                ts->cal_raw_z2[ts->cal_index] =
                    (unsigned int)((ts->cal_sum_z2 + ts->cal_sample_count / 2) /
                            ts->cal_sample_count);
                ts->cal_raw_pressure[ts->cal_index] =
                    (unsigned int)((ts->cal_sum_pressure +
                                ts->cal_sample_count / 2) /
                            ts->cal_sample_count);
                fprintf(stderr, "touch_cal point=%u screen=%d,%d raw=%u,%u z=%u,%u p=%u samples=%u\n",
                        ts->cal_index + 1, ts->cal_sx[ts->cal_index],
                        ts->cal_sy[ts->cal_index],
                        ts->cal_raw_x[ts->cal_index],
                        ts->cal_raw_y[ts->cal_index],
                        ts->cal_raw_z1[ts->cal_index],
                        ts->cal_raw_z2[ts->cal_index],
                        ts->cal_raw_pressure[ts->cal_index],
                        ts->cal_sample_count);
                ts->cal_index++;
            } else {
                ts->cal_press_z1[ts->cal_press_index] =
                    (unsigned int)((ts->cal_sum_z1 + ts->cal_sample_count / 2) /
                            ts->cal_sample_count);
                ts->cal_press_z2[ts->cal_press_index] =
                    (unsigned int)((ts->cal_sum_z2 + ts->cal_sample_count / 2) /
                            ts->cal_sample_count);
                ts->cal_press_pressure[ts->cal_press_index] =
                    (unsigned int)((ts->cal_sum_pressure +
                                ts->cal_sample_count / 2) /
                            ts->cal_sample_count);
                fprintf(stderr, "touch_cal pressure=%u/%u z=%u,%u p=%u samples=%u\n",
                        ts->cal_press_index + 1, CAL_PRESS_POINTS,
                        ts->cal_press_z1[ts->cal_press_index],
                        ts->cal_press_z2[ts->cal_press_index],
                        ts->cal_press_pressure[ts->cal_press_index],
                        ts->cal_sample_count);
                ts->cal_press_index++;
            }
            ts->cal_wait_release = 1;
            ts->cal_sample_count = 0;
            ts->cal_sum_x = 0;
            ts->cal_sum_y = 0;
            ts->cal_sum_z1 = 0;
            ts->cal_sum_z2 = 0;
            ts->cal_sum_pressure = 0;
            if (ts->cal_index == CAL_POS_POINTS &&
                    ts->cal_press_index == 0) {
                touch_solve_calibration(ts);
            }
            if (touch_cal_complete(ts)) {
                touch_solve_calibration(ts);
                touch_write_calibration(ts);
                ts->cal_done = 1;
                ts->cal_done_time = now;
                fprintf(stderr,
                        "touch_cal done x=%u..%u y=%u..%u invert=%d,%d p_min=%u p_max=%u file=%s\n",
                        ts->x_min, ts->x_max, ts->y_min, ts->y_max,
                        ts->invert_x, ts->invert_y, ts->pressure_min,
                        ts->cal_press_pressure[1],
                        ts->cal_file ? ts->cal_file : "");
            }
            return 1;
        }
        return 0;
    }

    ts->cursor_x = x;
    ts->cursor_y = y;
    if (!ts->cursor_visible) {
        ts->cursor_visible = 1;
        ts->last_x = x;
        ts->last_y = y;
    }

    if (!ts->down) {
        if (ts->input_mode == TOUCH_MODE_TOUCH)
            touch_uhid_report(ts, x, y, 1);
        else {
            x11_touch_motion(ts, x, y);
            x11_touch_button(ts, 1);
        }
        ts->down = 1;
        ts->last_x = x;
        ts->last_y = y;
        return 1;
    } else if (abs_int(x - ts->last_x) >= (int)ts->move_thresh ||
            abs_int(y - ts->last_y) >= (int)ts->move_thresh) {
        if (ts->input_mode == TOUCH_MODE_TOUCH)
            touch_uhid_report(ts, x, y, 1);
        else
            x11_touch_motion(ts, x, y);
        ts->last_x = x;
        ts->last_y = y;
        return 1;
    }

    if (ts->debug) {
        fprintf(stderr, "touch raw=%u,%u z=%u,%u p=%u xy=%d,%d irq=%d down=%d\n",
                raw_x, raw_y, raw_z1, raw_z2, pressure, x, y, irq_active,
                ts->down);
    }
    return 0;
}

static void swap16_frame(uint8_t *frame)
{
    for (size_t i = 0; i < FRAME_BYTES; i += 2) {
        uint8_t tmp = frame[i];

        frame[i] = frame[i + 1];
        frame[i + 1] = tmp;
    }
}

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static void put_pixel(uint8_t *frame, int x, int y, uint16_t color)
{
    size_t off;

    if (x < 0 || y < 0 || x >= LCD_WIDTH || y >= LCD_HEIGHT)
        return;

    off = ((size_t)y * LCD_WIDTH + x) * 2;
    frame[off] = color >> 8;
    frame[off + 1] = color & 0xff;
}

static void put_pixel_alpha(uint8_t *frame, int x, int y, uint16_t color,
        unsigned int alpha)
{
    size_t off;
    uint16_t old;
    unsigned int inverse;
    unsigned int red;
    unsigned int green;
    unsigned int blue;

    if (!alpha || x < 0 || y < 0 || x >= LCD_WIDTH || y >= LCD_HEIGHT)
        return;
    if (alpha >= 255) {
        put_pixel(frame, x, y, color);
        return;
    }
    off = ((size_t)y * LCD_WIDTH + x) * 2;
    old = (uint16_t)((frame[off] << 8) | frame[off + 1]);
    inverse = 255u - alpha;
    red = (((old >> 11) & 0x1fu) * inverse +
            ((color >> 11) & 0x1fu) * alpha + 127u) / 255u;
    green = (((old >> 5) & 0x3fu) * inverse +
            ((color >> 5) & 0x3fu) * alpha + 127u) / 255u;
    blue = ((old & 0x1fu) * inverse + (color & 0x1fu) * alpha + 127u) /
        255u;
    old = (uint16_t)((red << 11) | (green << 5) | blue);
    frame[off] = old >> 8;
    frame[off + 1] = old & 0xff;
}

static void draw_glyph_scaled(uint8_t *frame, int x, int y, char c,
        uint16_t color, unsigned int scale, unsigned int alpha)
{
    static const uint8_t digits[10][7] = {
        {0x0e,0x11,0x13,0x15,0x19,0x11,0x0e},
        {0x04,0x0c,0x04,0x04,0x04,0x04,0x0e},
        {0x0e,0x11,0x01,0x02,0x04,0x08,0x1f},
        {0x1e,0x01,0x01,0x0e,0x01,0x01,0x1e},
        {0x02,0x06,0x0a,0x12,0x1f,0x02,0x02},
        {0x1f,0x10,0x10,0x1e,0x01,0x01,0x1e},
        {0x0e,0x10,0x10,0x1e,0x11,0x11,0x0e},
        {0x1f,0x01,0x02,0x04,0x08,0x08,0x08},
        {0x0e,0x11,0x11,0x0e,0x11,0x11,0x0e},
        {0x0e,0x11,0x11,0x0f,0x01,0x01,0x0e},
    };
    const uint8_t *glyph = NULL;

    if (c >= '0' && c <= '9')
        glyph = digits[c - '0'];

    for (int yy = 0; yy < 7; yy++) {
        for (int xx = 0; xx < 5; xx++) {
            int on = 0;

            if (glyph)
                on = glyph[yy] & (1 << (4 - xx));
            else if (c == '.')
                on = yy >= 5 && xx == 2;
            else if (c == ':')
                on = (yy == 2 || yy == 4) && xx == 2;
            else if (c == '%')
                on = (xx == yy && yy < 5) || (xx == 4 - yy && yy > 1);
            else if (c == 'B')
                on = xx == 0 || yy == 0 || yy == 3 || yy == 6 ||
                    (xx == 4 && yy > 0 && yy < 6);
            else if (c == 'D')
                on = xx == 0 || yy == 0 || yy == 6 ||
                    (xx == 4 && yy > 0 && yy < 6);
            else if (c == 'F')
                on = xx == 0 || yy == 0 || yy == 3;
            else if (c == 'G')
                on = yy == 0 || yy == 6 || xx == 0 ||
                    (xx == 4 && yy >= 3) || (yy == 3 && xx >= 2);
            else if (c == 'P')
                on = xx == 0 || yy == 0 || yy == 3 ||
                    (xx == 4 && yy > 0 && yy < 3);
            else if (c == 'Q')
                on = yy == 0 || yy == 5 || xx == 0 || xx == 4 ||
                    (xx == yy - 2 && yy >= 3);
            else if (c == 'R')
                on = xx == 0 || yy == 0 || yy == 3 ||
                    (xx == 4 && yy > 0 && yy < 3) || (yy > 3 && xx == yy - 2);
            else if (c == 'S')
                on = yy == 0 || yy == 3 || yy == 6 ||
                    (xx == 0 && yy < 3) || (xx == 4 && yy > 3);
            else if (c == 'T')
                on = yy == 0 || xx == 2;
            else if (c == ' ')
                on = 0;
            else if (c == 'A')
                on = yy == 0 || yy == 3 || xx == 0 || xx == 4;
            else if (c == 'C')
                on = yy == 0 || yy == 6 || xx == 0;
            else if (c == 'E')
                on = yy == 0 || yy == 3 || yy == 6 || xx == 0;
            else if (c == 'H')
                on = xx == 0 || xx == 4 || yy == 3;
            else if (c == 'I')
                on = yy == 0 || yy == 6 || xx == 2;
            else if (c == 'L')
                on = xx == 0 || yy == 6;
            else if (c == 'K')
                on = xx == 0 || xx == 4 - abs_int(yy - 3);
            else if (c == 'M')
                on = xx == 0 || xx == 4 ||
                    (yy < 3 && (xx == yy + 1 || xx == 3 - yy));
            else if (c == 'N')
                on = xx == 0 || xx == 4 || xx == yy - 1;
            else if (c == 'O')
                on = yy == 0 || yy == 6 || xx == 0 || xx == 4;
            else if (c == 'U')
                on = xx == 0 || xx == 4 || yy == 6;
            else if (c == 'W')
                on = xx == 0 || xx == 4 || (yy >= 4 && (xx == yy - 3 ||
                            xx == 7 - yy));
            else if (c == 'X')
                on = xx == yy - 1 || xx == 5 - yy;
            else if (c == 'Y')
                on = (yy < 3 && (xx == yy || xx == 4 - yy)) ||
                    (yy >= 3 && xx == 2);
            else if (c == ',' || c == '-')
                on = c == ',' ? (yy >= 5 && (xx == 2 ||
                            (yy == 6 && xx == 1))) : yy == 3;
            else
                on = yy == 0 || yy == 6 || xx == 0 || xx == 4;

            if (on) {
                for (unsigned int sy = 0; sy < scale; sy++)
                    for (unsigned int sx = 0; sx < scale; sx++)
                        put_pixel_alpha(frame,
                                x + xx * (int)scale + (int)sx,
                                y + yy * (int)scale + (int)sy,
                                color, alpha);
            }
        }
    }
}

static void draw_glyph(uint8_t *frame, int x, int y, char c, uint16_t color)
{
    draw_glyph_scaled(frame, x, y, c, color, 2, 255);
}

static void draw_text(uint8_t *frame, int x, int y, const char *s,
        uint16_t color)
{
    while (*s) {
        draw_glyph(frame, x, y, *s, color);
        x += 12;
        s++;
    }
}

static unsigned long sink_rss_kib(void)
{
    FILE *stream;
    unsigned long total_pages;
    unsigned long resident_pages;
    long page_size;

    stream = fopen("/proc/self/statm", "r");
    if (!stream)
        return 0;
    if (fscanf(stream, "%lu %lu", &total_pages, &resident_pages) != 2) {
        fclose(stream);
        return 0;
    }
    fclose(stream);
    (void)total_pages;
    page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0)
        return 0;
    return resident_pages * (unsigned long)page_size / 1024u;
}

static int cpu_usage_read_counters(const char *path, uint64_t *total,
        uint64_t *idle_total)
{
    FILE *stream;
    char line[512];
    char name[8];
    unsigned long long user = 0;
    unsigned long long nice = 0;
    unsigned long long system = 0;
    unsigned long long idle = 0;
    unsigned long long iowait = 0;
    unsigned long long irq = 0;
    unsigned long long softirq = 0;
    unsigned long long steal = 0;
    int fields;

    if (!path || !*path || !total || !idle_total)
        return 0;
    stream = fopen(path, "r");
    if (!stream)
        return 0;
    if (!fgets(line, sizeof(line), stream)) {
        fclose(stream);
        return 0;
    }
    fclose(stream);
    fields = sscanf(line, "%7s %llu %llu %llu %llu %llu %llu %llu %llu",
            name, &user, &nice, &system, &idle, &iowait, &irq, &softirq,
            &steal);
    if (fields != 9 || strcmp(name, "cpu") != 0)
        return 0;

    /* /proc/stat appends guest and guest_nice after steal. They are already
     * included in user/nice and must not be counted a second time. */
    *total = (uint64_t)user + (uint64_t)nice + (uint64_t)system +
        (uint64_t)idle + (uint64_t)iowait + (uint64_t)irq +
        (uint64_t)softirq + (uint64_t)steal;
    *idle_total = (uint64_t)idle + (uint64_t)iowait;
    return 1;
}

static void cpu_usage_log_transition(struct cpu_usage_state *state,
        int next_state, double percent)
{
    if (state->log_state == next_state)
        return;
    state->log_state = next_state;
    if (next_state == 1)
        fprintf(stderr, "debug_overlay_cpu state=baseline source=/proc/stat\n");
    else if (next_state == 2)
        fprintf(stderr, "debug_overlay_cpu state=ready usage=%.1f\n",
                percent);
    else
        fprintf(stderr, "debug_overlay_cpu state=unavailable source=/proc/stat\n");
}

static int cpu_usage_sample(struct cpu_usage_state *state, const char *path)
{
    uint64_t total;
    uint64_t idle_total;
    uint64_t total_delta;
    uint64_t idle_delta;
    double percent;

    if (!state || !cpu_usage_read_counters(path, &total, &idle_total)) {
        if (state) {
            state->baseline_valid = 0;
            state->percent_valid = 0;
            cpu_usage_log_transition(state, 3, 0.0);
        }
        return 0;
    }
    state->sample_count++;
    if (!state->baseline_valid || total <= state->previous_total ||
            idle_total < state->previous_idle) {
        state->previous_total = total;
        state->previous_idle = idle_total;
        state->baseline_valid = 1;
        state->percent_valid = 0;
        cpu_usage_log_transition(state, 1, 0.0);
        return 0;
    }

    total_delta = total - state->previous_total;
    idle_delta = idle_total - state->previous_idle;
    state->previous_total = total;
    state->previous_idle = idle_total;
    if (idle_delta > total_delta)
        idle_delta = total_delta;
    percent = (double)(total_delta - idle_delta) * 100.0 /
        (double)total_delta;
    if (percent < 0.0)
        percent = 0.0;
    else if (percent > 100.0)
        percent = 100.0;
    state->percent = percent;
    state->percent_valid = 1;
    cpu_usage_log_transition(state, 2, percent);
    return 1;
}

static unsigned int debug_overlay_max_chars(unsigned int items)
{
    unsigned int width = 0;

    if (items & DEBUG_OVERLAY_FPS)
        width = 15;
    if ((items & DEBUG_OVERLAY_DIRTY) && width < 14)
        width = 14;
    if ((items & DEBUG_OVERLAY_BYTES) && width < 21)
        width = 21;
    if ((items & DEBUG_OVERLAY_BBOX) && width < 19)
        width = 19;
    if ((items & DEBUG_OVERLAY_MEMORY) && width < 16)
        width = 16;
    if ((items & DEBUG_OVERLAY_CPU) && width < 10)
        width = 10;
    return width;
}

static void debug_overlay_configure(struct debug_overlay_state *overlay,
        int enabled, unsigned int alpha, unsigned int scale,
        unsigned int items, unsigned int interval_ms)
{
    unsigned int max_chars;
    unsigned int lines = 0;
    unsigned int width;
    unsigned int height;

    memset(overlay, 0, sizeof(*overlay));
    overlay->enabled = enabled != 0;
    overlay->alpha = alpha;
    overlay->scale = scale;
    overlay->interval_ms = interval_ms;
    overlay->items = items;
    overlay->cpu_stat_path = "/proc/stat";
    for (unsigned int bit = 0; bit < DEBUG_OVERLAY_MAX_LINES; bit++)
        if (overlay->items & (1u << bit))
            lines++;
    overlay->line_count = lines;
    max_chars = debug_overlay_max_chars(overlay->items);
    width = (max_chars * 6u + 4u) * overlay->scale;
    height = (lines * 9u + 4u) * overlay->scale;
    if (!width || !height) {
        overlay->bounds.x0 = 0;
        overlay->bounds.y0 = 0;
        overlay->bounds.x1 = 0;
        overlay->bounds.y1 = 0;
        return;
    }
    if (width > LCD_WIDTH)
        width = LCD_WIDTH;
    if (height > LCD_HEIGHT)
        height = LCD_HEIGHT;
    overlay->bounds.x0 = 0;
    overlay->bounds.y0 = 0;
    overlay->bounds.x1 = width - 1;
    overlay->bounds.y1 = height - 1;
}

static void debug_overlay_init(struct debug_overlay_state *overlay)
{
    debug_overlay_configure(overlay,
            env_u32_range("CH347_DEBUG_OVERLAY", 0, 0, 1) != 0,
            env_u32_range("CH347_DEBUG_OVERLAY_ALPHA", 176, 0, 255),
            env_u32_range("CH347_DEBUG_OVERLAY_SCALE", 1, 1, 2),
            debug_overlay_items_parse(getenv("CH347_DEBUG_OVERLAY_ITEMS")),
            env_u32_range("CH347_DEBUG_OVERLAY_INTERVAL_MS", 1000, 250,
                5000));
}

struct sink_runtime_config {
    int debug;
    int cursor_enabled;
    enum frame_rotation rotation;
    int overlay_enabled;
    unsigned int overlay_alpha;
    unsigned int overlay_scale;
    unsigned int overlay_items;
    unsigned int overlay_interval_ms;
};

static int parse_uint_text(const char *text, unsigned int minimum,
        unsigned int maximum, unsigned int *value)
{
    char *end = NULL;
    unsigned long parsed;

    if (!text || !*text || !value)
        return 0;
    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno || !end || (*end && *end != '\n' && *end != '\r') ||
            parsed < minimum || parsed > maximum)
        return 0;
    *value = (unsigned int)parsed;
    return 1;
}

static int read_uint_assignment(const char *path, const char *key,
        unsigned int minimum, unsigned int maximum, unsigned int *value)
{
    FILE *stream;
    char line[192];
    size_t key_length;
    int found = 0;
    unsigned int selected = 0;

    if (!path || !*path || !key || !*key || !value)
        return 0;
    stream = fopen(path, "r");
    if (!stream)
        return 0;
    key_length = strlen(key);
    while (fgets(line, sizeof(line), stream)) {
        char *cursor = line;

        while (*cursor == ' ' || *cursor == '\t')
            cursor++;
        if (!*cursor || *cursor == '\n' || *cursor == '#')
            continue;
        if (strncmp(cursor, key, key_length) != 0 ||
                cursor[key_length] != '=')
            continue;
        if (found || !parse_uint_text(cursor + key_length + 1u, minimum,
                    maximum, &selected)) {
            fclose(stream);
            return 0;
        }
        found = 1;
    }
    if (ferror(stream)) {
        fclose(stream);
        return 0;
    }
    fclose(stream);
    if (!found)
        return 0;
    *value = selected;
    return 1;
}

static int read_rotation_assignment(const char *path,
        enum frame_rotation *rotation)
{
    FILE *stream;
    char line[192];
    char value[64];
    int found = 0;
    enum frame_rotation selected = FRAME_ROTATION_NORMAL;

    if (!path || !*path || !rotation)
        return 0;
    stream = fopen(path, "r");
    if (!stream)
        return 0;
    while (fgets(line, sizeof(line), stream)) {
        char extra;
        char *cursor = line;

        while (*cursor == ' ' || *cursor == '\t')
            cursor++;
        if (!*cursor || *cursor == '\n' || *cursor == '#')
            continue;
        if (sscanf(cursor, "CH347_DISPLAY_ROTATION=%63[^\r\n]%c", value,
                    &extra) < 1)
            continue;
        if (found || !frame_rotation_parse(value, &selected)) {
            fclose(stream);
            return 0;
        }
        found = 1;
    }
    if (ferror(stream)) {
        fclose(stream);
        return 0;
    }
    fclose(stream);
    if (!found)
        return 0;
    *rotation = selected;
    return 1;
}

static int sink_runtime_config_load(struct sink_runtime_config *config,
        const char *fps_path, const char *overlay_path,
        const char *cursor_path, const char *rotation_path)
{
    struct sink_runtime_config selected;
    unsigned int value;

    if (!config)
        return 0;
    selected = *config;
    if (fps_path && *fps_path) {
        if (!read_uint_assignment(fps_path, "DEBUG", 0, 1, &value))
            return 0;
        selected.debug = value != 0;
    }
    if (overlay_path && *overlay_path) {
        if (!read_uint_assignment(overlay_path, "CH347_DEBUG_OVERLAY", 0, 1,
                    &value))
            return 0;
        selected.overlay_enabled = value != 0;
        if (!read_uint_assignment(overlay_path,
                    "CH347_DEBUG_OVERLAY_ALPHA", 0, 255,
                    &selected.overlay_alpha) ||
                !read_uint_assignment(overlay_path,
                    "CH347_DEBUG_OVERLAY_SCALE", 1, 2,
                    &selected.overlay_scale) ||
                !read_uint_assignment(overlay_path,
                    "CH347_DEBUG_OVERLAY_ITEMS", 1, 63,
                    &selected.overlay_items) ||
                !read_uint_assignment(overlay_path,
                    "CH347_DEBUG_OVERLAY_INTERVAL_MS", 250, 5000,
                    &selected.overlay_interval_ms))
            return 0;
    }
    if (cursor_path && *cursor_path) {
        if (!read_uint_assignment(cursor_path, "CH347_CURSOR", 0, 1,
                    &value))
            return 0;
        selected.cursor_enabled = value != 0;
    }
    if (rotation_path && *rotation_path &&
            !read_rotation_assignment(rotation_path, &selected.rotation))
        return 0;
    *config = selected;
    return 1;
}

static int debug_overlay_sample(struct debug_overlay_state *overlay,
        const struct debug_overlay_metrics *metrics, double now)
{
    unsigned int line = 0;

    if (!overlay->enabled || !overlay->items || !overlay->alpha)
        return 0;
    if (overlay->last_sample > 0.0 &&
            now - overlay->last_sample <
            (double)overlay->interval_ms / 1000.0)
        return 0;
    overlay->last_sample = now;
    if (overlay->items & DEBUG_OVERLAY_CPU)
        (void)cpu_usage_sample(&overlay->cpu, overlay->cpu_stat_path);
    if (overlay->items & DEBUG_OVERLAY_FPS)
        snprintf(overlay->lines[line++], DEBUG_OVERLAY_LINE_CAPACITY,
                "C:%04.1f P:%04.1f", metrics->capture_fps,
                metrics->panel_fps);
    if (overlay->items & DEBUG_OVERLAY_DIRTY)
        snprintf(overlay->lines[line++], DEBUG_OVERLAY_LINE_CAPACITY,
                "D:%03.0f%% R:%03u", metrics->dirty_pct, metrics->rects);
    if (overlay->items & DEBUG_OVERLAY_BYTES)
        snprintf(overlay->lines[line++], DEBUG_OVERLAY_LINE_CAPACITY,
                "B:%zu T:%lluK", metrics->last_sent_pixels * 2u,
                metrics->sent_pixels * 2u / 1024u);
    if (overlay->items & DEBUG_OVERLAY_BBOX) {
        if (metrics->bbox_valid) {
            snprintf(overlay->lines[line++], DEBUG_OVERLAY_LINE_CAPACITY,
                    "Q:%u,%u-%u,%u", metrics->bbox.x0, metrics->bbox.y0,
                    metrics->bbox.x1, metrics->bbox.y1);
        } else {
            snprintf(overlay->lines[line++], DEBUG_OVERLAY_LINE_CAPACITY,
                    "Q:NONE");
        }
    }
    if (overlay->items & DEBUG_OVERLAY_MEMORY)
        snprintf(overlay->lines[line++], DEBUG_OVERLAY_LINE_CAPACITY,
                "SINK RSS:%luK", sink_rss_kib());
    if (overlay->items & DEBUG_OVERLAY_CPU) {
        if (overlay->cpu.percent_valid)
            snprintf(overlay->lines[line++], DEBUG_OVERLAY_LINE_CAPACITY,
                    "CPU:%.1f%%", overlay->cpu.percent);
        else
            snprintf(overlay->lines[line++], DEBUG_OVERLAY_LINE_CAPACITY,
                    "CPU:--");
    }
    overlay->line_count = line;
    return 1;
}

static int debug_overlay_due(const struct debug_overlay_state *overlay,
        double now)
{
    if (!overlay->enabled || !overlay->items || !overlay->alpha)
        return 0;
    return overlay->last_sample <= 0.0 ||
        now - overlay->last_sample >=
        (double)overlay->interval_ms / 1000.0;
}

static unsigned int idle_wake_interval_ms(int touch_enabled,
        unsigned int touch_ms, int gpio_enabled, unsigned int gpio_ms,
        const struct debug_overlay_state *overlay, double now)
{
    unsigned int wake_ms = 0;

    if (touch_enabled)
        wake_ms = touch_ms ? touch_ms : 1;
    if (gpio_enabled && (!wake_ms || gpio_ms < wake_ms))
        wake_ms = gpio_ms ? gpio_ms : 1;
    if (overlay->enabled && overlay->items && overlay->alpha) {
        unsigned int overlay_ms = 1;

        if (overlay->last_sample > 0.0) {
            double elapsed_ms = (now - overlay->last_sample) * 1000.0;

            if (elapsed_ms < (double)overlay->interval_ms) {
                double remaining_ms = overlay->interval_ms - elapsed_ms;

                overlay_ms = (unsigned int)remaining_ms;
                if ((double)overlay_ms < remaining_ms)
                    overlay_ms++;
                if (!overlay_ms)
                    overlay_ms = 1;
            }
        }
        if (!wake_ms || overlay_ms < wake_ms)
            wake_ms = overlay_ms;
    }
    return wake_ms;
}

static void draw_debug_overlay(uint8_t *frame,
        const struct debug_overlay_state *overlay)
{
    uint16_t bg = rgb565(0, 0, 0);
    uint16_t fg = rgb565(255, 255, 255);
    unsigned int scale = overlay->scale;

    if (!overlay->enabled || !overlay->line_count || !overlay->alpha)
        return;
    for (unsigned int y = overlay->bounds.y0; y <= overlay->bounds.y1; y++)
        for (unsigned int x = overlay->bounds.x0; x <= overlay->bounds.x1; x++)
            put_pixel_alpha(frame, (int)x, (int)y, bg, overlay->alpha);

    for (unsigned int line = 0; line < overlay->line_count; line++) {
        const char *cursor = overlay->lines[line];
        int x = (int)(2u * scale);
        int y = (int)((2u + line * 9u) * scale);

        while (*cursor) {
            draw_glyph_scaled(frame, x, y, *cursor++, fg, scale,
                    overlay->alpha);
            x += (int)(6u * scale);
        }
    }
}

static void draw_gpio_overlay(uint8_t *frame, const struct gpio_overlay_state *gs)
{
    char text[64];
    uint16_t bg = rgb565(0, 0, 0);
    uint16_t fg = rgb565(255, 255, 255);
    uint16_t hi = rgb565(80, 255, 120);
    uint16_t lo = rgb565(255, 80, 80);
    int y0 = LCD_HEIGHT - 122;

    for (int y = y0; y < LCD_HEIGHT; y++)
        for (int x = 0; x < 245; x++)
            put_pixel(frame, x, y, bg);

    if (!gs->valid) {
        draw_text(frame, 4, y0 + 6, "GPIO READ FAIL", lo);
        return;
    }

    snprintf(text, sizeof(text), "GPIO:%02X", gs->pins);
    draw_text(frame, 4, y0 + 4, text, fg);

    for (unsigned int row = 0; row < 4; row++) {
        unsigned int a = row * 2;
        unsigned int b = a + 1;
        int y = y0 + 24 + (int)row * 22;

        snprintf(text, sizeof(text), "G%u:%u %02X", a,
                (gs->pins & GPIO_MASK(a)) ? 1 : 0, gs->raw[a]);
        draw_text(frame, 4, y, text,
                (gs->pins & GPIO_MASK(a)) ? hi : lo);

        snprintf(text, sizeof(text), "G%u:%u %02X", b,
                (gs->pins & GPIO_MASK(b)) ? 1 : 0, gs->raw[b]);
        draw_text(frame, 118, y, text,
                (gs->pins & GPIO_MASK(b)) ? hi : lo);
    }
}

static void draw_hline(uint8_t *frame, int x0, int x1, int y, uint16_t color)
{
    if (y < 0 || y >= LCD_HEIGHT)
        return;
    if (x0 > x1) {
        int tmp = x0;

        x0 = x1;
        x1 = tmp;
    }
    if (x0 < 0)
        x0 = 0;
    if (x1 >= LCD_WIDTH)
        x1 = LCD_WIDTH - 1;
    for (int x = x0; x <= x1; x++)
        put_pixel(frame, x, y, color);
}

static void draw_vline(uint8_t *frame, int x, int y0, int y1, uint16_t color)
{
    if (x < 0 || x >= LCD_WIDTH)
        return;
    if (y0 > y1) {
        int tmp = y0;

        y0 = y1;
        y1 = tmp;
    }
    if (y0 < 0)
        y0 = 0;
    if (y1 >= LCD_HEIGHT)
        y1 = LCD_HEIGHT - 1;
    for (int y = y0; y <= y1; y++)
        put_pixel(frame, x, y, color);
}

static void draw_cross(uint8_t *frame, int x, int y, int radius,
        uint16_t color)
{
    draw_hline(frame, x - radius, x + radius, y, color);
    draw_vline(frame, x, y - radius, y + radius, color);
    draw_hline(frame, x - 3, x + 3, y - 3, color);
    draw_hline(frame, x - 3, x + 3, y + 3, color);
    draw_vline(frame, x - 3, y - 3, y + 3, color);
    draw_vline(frame, x + 3, y - 3, y + 3, color);
}

static void draw_cursor(uint8_t *frame, int x, int y)
{
    uint16_t white = rgb565(255, 255, 255);
    uint16_t black = rgb565(0, 0, 0);
    uint16_t accent = rgb565(255, 230, 64);

    draw_hline(frame, x - 9, x - 3, y, black);
    draw_hline(frame, x + 3, x + 9, y, black);
    draw_vline(frame, x, y - 9, y - 3, black);
    draw_vline(frame, x, y + 3, y + 9, black);
    draw_hline(frame, x - 8, x - 4, y, white);
    draw_hline(frame, x + 4, x + 8, y, white);
    draw_vline(frame, x, y - 8, y - 4, white);
    draw_vline(frame, x, y + 4, y + 8, white);
    put_pixel(frame, x, y, accent);
}

static void draw_calibration(uint8_t *frame, const struct touch_state *ts)
{
    uint16_t bg = rgb565(0, 0, 0);
    uint16_t white = rgb565(255, 255, 255);
    uint16_t dim = rgb565(80, 80, 80);
    uint16_t accent = rgb565(255, 230, 64);
    uint16_t good = rgb565(80, 255, 120);
    uint16_t warn = rgb565(255, 80, 80);
    char text[64];

    for (int y = 0; y < LCD_HEIGHT; y++)
        for (int x = 0; x < LCD_WIDTH; x++)
            put_pixel(frame, x, y, bg);

    draw_text(frame, 12, 10, "TOUCH CAL", white);
    if (touch_calibrating_pressure(ts)) {
        if ((ts->cal_press_index & 1) == 0)
            draw_text(frame, 12, 30, "LIGHT PRESS", white);
        else
            draw_text(frame, 12, 30, "HARD PRESS", white);
    } else {
        draw_text(frame, 12, 30, "PRESS POINT", white);
    }
    if (ts->raw_valid) {
        snprintf(text, sizeof(text), "RAW %04u %04u", ts->last_raw_x,
                ts->last_raw_y);
        draw_text(frame, 12, 54, text, white);
        snprintf(text, sizeof(text), "Z%04u %04u", ts->last_raw_z1,
                ts->last_raw_z2);
        draw_text(frame, 12, 76, text, white);
        snprintf(text, sizeof(text), "P%04u MIN%04u", ts->last_pressure,
                ts->pressure_min);
        draw_text(frame, 12, 98, text,
                ts->last_pressure >= ts->pressure_min ? good : warn);
        snprintf(text, sizeof(text), "XY%03d %03d", ts->last_screen_x,
                ts->last_screen_y);
        draw_text(frame, 12, 120, text, white);
        draw_cursor(frame, ts->last_screen_x, ts->last_screen_y);
    } else {
        snprintf(text, sizeof(text), "MIN%04u", ts->pressure_min);
        draw_text(frame, 12, 54, text, white);
    }
    if (!ts->cal_done && ts->cal_sample_count) {
        snprintf(text, sizeof(text), "HOLD %u/%u", ts->cal_sample_count,
                ts->cal_samples_required);
        draw_text(frame, 12, 142, text, accent);
    }

    for (unsigned int i = 0; i < CAL_POS_POINTS; i++)
        draw_cross(frame, ts->cal_sx[i], ts->cal_sy[i],
                i == ts->cal_index && !touch_calibrating_pressure(ts) ? 16 : 8,
                i == ts->cal_index && !touch_calibrating_pressure(ts) ?
                accent : dim);

    if (touch_calibrating_pressure(ts))
        draw_cross(frame, LCD_WIDTH / 2, LCD_HEIGHT / 2, 20, accent);

    if (ts->cal_done) {
        draw_text(frame, 18, LCD_HEIGHT - 46, "DONE", accent);
    } else if (touch_calibrating_pressure(ts)) {
        snprintf(text, sizeof(text), "P %u/%u", ts->cal_press_index + 1,
                CAL_PRESS_POINTS);
        draw_text(frame, 18, LCD_HEIGHT - 46, text, white);
    } else {
        snprintf(text, sizeof(text), "%u/%u", ts->cal_index + 1,
                CAL_POS_POINTS);
        draw_text(frame, 18, LCD_HEIGHT - 46, text, white);
    }
}

static int tile_changed(const uint8_t *frame, const uint8_t *prev,
        unsigned int tx, unsigned int ty)
{
    unsigned int x0 = tx * TILE_W;
    unsigned int y0 = ty * TILE_H;
    unsigned int x1 = x0 + TILE_W;
    unsigned int y1 = y0 + TILE_H;

    if (x1 > LCD_WIDTH)
        x1 = LCD_WIDTH;
    if (y1 > LCD_HEIGHT)
        y1 = LCD_HEIGHT;

    for (unsigned int y = y0; y < y1; y++) {
        size_t off = ((size_t)y * LCD_WIDTH + x0) * 2;
        size_t len = (x1 - x0) * 2;

        if (memcmp(frame + off, prev + off, len))
            return 1;
    }

    return 0;
}

static size_t tile_pixels(unsigned int tx, unsigned int ty)
{
    unsigned int x0 = tx * TILE_W;
    unsigned int y0 = ty * TILE_H;
    unsigned int x1 = x0 + TILE_W;
    unsigned int y1 = y0 + TILE_H;

    if (x1 > LCD_WIDTH)
        x1 = LCD_WIDTH;
    if (y1 > LCD_HEIGHT)
        y1 = LCD_HEIGHT;

    return (size_t)(x1 - x0) * (y1 - y0);
}

static void rect_from_tiles(struct rect *r, unsigned int tx0, unsigned int ty0,
        unsigned int tx1, unsigned int ty1)
{
    r->x0 = tx0 * TILE_W;
    r->y0 = ty0 * TILE_H;
    r->x1 = (tx1 + 1) * TILE_W - 1;
    r->y1 = (ty1 + 1) * TILE_H - 1;

    if (r->x1 >= LCD_WIDTH)
        r->x1 = LCD_WIDTH - 1;
    if (r->y1 >= LCD_HEIGHT)
        r->y1 = LCD_HEIGHT - 1;
}

static unsigned int build_tile_rects(uint8_t dirty[TILES_Y][TILES_X],
        struct rect *rects)
{
    uint8_t used[TILES_Y][TILES_X];
    unsigned int count = 0;

    memset(used, 0, sizeof(used));

    for (unsigned int ty = 0; ty < TILES_Y; ty++) {
        for (unsigned int tx = 0; tx < TILES_X; tx++) {
            unsigned int tx1;
            unsigned int ty1;

            if (!dirty[ty][tx] || used[ty][tx])
                continue;

            tx1 = tx;
            while (tx1 + 1 < TILES_X && dirty[ty][tx1 + 1] &&
                    !used[ty][tx1 + 1])
                tx1++;

            ty1 = ty;
            while (ty1 + 1 < TILES_Y) {
                int ok = 1;

                for (unsigned int x = tx; x <= tx1; x++) {
                    if (!dirty[ty1 + 1][x] || used[ty1 + 1][x]) {
                        ok = 0;
                        break;
                    }
                }
                if (!ok)
                    break;
                ty1++;
            }

            for (unsigned int y = ty; y <= ty1; y++)
                for (unsigned int x = tx; x <= tx1; x++)
                    used[y][x] = 1;

            rect_from_tiles(&rects[count++], tx, ty, tx1, ty1);
        }
    }

    return count;
}

static void mark_refreshed(double tile_last[TILES_Y][TILES_X],
        const struct rect *r, double now)
{
    unsigned int tx0 = r->x0 / TILE_W;
    unsigned int tx1 = r->x1 / TILE_W;
    unsigned int ty0 = r->y0 / TILE_H;
    unsigned int ty1 = r->y1 / TILE_H;

    if (tx1 >= TILES_X)
        tx1 = TILES_X - 1;
    if (ty1 >= TILES_Y)
        ty1 = TILES_Y - 1;

    for (unsigned int ty = ty0; ty <= ty1; ty++)
        for (unsigned int tx = tx0; tx <= tx1; tx++)
            tile_last[ty][tx] = now;
}

static size_t rect_pixels(const struct rect *r)
{
    return (size_t)(r->x1 - r->x0 + 1) * (r->y1 - r->y0 + 1);
}

static size_t rect_list_pixels(const struct rect *rects, unsigned int count)
{
    size_t pixels = 0;

    for (unsigned int i = 0; i < count; i++)
        pixels += rect_pixels(&rects[i]);
    return pixels;
}

static int rect_list_has_full_refresh(const struct rect *rects,
        unsigned int count)
{
    for (unsigned int i = 0; i < count; i++) {
        if (rects[i].x0 == 0 && rects[i].y0 == 0 &&
                rects[i].x1 == LCD_WIDTH - 1 &&
                rects[i].y1 == LCD_HEIGHT - 1)
            return 1;
    }
    return 0;
}

static int row_diff_bounds(const uint8_t *a, const uint8_t *b,
        unsigned int *x0, unsigned int *x1)
{
    const uint64_t *wa = (const uint64_t *)a;
    const uint64_t *wb = (const uint64_t *)b;
    unsigned int words = STRIDE_BYTES / sizeof(uint64_t);
    unsigned int left_word = words;
    unsigned int right_word = 0;
    unsigned int left_byte;
    unsigned int right_byte;

    for (unsigned int i = 0; i < words; i++) {
        if (wa[i] != wb[i]) {
            left_word = i;
            break;
        }
    }
    if (left_word == words)
        return 0;

    for (unsigned int i = words; i > left_word; i--) {
        if (wa[i - 1] != wb[i - 1]) {
            right_word = i - 1;
            break;
        }
    }

    left_byte = left_word * sizeof(uint64_t);
    while (left_byte < STRIDE_BYTES && a[left_byte] == b[left_byte])
        left_byte++;

    right_byte = (right_word + 1) * sizeof(uint64_t) - 1;
    while (right_byte > left_byte && a[right_byte] == b[right_byte])
        right_byte--;

    *x0 = left_byte / 2;
    *x1 = right_byte / 2;
    return 1;
}

static int fast_dirty_bbox(const uint8_t *frame, const uint8_t *prev,
        struct rect *r)
{
    unsigned int min_x = LCD_WIDTH;
    unsigned int min_y = LCD_HEIGHT;
    unsigned int max_x = 0;
    unsigned int max_y = 0;

    for (unsigned int y = 0; y < LCD_HEIGHT; y++) {
        size_t off = (size_t)y * STRIDE_BYTES;
        unsigned int x0;
        unsigned int x1;

        if (!row_diff_bounds(frame + off, prev + off, &x0, &x1))
            continue;

        if (x0 < min_x)
            min_x = x0;
        if (x1 > max_x)
            max_x = x1;
        if (y < min_y)
            min_y = y;
        max_y = y;
    }

    if (min_x == LCD_WIDTH)
        return 0;

    r->x0 = min_x;
    r->y0 = min_y;
    r->x1 = max_x;
    r->y1 = max_y;
    return 1;
}

static void rect_union_into(struct rect *dst, const struct rect *src)
{
    if (src->x0 < dst->x0)
        dst->x0 = src->x0;
    if (src->y0 < dst->y0)
        dst->y0 = src->y0;
    if (src->x1 > dst->x1)
        dst->x1 = src->x1;
    if (src->y1 > dst->y1)
        dst->y1 = src->y1;
}

static int rect_list_bbox(const struct rect *rects, unsigned int count,
        struct rect *bbox)
{
    if (!count || !bbox)
        return 0;
    *bbox = rects[0];
    for (unsigned int i = 1; i < count; i++)
        rect_union_into(bbox, &rects[i]);
    return 1;
}

static int collapse_overlay_damage_to_single_bbox(struct rect *rects,
        unsigned int *count, unsigned int max_rects, int overlay_updated)
{
    struct rect bbox;

    if (!overlay_updated || max_rects != 1 || *count <= 1)
        return 0;
    if (!rect_list_bbox(rects, *count, &bbox))
        return 0;
    rects[0] = bbox;
    *count = 1;
    return 1;
}

static int select_overlay_idle_damage(struct rect *rects,
        unsigned int *count, size_t *dirty_area,
        const struct debug_overlay_state *overlay, int overlay_wakeup,
        int overlay_updated, int new_frame, int touch_changed,
        int gpio_changed)
{
    if (!overlay_wakeup || !overlay_updated || new_frame || touch_changed ||
            gpio_changed)
        return 0;
    *count = 1;
    rects[0] = overlay->bounds;
    *dirty_area = rect_pixels(&overlay->bounds);
    return 1;
}

static void append_send_rect(struct rect *rects, unsigned int *count,
        const struct rect *r)
{
    if (r->x1 < r->x0 || r->y1 < r->y0)
        return;

    if (*count < MAX_TILE_RECTS) {
        rects[*count] = *r;
        (*count)++;
        return;
    }

    rect_union_into(&rects[0], r);
    *count = 1;
}

static int rects_intersect(const struct rect *a, const struct rect *b)
{
    return a->x0 <= b->x1 && a->x1 >= b->x0 &&
        a->y0 <= b->y1 && a->y1 >= b->y0;
}

static void mark_dirty_tiles_for_rect(uint8_t dirty[TILES_Y][TILES_X],
        const uint8_t *frame, const uint8_t *prev, const struct rect *r,
        unsigned int *dirty_tiles, size_t *dirty_area)
{
    unsigned int tx0 = r->x0 / TILE_W;
    unsigned int tx1 = r->x1 / TILE_W;
    unsigned int ty0 = r->y0 / TILE_H;
    unsigned int ty1 = r->y1 / TILE_H;

    if (tx1 >= TILES_X)
        tx1 = TILES_X - 1;
    if (ty1 >= TILES_Y)
        ty1 = TILES_Y - 1;

    for (unsigned int ty = ty0; ty <= ty1; ty++) {
        for (unsigned int tx = tx0; tx <= tx1; tx++) {
            if (dirty[ty][tx])
                continue;
            if (!tile_changed(frame, prev, tx, ty))
                continue;
            dirty[ty][tx] = 1;
            (*dirty_tiles)++;
            *dirty_area += tile_pixels(tx, ty);
        }
    }
}

static void cursor_rect(int x, int y, struct rect *r)
{
    int x0 = x - 10;
    int y0 = y - 10;
    int x1 = x + 10;
    int y1 = y + 10;

    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 >= LCD_WIDTH)
        x1 = LCD_WIDTH - 1;
    if (y1 >= LCD_HEIGHT)
        y1 = LCD_HEIGHT - 1;

    r->x0 = (unsigned int)x0;
    r->y0 = (unsigned int)y0;
    r->x1 = (unsigned int)x1;
    r->y1 = (unsigned int)y1;
}

static void restore_rect_from_base(uint8_t *frame, const uint8_t *base,
        const struct rect *r)
{
    unsigned int w = r->x1 - r->x0 + 1;

    for (unsigned int y = r->y0; y <= r->y1; y++) {
        size_t off = ((size_t)y * LCD_WIDTH + r->x0) * 2;

        memcpy(frame + off, base + off, w * 2);
    }
}

static int dirty_bbox(uint8_t dirty[TILES_Y][TILES_X], struct rect *r)
{
    unsigned int tx0 = TILES_X;
    unsigned int ty0 = TILES_Y;
    unsigned int tx1 = 0;
    unsigned int ty1 = 0;

    for (unsigned int ty = 0; ty < TILES_Y; ty++) {
        for (unsigned int tx = 0; tx < TILES_X; tx++) {
            if (!dirty[ty][tx])
                continue;
            if (tx < tx0)
                tx0 = tx;
            if (ty < ty0)
                ty0 = ty;
            if (tx > tx1)
                tx1 = tx;
            if (ty > ty1)
                ty1 = ty;
        }
    }

    if (tx0 == TILES_X)
        return 0;

    rect_from_tiles(r, tx0, ty0, tx1, ty1);
    return 1;
}

static int send_rect(int fd, struct slot *slots, unsigned int depth,
        const uint8_t *frame, uint8_t *scratch, const struct rect *r,
        unsigned int packet_delay_us)
{
    unsigned int w = r->x1 - r->x0 + 1;
    unsigned int h = r->y1 - r->y0 + 1;
    size_t len = (size_t)w * h * 2;
    const uint8_t *src = scratch;
    if (w == LCD_WIDTH) {
        src = frame + (size_t)r->y0 * STRIDE_BYTES;
    } else {
        /*
         * ST7796 RAMWR is a linear GRAM stream.  A merged dirty rectangle must
         * include every row between y0 and y1; gaps are filled from the current
         * framebuffer rather than trying to "skip" rows on SPI.
         */
        for (unsigned int y = 0; y < h; y++) {
            memcpy(scratch + (size_t)y * w * 2,
                    frame + ((size_t)(r->y0 + y) * LCD_WIDTH + r->x0) * 2,
                    w * 2);
        }
    }

    if (lcd_set_window_for_data(fd, r->x0, r->y0, r->x1, r->y1) < 0)
        return -1;

    if (send_bytes(fd, slots, depth, src, len, packet_delay_us) < 0)
        return -1;

    return lcd_cs(fd, 0);
}

static int rect_protocol_loop(int input_fd, int fd, struct slot *slots,
        unsigned int depth, uint8_t *frame, uint8_t *prev, uint8_t *scratch,
        uint8_t *first_hdr, unsigned int max_frames,
        unsigned int packet_delay_us, int debug, struct touch_state *touch,
        struct gpio_overlay_state *gpio_overlay,
        struct debug_overlay_state *debug_overlay, double start,
        unsigned int max_rects, double full_area_ratio)
{
    uint8_t hdr[RECT_HDR_LEN];
    unsigned int frames = 0;
    unsigned int captured_frames = 0;
    unsigned int dropped_frames = 0;
    unsigned int last_rects = 0;
    double last_dirty_pct = 0.0;
    double pixels_sent = 0.0;
    unsigned long long sent_pixels_total = 0;
    size_t last_sent_pixels = 0;
    struct rect last_bbox = {0, 0, 0, 0};
    int last_bbox_valid = 0;
    uint8_t *base = aligned_alloc(64, FRAME_BYTES);
    int overlay_cursor_visible = 0;
    struct rect overlay_cursor_rect = {0, 0, 0, 0};

    if (!base) {
        fprintf(stderr, "rect protocol base alloc failed\n");
        return 1;
    }

    memcpy(hdr, first_hdr, RECT_HDR_LEN);
    memset(frame, 0, FRAME_BYTES);
    memset(base, 0, FRAME_BYTES);
    memset(prev, 0, FRAME_BYTES);

    for (;;) {
        uint16_t screen_w;
        uint16_t screen_h;
        uint16_t count;
        struct rect send_list[MAX_TILE_RECTS];
        struct rect input_union = {LCD_WIDTH - 1, LCD_HEIGHT - 1, 0, 0};
        uint8_t dirty[TILES_Y][TILES_X];
        unsigned int send_count = 0;
        int have_input_union = 0;
        size_t dirty_area = 0;
        unsigned int dirty_tiles = 0;
        double now;
        int overlay_updated = 0;

        if (memcmp(hdr, RECT_MAGIC, 4)) {
            fprintf(stderr, "rect protocol lost sync\n");
            break;
        }

        screen_w = get_le16(hdr + 4);
        screen_h = get_le16(hdr + 6);
        count = get_le16(hdr + 8);
        captured_frames++;
        if (screen_w != LCD_WIDTH || screen_h != LCD_HEIGHT ||
                count > MAX_TILE_RECTS) {
            fprintf(stderr, "bad rect packet size=%ux%u count=%u\n",
                    screen_w, screen_h, count);
            break;
        }

        now = now_sec();
        for (unsigned int i = 0; i < count; i++) {
            uint8_t rh[RECT_ITEM_HDR_LEN];
            uint16_t x;
            uint16_t y;
            uint16_t w;
            uint16_t h;
            uint32_t len;
            struct rect r;

            if (read_full_fd(input_fd, rh, sizeof(rh)) != 1) {
                fprintf(stderr, "short rect header\n");
                goto out;
            }

            x = get_le16(rh + 0);
            y = get_le16(rh + 2);
            w = get_le16(rh + 4);
            h = get_le16(rh + 6);
            len = get_le32(rh + 8);
            if (!w || !h || x >= LCD_WIDTH || y >= LCD_HEIGHT ||
                    x + w > LCD_WIDTH || y + h > LCD_HEIGHT ||
                    len != (uint32_t)w * h * 2 || len > FRAME_BYTES) {
                fprintf(stderr,
                        "bad rect item x=%u y=%u w=%u h=%u len=%u\n",
                        x, y, w, h, len);
                goto out;
            }
            if (read_full_fd(input_fd, scratch, len) != 1) {
                fprintf(stderr, "short rect payload\n");
                goto out;
            }

            for (unsigned int yy = 0; yy < h; yy++) {
                size_t dst = ((size_t)(y + yy) * LCD_WIDTH + x) * 2;
                size_t src = (size_t)yy * w * 2;

                memcpy(base + dst, scratch + src, w * 2);
                memcpy(frame + dst,
                        scratch + (size_t)yy * w * 2, w * 2);
            }

            r.x0 = x;
            r.y0 = y;
            r.x1 = x + w - 1;
            r.y1 = y + h - 1;
            if (!have_input_union) {
                input_union = r;
                have_input_union = 1;
            } else {
                rect_union_into(&input_union, &r);
            }
        }

        memset(dirty, 0, sizeof(dirty));
        if (frames == 0) {
            struct rect full = {0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1};

            append_send_rect(send_list, &send_count, &full);
            dirty_area = FRAME_PIXELS;
        } else if (have_input_union) {
            struct rect rects[MAX_TILE_RECTS];
            unsigned int rect_count;

            mark_dirty_tiles_for_rect(dirty, frame, prev, &input_union,
                    &dirty_tiles, &dirty_area);
            rect_count = build_tile_rects(dirty, rects);
            if (!rect_count) {
                send_count = 0;
            } else if (dirty_area >=
                    (size_t)(FRAME_PIXELS * full_area_ratio)) {
                struct rect full = {0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1};

                append_send_rect(send_list, &send_count, &full);
            } else if (rect_count > max_rects) {
                struct rect bbox;

                if (dirty_bbox(dirty, &bbox))
                    append_send_rect(send_list, &send_count, &bbox);
            } else {
                for (unsigned int i = 0; i < rect_count; i++)
                    append_send_rect(send_list, &send_count, &rects[i]);
            }
        }

        if (debug_overlay->enabled) {
            double elapsed = now - start + 0.000001;
            struct debug_overlay_metrics metrics;
            int updated;

            restore_rect_from_base(frame, base, &debug_overlay->bounds);
            memset(&metrics, 0, sizeof(metrics));
            metrics.capture_fps = frames / elapsed;
            metrics.panel_fps = pixels_sent / (FRAME_PIXELS * elapsed);
            metrics.rects = last_rects;
            metrics.dirty_pct = last_dirty_pct;
            metrics.last_sent_pixels = last_sent_pixels;
            metrics.sent_pixels = sent_pixels_total;
            metrics.bbox = last_bbox;
            metrics.bbox_valid = last_bbox_valid;
            updated = debug_overlay_sample(debug_overlay, &metrics, now);
            overlay_updated = updated;
            draw_debug_overlay(frame, debug_overlay);
            if (updated) {
                append_send_rect(send_list, &send_count,
                        &debug_overlay->bounds);
                dirty_area += rect_pixels(&debug_overlay->bounds);
            }
        }

        if (gpio_overlay->enabled &&
                now - gpio_overlay->last_poll >=
                (double)gpio_overlay->poll_ms / 1000.0) {
            struct rect gr = {0, LCD_HEIGHT - 122, 244, LCD_HEIGHT - 1};

            gpio_overlay->valid = ch347_read_gpio_full(fd, &gpio_overlay->pins,
                    gpio_overlay->raw) == 0;
            gpio_overlay->last_poll = now;
            draw_gpio_overlay(frame, gpio_overlay);
            append_send_rect(send_list, &send_count, &gr);
            dirty_area += rect_pixels(&gr);
        }

        if (touch->enabled) {
            int old_cursor_visible = overlay_cursor_visible;
            struct rect old_cursor = overlay_cursor_rect;

            (void)touch_poll(fd, touch, now);
            if (touch->calibrate) {
                struct rect full = {0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1};

                draw_calibration(frame, touch);
                append_send_rect(send_list, &send_count, &full);
                dirty_area = FRAME_PIXELS;
            } else if (touch->cursor_enabled && touch->cursor_visible) {
                struct rect nr;

                cursor_rect(touch->cursor_x, touch->cursor_y, &nr);
                if (old_cursor_visible && !rects_intersect(&old_cursor, &nr)) {
                    restore_rect_from_base(frame, base, &old_cursor);
                    append_send_rect(send_list, &send_count, &old_cursor);
                    dirty_area += rect_pixels(&old_cursor);
                }
                if (old_cursor_visible && rects_intersect(&old_cursor, &nr)) {
                    rect_union_into(&nr, &old_cursor);
                    restore_rect_from_base(frame, base, &nr);
                }
                draw_cursor(frame, touch->cursor_x, touch->cursor_y);
                append_send_rect(send_list, &send_count, &nr);
                dirty_area += rect_pixels(&nr);
                overlay_cursor_visible = 1;
                overlay_cursor_rect = nr;
            } else if (old_cursor_visible) {
                restore_rect_from_base(frame, base, &old_cursor);
                append_send_rect(send_list, &send_count, &old_cursor);
                dirty_area += rect_pixels(&old_cursor);
                overlay_cursor_visible = 0;
            }
        }

        if (overlay_cursor_visible && have_input_union &&
                rects_intersect(&input_union, &overlay_cursor_rect) &&
                touch->enabled && !touch->calibrate &&
                touch->cursor_enabled && touch->cursor_visible) {
            draw_cursor(frame, touch->cursor_x, touch->cursor_y);
            append_send_rect(send_list, &send_count, &overlay_cursor_rect);
        }

        if (collapse_overlay_damage_to_single_bbox(send_list, &send_count,
                    max_rects, overlay_updated))
            dirty_area = rect_pixels(&send_list[0]);

        for (unsigned int i = 0; i < send_count; i++) {
            if (send_rect(fd, slots, depth, frame, scratch, &send_list[i],
                        packet_delay_us) < 0) {
                fprintf(stderr,
                        "LCD rect write failed rect_proto frame=%u rect=%u [%u,%u]-[%u,%u]\n",
                        frames, i, send_list[i].x0, send_list[i].y0,
                        send_list[i].x1, send_list[i].y1);
                goto out;
            }
            pixels_sent += rect_pixels(&send_list[i]);
        }

        last_sent_pixels = rect_list_pixels(send_list, send_count);
        sent_pixels_total += last_sent_pixels;
        last_bbox_valid = rect_list_bbox(send_list, send_count, &last_bbox);

        last_dirty_pct = (double)dirty_area * 100.0 / FRAME_PIXELS;
        last_rects = send_count;
        memcpy(prev, frame, FRAME_BYTES);
        frames++;

        if (debug && (frames == 1 || (frames % 10) == 0)) {
            fprintf(stderr,
                    "rect frame=%u captured=%u drop=%u sent_rects=%u dirty=%.1f%% bus_fps=%.2f out_fps=%.2f\n",
                    frames, captured_frames, dropped_frames, send_count,
                    last_dirty_pct,
                    pixels_sent / (FRAME_PIXELS *
                        (now_sec() - start + 0.000001)),
                    frames / (now_sec() - start + 0.000001));
        }

        if (touch->cal_done && touch->cal_exit &&
                now_sec() - touch->cal_done_time > 1.0)
            break;
        if (max_frames && frames >= max_frames)
            break;

        if (read_full_fd(input_fd, hdr, sizeof(hdr)) != 1)
            break;
    }

out:
    fprintf(stderr,
            "rect_usb_sink frames=%u captured=%u dropped=%u total=%.3fs out_fps=%.2f bus_fps=%.2f\n",
            frames, captured_frames, dropped_frames, now_sec() - start,
            frames / (now_sec() - start + 0.000001),
            pixels_sent / (FRAME_PIXELS * (now_sec() - start + 0.000001)));
    free(base);
    return frames ? 0 : 1;
}

int main(int argc, char **argv)
{
    unsigned int depth = DEFAULT_DEPTH;
    unsigned int max_frames = 0;
    const char *usb_dev = USB_DEV;
    const char *input_path = NULL;
    const char *mailbox_path = getenv("CH347_FRAME_MAILBOX");
    unsigned int iface = USB_IFACE;
    unsigned int packet_delay_us = env_u32("CH347_PACKET_US", 0);
    unsigned int stale_ms = env_u32("CH347_STALE_MS", 1000);
    unsigned int stale_budget = env_u32("CH347_STALE_BUDGET", 60);
    unsigned int max_rects = env_u32("CH347_MAX_RECTS", 1);
    double full_area_ratio = env_double("CH347_FULL_AREA_PCT", 40.0) / 100.0;
    int swap16 = env_u32("CH347_SWAP16", 0) != 0;
    int debug = env_u32("CH347_DEBUG", 0) != 0;
    const char *fps_file = getenv("CH347_FPS_FILE");
    const char *overlay_file = getenv("CH347_DEBUG_OVERLAY_FILE");
    const char *cursor_file = getenv("CH347_CURSOR_FILE");
    const char *rotation_file = getenv("CH347_ROTATION_FILE");
    int repeat_input = env_u32("CH347_REPEAT_INPUT", 0) != 0;
    int latest_only = env_u32("CH347_LATEST_ONLY", 1) != 0;
    struct touch_state touch;
    struct gpio_overlay_state gpio_overlay;
    struct debug_overlay_state debug_overlay;
    struct sink_runtime_config runtime_config;
    struct slot *slots;
    struct rect rects[MAX_TILE_RECTS];
    struct rect send_list[MAX_TILE_RECTS];
    uint8_t dirty[TILES_Y][TILES_X];
    double tile_last[TILES_Y][TILES_X];
    struct reader_state reader;
    pthread_t reader_tid;
    uint8_t *frame;
    uint8_t *prev;
    uint8_t *scratch;
    uint8_t *latest;
    uint8_t *reader_tmp;
    uint8_t *base_frame;
    uint8_t first_hdr[RECT_HDR_LEN];
    uint64_t consumed_seq = 0;
    unsigned int captured_frames = 0;
    unsigned int dropped_frames = 0;
    unsigned int frames = 0;
    unsigned int last_rects = 0;
    double last_dirty_pct = 0.0;
    double pixels_sent = 0.0;
    double start;
    double rate_time;
    double rate_fps = 0.0;
    double rate_bus_fps = 0.0;
    double rate_capture_ema = 0.0;
    double rate_bus_ema = 0.0;
    unsigned int rate_captured_base = 0;
    unsigned int rate_sent_frames = 0;
    unsigned long long sent_pixels_total = 0;
    unsigned int sent_frames_total = 0;
    unsigned int zero_damage_frames = 0;
    unsigned int full_refreshes = 0;
    unsigned int large_refreshes = 0;
    size_t last_sent_pixels = 0;
    struct rect last_bbox = {0, 0, 0, 0};
    int last_bbox_valid = 0;
    int input_fd = -1;
    int mailbox_fd = -1;
    void *mailbox_mapping = MAP_FAILED;
    size_t mailbox_mapping_size = 0;
    int mailbox_mode = mailbox_path && *mailbox_path;
    int prefetched_frame = 0;
    int fd;
    int rect_protocol = 0;
    int reader_started = 0;
    int exit_status = 1;

    if (argc == 3 && strcmp(argv[1], "--usb-reset") == 0)
        return reset_usb_device(argv[2]);

    if (argc > 1)
        depth = (unsigned int)strtoul(argv[1], NULL, 0);
    if (argc > 2)
        max_frames = (unsigned int)strtoul(argv[2], NULL, 0);
    if (argc > 3)
        usb_dev = argv[3];
    if (argc > 4)
        input_path = argv[4];
    if (getenv("CH347_USB_DEV"))
        usb_dev = getenv("CH347_USB_DEV");
    hold_cs = env_u32("CH347_HOLD_CS", 1) != 0;
    usb_debug = env_u32("CH347_USB_DEBUG", 0);

    memset(&touch, 0, sizeof(touch));
    touch.uhid.fd = -1;
    memset(&gpio_overlay, 0, sizeof(gpio_overlay));
    gpio_overlay.enabled = env_u32("CH347_GPIO_OVERLAY", 0) != 0;
    gpio_overlay.poll_ms = env_u32("CH347_GPIO_OVERLAY_MS", 200);
    if (!gpio_overlay.poll_ms)
        gpio_overlay.poll_ms = 1;
    debug_overlay_init(&debug_overlay);
    signal(SIGUSR1, runtime_reload_signal);

    touch.enabled = env_u32("CH347_TOUCH", 0) != 0;
    touch.use_irq = env_u32("CH347_TOUCH_USE_IRQ", 1) != 0;
    touch.cursor_enabled = env_u32("CH347_CURSOR", 0) != 0;
    touch.input_mode = touch_mode_parse(getenv("CH347_TOUCH_MODE"),
            TOUCH_MODE_TOUCH);
    touch.calibrate = env_u32("CH347_TOUCH_CALIBRATE", 0) != 0;
    touch.cal_exit = env_u32("CH347_TOUCH_CAL_EXIT", 1) != 0;
    touch.swap_xy = env_u32("CH347_TOUCH_SWAP_XY", 0) != 0;
    touch.invert_x = env_u32("CH347_TOUCH_INVERT_X", 0) != 0;
    touch.invert_y = env_u32("CH347_TOUCH_INVERT_Y", 0) != 0;
    touch.debug = env_u32("CH347_TOUCH_DEBUG", 0) != 0;
    touch.disable_on_errors = env_u32("CH347_TOUCH_DISABLE_ON_ERRORS", 0) != 0;
    touch.poll_ms = env_u32("CH347_TOUCH_POLL_MS", 25);
    touch.width = env_u32("CH347_TOUCH_WIDTH", LCD_WIDTH);
    touch.height = env_u32("CH347_TOUCH_HEIGHT", LCD_HEIGHT);
    touch.x_min = env_u32("CH347_TOUCH_X_MIN", 200);
    touch.x_max = env_u32("CH347_TOUCH_X_MAX", 3900);
    touch.y_min = env_u32("CH347_TOUCH_Y_MIN", 200);
    touch.y_max = env_u32("CH347_TOUCH_Y_MAX", 3900);
    touch.move_thresh = env_u32("CH347_TOUCH_MOVE_THRESH", 2);
    touch.jump_thresh = env_u32("CH347_TOUCH_JUMP_THRESH", 96);
    touch.filter_weight = env_u32("CH347_TOUCH_FILTER_WEIGHT", 1);
    touch.max_errors = env_u32("CH347_TOUCH_MAX_ERRORS", 8);
    touch.z_min = env_u32("CH347_TOUCH_Z_MIN", 50);
    touch.pressure_min = env_u32("CH347_TOUCH_PRESSURE_MIN", touch.z_min);
    touch.z_strict = env_u32("CH347_TOUCH_Z_STRICT", 1) != 0;
    if (!frame_rotation_parse(getenv("CH347_DISPLAY_ROTATION"),
                &touch.rotation)) {
        fprintf(stderr, "invalid CH347_DISPLAY_ROTATION\n");
        return 1;
    }
    touch.touch_clock = env_u32("CH347_TOUCH_CLOCK", 5);
    touch.lcd_clock = env_u32("CH347_CLOCK", 1);
    touch.release_samples = env_u32("CH347_TOUCH_RELEASE_SAMPLES", 3);
    touch.cal_samples_required = env_u32("CH347_TOUCH_CAL_SAMPLES", 4);
    touch.cal_margin = env_u32("CH347_TOUCH_CAL_MARGIN", 32);
    touch.display_name = getenv("DISPLAY_ID");
    if (!touch.display_name || !*touch.display_name)
        touch.display_name = getenv("DISPLAY");
    if (!touch.display_name || !*touch.display_name)
        touch.display_name = ":24";
    touch.cal_file = getenv("CH347_TOUCH_CAL_FILE");
    touch.mode_file = getenv("CH347_TOUCH_MODE_FILE");
    if (!touch.cal_file || !*touch.cal_file)
        touch.cal_file = "/root/x11display/ch347/touch_calibration.env";
    if (touch.calibrate)
        touch.enabled = 1;
    if (touch.calibrate)
        touch.cal_wait_release = 1;
    if (!touch.poll_ms)
        touch.poll_ms = 1;
    if (touch.width < 2)
        touch.width = LCD_WIDTH;
    if (touch.height < 2)
        touch.height = LCD_HEIGHT;
    frame_rotation_output_size(touch.rotation, touch.width, touch.height,
            &touch.logical_width, &touch.logical_height);
    memset(&runtime_config, 0, sizeof(runtime_config));
    runtime_config.debug = debug;
    runtime_config.cursor_enabled = touch.cursor_enabled;
    runtime_config.rotation = touch.rotation;
    runtime_config.overlay_enabled = debug_overlay.enabled;
    runtime_config.overlay_alpha = debug_overlay.alpha;
    runtime_config.overlay_scale = debug_overlay.scale;
    runtime_config.overlay_items = debug_overlay.items;
    runtime_config.overlay_interval_ms = debug_overlay.interval_ms;
    if (!touch.max_errors)
        touch.max_errors = 1;
    if (!touch.jump_thresh)
        touch.jump_thresh = touch.width + touch.height;
    if (touch.filter_weight > 7)
        touch.filter_weight = 7;
    if (touch.touch_clock > 7)
        touch.touch_clock = 5;
    if (touch.lcd_clock > 7)
        touch.lcd_clock = 1;
    if (!touch.release_samples)
        touch.release_samples = 1;
    if (!touch.cal_samples_required)
        touch.cal_samples_required = 1;
    touch.cursor_x = (int)touch.width / 2;
    touch.cursor_y = (int)touch.height / 2;
    touch_config_cal_points(&touch);

    if (depth < 1)
        depth = 1;
    if (depth > 64)
        depth = 64;
    if (max_rects < 1)
        max_rects = 1;
    if (max_rects > MAX_TILE_RECTS)
        max_rects = MAX_TILE_RECTS;
    if (full_area_ratio <= 0.0 || full_area_ratio > 1.0)
        full_area_ratio = 0.40;
    if (stale_budget > MAX_TILE_RECTS)
        stale_budget = MAX_TILE_RECTS;

    (void)mlockall(MCL_CURRENT | MCL_FUTURE);

    frame = aligned_alloc(64, FRAME_BYTES);
    prev = aligned_alloc(64, FRAME_BYTES);
    scratch = aligned_alloc(64, FRAME_BYTES);
    latest = aligned_alloc(64, FRAME_BYTES);
    reader_tmp = aligned_alloc(64, FRAME_BYTES);
    base_frame = aligned_alloc(64, FRAME_BYTES);
    slots = calloc(depth, sizeof(*slots));
    if (!frame || !prev || !scratch || !latest || !reader_tmp || !base_frame || !slots) {
        fprintf(stderr, "alloc failed\n");
        return 1;
    }

    memset(prev, 0, FRAME_BYTES);
    memset(tile_last, 0, sizeof(tile_last));

    if (mailbox_mode) {
        struct stat st;
        double deadline = now_sec() + 5.0;

        do {
            mailbox_fd = open(mailbox_path, O_RDWR | O_CLOEXEC);
            if (mailbox_fd >= 0)
                break;
            usleep(10000);
        } while (now_sec() < deadline);
        if (mailbox_fd < 0 || fstat(mailbox_fd, &st) < 0) {
            fprintf(stderr, "open mailbox %s failed: %s\n", mailbox_path,
                    strerror(errno));
            return 1;
        }
        mailbox_mapping_size = (size_t)st.st_size;
        mailbox_mapping = mmap(NULL, mailbox_mapping_size, PROT_READ | PROT_WRITE,
                MAP_SHARED, mailbox_fd, 0);
        if (mailbox_mapping == MAP_FAILED) {
            fprintf(stderr, "mmap mailbox %s failed: %s\n", mailbox_path,
                    strerror(errno));
            return 1;
        }
    } else if (input_path) {
        input_fd = open(input_path, O_RDONLY);
        if (input_fd < 0) {
            fprintf(stderr, "open input %s failed: %s\n", input_path,
                    strerror(errno));
            return 1;
        }
    } else {
        input_fd = STDIN_FILENO;
    }

    fd = open(usb_dev, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "open %s failed: %s\n", usb_dev, strerror(errno));
        return 1;
    }

    if (ioctl(fd, USBDEVFS_CLAIMINTERFACE, &iface) < 0) {
        fprintf(stderr, "CLAIMINTERFACE failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    (void)lcd_cs(fd, 0);
    if (hold_cs)
        (void)lcd_cs_raw(fd, 1);

    if (touch.enabled && !touch.calibrate) {
        if (touch.input_mode == TOUCH_MODE_TOUCH) {
            if (touch_uhid_init(&touch) < 0) {
                fprintf(stderr,
                        "touch disabled: cannot create UHID touchscreen: %s\n",
                        strerror(errno));
                touch.enabled = 0;
            }
        } else if (x11_touch_init(&touch) < 0) {
            fprintf(stderr,
                    "touch disabled: cannot open XTest display %s via libX11/libXtst\n",
                    touch.display_name);
            touch.enabled = 0;
        }
    }

    start = now_sec();
    rate_time = start;
    if (max_rects == 1 && !stale_ms) {
        fprintf(stderr,
                "dirty_usb_sink depth=%u max_frames=%u tile=%ux%u stale_ms=%u stale_budget=%u max_rects=%u full_pct=inactive(single-bbox) packet_us=%u hold_cs=%d latest_only=%d touch=%d touch_mode=%s touch_irq=%d cursor=%d calibrate=%d rotation=%s logical=%ux%u gpio_overlay=%d debug_log=%d debug_overlay=%d overlay_alpha=%u overlay_scale=%u overlay_items=0x%x overlay_ms=%u\n",
                depth, max_frames, TILE_W, TILE_H, stale_ms, stale_budget,
                max_rects, packet_delay_us, hold_cs, latest_only, touch.enabled,
                touch_mode_name(touch.input_mode), touch.use_irq,
                touch.cursor_enabled, touch.calibrate,
                frame_rotation_name(touch.rotation), touch.logical_width,
                touch.logical_height, gpio_overlay.enabled, debug,
                debug_overlay.enabled, debug_overlay.alpha,
                debug_overlay.scale, debug_overlay.items,
                debug_overlay.interval_ms);
    } else {
        fprintf(stderr,
                "dirty_usb_sink depth=%u max_frames=%u tile=%ux%u stale_ms=%u stale_budget=%u max_rects=%u full_pct=%.1f packet_us=%u hold_cs=%d latest_only=%d touch=%d touch_mode=%s touch_irq=%d cursor=%d calibrate=%d rotation=%s logical=%ux%u gpio_overlay=%d debug_log=%d debug_overlay=%d overlay_alpha=%u overlay_scale=%u overlay_items=0x%x overlay_ms=%u\n",
                depth, max_frames, TILE_W, TILE_H, stale_ms, stale_budget,
                max_rects, full_area_ratio * 100.0, packet_delay_us, hold_cs,
                latest_only, touch.enabled, touch_mode_name(touch.input_mode),
                touch.use_irq, touch.cursor_enabled, touch.calibrate,
                frame_rotation_name(touch.rotation), touch.logical_width,
                touch.logical_height, gpio_overlay.enabled, debug,
                debug_overlay.enabled, debug_overlay.alpha,
                debug_overlay.scale, debug_overlay.items,
                debug_overlay.interval_ms);
    }

    if (mailbox_mode) {
        double deadline = now_sec() + 5.0;
        int got = 0;

        while (now_sec() < deadline) {
            got = mailbox_copy_latest(mailbox_mapping, mailbox_mapping_size,
                    frame, &consumed_seq, &captured_frames, &dropped_frames);
            if (got != 0)
                break;
            usleep(1000);
        }
        if (got < 0) {
            fprintf(stderr, "invalid frame mailbox %s\n", mailbox_path);
            goto out;
        }
        if (!got) {
            fprintf(stderr, "frame mailbox timed out waiting for first frame\n");
            goto out;
        }
        /* mailbox_copy_latest() advanced consumed_seq while waiting for the
         * producer.  Preserve that already-copied frame as pending work so it
         * follows the normal new-frame path and reaches the panel immediately. */
        prefetched_frame = 1;
        memset(prev, 0, FRAME_BYTES);
        latest_only = 0;
        goto frame_loop;
    }

    if (read_full_fd(input_fd, first_hdr, sizeof(first_hdr)) != 1) {
        fprintf(stderr, "input ended before first packet\n");
        goto out;
    }
    rect_protocol = !memcmp(first_hdr, RECT_MAGIC, 4);
    if (rect_protocol) {
        fprintf(stderr, "dirty_usb_sink input=rect-protocol\n");
        exit_status = rect_protocol_loop(input_fd, fd, slots, depth, frame, prev,
                scratch, first_hdr, max_frames, packet_delay_us, debug, &touch,
                &gpio_overlay, &debug_overlay, start, max_rects,
                full_area_ratio);
        goto out;
    }

    memset(&reader, 0, sizeof(reader));
    reader.fd = input_fd;
    reader.repeat_input = repeat_input;
    reader.input_path = input_path;
    reader.latest = latest;
    reader.tmp = reader_tmp;
    pthread_mutex_init(&reader.lock, NULL);
    pthread_cond_init(&reader.cond, NULL);

    memcpy(reader_tmp, first_hdr, sizeof(first_hdr));
    if (read_full_fd(input_fd, reader_tmp + sizeof(first_hdr),
                FRAME_BYTES - sizeof(first_hdr)) != 1) {
        fprintf(stderr, "short first frame\n");
        goto out;
    }

    if (latest_only) {
        memcpy(reader.latest, reader_tmp, FRAME_BYTES);
        reader.seq = 1;
        reader.frames = 1;
        if (pthread_create(&reader_tid, NULL, reader_thread, &reader) != 0) {
            fprintf(stderr, "reader thread create failed\n");
            goto out;
        }
        reader_started = 1;
    } else {
        memcpy(frame, reader_tmp, FRAME_BYTES);
    }

frame_loop:
    for (;;) {
        double now;
                double fps;
        double bus_fps;
        size_t dirty_area = 0;
        unsigned int dirty_tiles = 0;
        unsigned int stale_marked = 0;
        unsigned int rect_count = 0;
        unsigned int send_count = 0;
        size_t frame_sent_pixels = 0;
        int frame_full_refresh = 0;
        int poll_wakeup = 0;
        int new_frame = 0;
        int touch_changed = 0;
        int gpio_changed = 0;
        int overlay_wakeup = 0;
        int overlay_updated = 0;
        int control_changed = 0;
        int ret;

        if (runtime_reload_requested) {
            struct sink_runtime_config selected = runtime_config;

            runtime_reload_requested = 0;
            if (!sink_runtime_config_load(&selected, fps_file, overlay_file,
                        cursor_file, rotation_file)) {
                fprintf(stderr,
                        "dirty_usb_sink control_reload rejected invalid config\n");
            } else {
                int rotation_changed = selected.rotation != touch.rotation;

                if (rotation_changed) {
                    if (touch.down)
                        touch_release(&touch);
                    touch.cursor_visible = 0;
                    touch.rotation = selected.rotation;
                    frame_rotation_output_size(touch.rotation, touch.width,
                            touch.height, &touch.logical_width,
                            &touch.logical_height);
                    touch.filter_valid = 0;
                    touch.raw_valid = 0;
                }
                debug = selected.debug;
                touch.cursor_enabled = selected.cursor_enabled;
                debug_overlay_configure(&debug_overlay,
                        selected.overlay_enabled, selected.overlay_alpha,
                        selected.overlay_scale, selected.overlay_items,
                        selected.overlay_interval_ms);
                runtime_config = selected;
                control_changed = 1;
                fprintf(stderr,
                        "dirty_usb_sink control_reload debug_log=%d debug_overlay=%d overlay_alpha=%u overlay_scale=%u overlay_items=0x%x overlay_ms=%u cursor=%d rotation=%s logical=%ux%u\n",
                        debug, debug_overlay.enabled, debug_overlay.alpha,
                        debug_overlay.scale, debug_overlay.items,
                        debug_overlay.interval_ms, touch.cursor_enabled,
                        frame_rotation_name(touch.rotation),
                        touch.logical_width, touch.logical_height);
            }
        }
        if (mailbox_mode) {
            int got = mailbox_copy_next(mailbox_mapping, mailbox_mapping_size,
                    frame, &consumed_seq, &captured_frames, &dropped_frames,
                    prefetched_frame);
            {
                struct frame_mailbox_header *header = mailbox_mapping;
                uint64_t heartbeat = atomic_load_explicit(
                        &header->producer_heartbeat_ms, memory_order_acquire);
                uint64_t age_ms = heartbeat ? now_ms() - heartbeat : 0;
                int producer_alive = atomic_load_explicit(
                        &header->producer_alive, memory_order_acquire) != 0;

                if (!producer_alive || (heartbeat && age_ms > 750)) {
                    if (touch.down)
                        touch_release(&touch);
                    touch.cursor_visible = 0;
                    {
                        struct timespec pause = {0, 10000000L};
                        nanosleep(&pause, NULL);
                    }
                    continue;
                }
            }

            if (got < 0) {
                fprintf(stderr, "frame mailbox became invalid\n");
                break;
            }
            prefetched_frame = 0;
            new_frame = got > 0;
            now = now_sec();
            if (touch.enabled)
                touch_changed = touch_poll(fd, &touch, now);
            if (gpio_overlay.enabled &&
                    now - gpio_overlay.last_poll >=
                    (double)gpio_overlay.poll_ms / 1000.0) {
                gpio_overlay.valid = ch347_read_gpio_full(fd,
                        &gpio_overlay.pins, gpio_overlay.raw) == 0;
                gpio_overlay.last_poll = now;
                gpio_changed = 1;
            }
            overlay_wakeup = debug_overlay_due(&debug_overlay, now);
            if (!new_frame && !touch_changed && !gpio_changed &&
                    !control_changed &&
                    !overlay_wakeup) {
                struct timespec pause = {0, 1000000L};

                nanosleep(&pause, NULL);
                continue;
            }
            if (!new_frame)
                memcpy(frame, base_frame, FRAME_BYTES);

        } else if (latest_only) {
            uint64_t seq;

            pthread_mutex_lock(&reader.lock);
            while (reader.seq == consumed_seq && !reader.eof && !reader.error) {
                unsigned int wake_ms = idle_wake_interval_ms(touch.enabled,
                        touch.poll_ms, gpio_overlay.enabled,
                        gpio_overlay.poll_ms, &debug_overlay, now_sec());

                if (wake_ms) {
                    struct timespec deadline;

                    realtime_after_ms(&deadline, wake_ms);
                    ret = pthread_cond_timedwait(&reader.cond, &reader.lock,
                            &deadline);
                    if (ret == ETIMEDOUT) {
                        poll_wakeup = 1;
                        break;
                    }
                } else {
                    pthread_cond_wait(&reader.cond, &reader.lock);
                }
            }

            if (reader.error) {
                pthread_mutex_unlock(&reader.lock);
                fprintf(stderr, "reader failed after captured=%u displayed=%u\n",
                        reader.frames, frames);
                break;
            }

            if (reader.eof && reader.seq == consumed_seq) {
                pthread_mutex_unlock(&reader.lock);
                exit_status = 0;
                break;
            }

            seq = reader.seq;
            if (seq != consumed_seq) {
                memcpy(frame, reader.latest, FRAME_BYTES);
                captured_frames = reader.frames;
                if (seq > consumed_seq + 1)
                    dropped_frames += (unsigned int)(seq - consumed_seq - 1);
                consumed_seq = seq;
                reader.acked_seq = seq;
                pthread_cond_broadcast(&reader.cond);
                new_frame = 1;
            }
            pthread_mutex_unlock(&reader.lock);

            now = now_sec();
            if (touch.enabled)
                touch_changed = touch_poll(fd, &touch, now);
            if (gpio_overlay.enabled &&
                    now - gpio_overlay.last_poll >=
                    (double)gpio_overlay.poll_ms / 1000.0) {
                gpio_overlay.valid = ch347_read_gpio_full(fd,
                        &gpio_overlay.pins, gpio_overlay.raw) == 0;
                gpio_overlay.last_poll = now;
                gpio_changed = 1;
            }

            overlay_wakeup = debug_overlay_due(&debug_overlay, now);

            if (poll_wakeup && !new_frame && !touch_changed && !gpio_changed &&
                    !overlay_wakeup)
                continue;

            if (poll_wakeup && !new_frame) {
                pthread_mutex_lock(&reader.lock);
                memcpy(frame, reader.latest, FRAME_BYTES);
                pthread_mutex_unlock(&reader.lock);
            }
        } else {
            if (frames > 0) {
                ret = read_full_fd(input_fd, frame, FRAME_BYTES);
                if (ret == 0) {
                    if (repeat_input && input_path &&
                            lseek(input_fd, 0, SEEK_SET) >= 0)
                        continue;
                    exit_status = 0;
                    break;
                }
                if (ret < 0) {
                    fprintf(stderr, "short or failed frame read after %u frames\n",
                            frames);
                    break;
                }
            }
            captured_frames = frames + 1;
        }

        /* Mailbox cursor-only passes restore the display-order base frame.
         * Re-swapping it would make the entire panel dirty.  Other inputs
         * still arrive in capture byte order on every pass. */
        if (swap16 && (!mailbox_mode || new_frame))
            swap16_frame(frame);
        if (new_frame)
            memcpy(base_frame, frame, FRAME_BYTES);

        now = now_sec();
        if (!mailbox_mode && !latest_only && touch.enabled)
            touch_changed = touch_poll(fd, &touch, now);
        if (!mailbox_mode && !latest_only && gpio_overlay.enabled &&
                now - gpio_overlay.last_poll >=
                (double)gpio_overlay.poll_ms / 1000.0) {
            gpio_overlay.valid = ch347_read_gpio_full(fd, &gpio_overlay.pins,
                    gpio_overlay.raw) == 0;
            gpio_overlay.last_poll = now;
        }

        fps = rate_fps;
        bus_fps = rate_bus_fps;
        if (touch.calibrate) {
            draw_calibration(frame, &touch);
        } else if (debug_overlay.enabled) {
            struct debug_overlay_metrics metrics;

            memset(&metrics, 0, sizeof(metrics));
            metrics.capture_fps = fps;
            metrics.panel_fps = bus_fps;
            metrics.rects = last_rects;
            metrics.dirty_pct = last_dirty_pct;
            metrics.last_sent_pixels = last_sent_pixels;
            metrics.sent_pixels = sent_pixels_total;
            metrics.bbox = last_bbox;
            metrics.bbox_valid = last_bbox_valid;
            overlay_updated = debug_overlay_sample(&debug_overlay, &metrics,
                    now);
            draw_debug_overlay(frame, &debug_overlay);
        }

        if (touch.enabled && !touch.calibrate && touch.cursor_enabled &&
                touch.cursor_visible)
            draw_cursor(frame, touch.cursor_x, touch.cursor_y);

        if (gpio_overlay.enabled)
            draw_gpio_overlay(frame, &gpio_overlay);

        if (select_overlay_idle_damage(send_list, &send_count, &dirty_area,
                    &debug_overlay, overlay_wakeup, overlay_updated, new_frame,
                    touch_changed, gpio_changed)) {
            /* The helper deliberately selects the exact overlay bounds. */
        } else if (max_rects == 1 && !stale_ms) {
            struct rect bbox;

            if (frames == 0) {
                send_count = 1;
                send_list[0].x0 = 0;
                send_list[0].y0 = 0;
                send_list[0].x1 = LCD_WIDTH - 1;
                send_list[0].y1 = LCD_HEIGHT - 1;
                dirty_area = FRAME_PIXELS;
            } else if (fast_dirty_bbox(frame, prev, &bbox)) {
                send_count = 1;
                send_list[0] = bbox;
                dirty_area = rect_pixels(&bbox);
            }
        } else {
            memset(dirty, 0, sizeof(dirty));
            for (unsigned int ty = 0; ty < TILES_Y; ty++) {
                for (unsigned int tx = 0; tx < TILES_X; tx++) {
                    int changed = frames == 0 ||
                        tile_changed(frame, prev, tx, ty);

                    if (!changed && stale_ms &&
                            now - tile_last[ty][tx] >=
                            (double)stale_ms / 1000.0 &&
                            stale_marked < stale_budget) {
                        changed = 1;
                        stale_marked++;
                    }

                    if (changed) {
                        dirty[ty][tx] = 1;
                        dirty_tiles++;
                        dirty_area += tile_pixels(tx, ty);
                    }
                }
            }
        }

        if (dirty_tiles) {
            rect_count = build_tile_rects(dirty, rects);

            if (frames == 0 ||
                    dirty_area >= (size_t)(FRAME_PIXELS * full_area_ratio)) {
                send_count = 1;
                send_list[0].x0 = 0;
                send_list[0].y0 = 0;
                send_list[0].x1 = LCD_WIDTH - 1;
                send_list[0].y1 = LCD_HEIGHT - 1;
            } else if (rect_count > max_rects) {
                struct rect bbox;

                if (!dirty_bbox(dirty, &bbox)) {
                    send_count = 0;
                } else if (rect_pixels(&bbox) <
                        (size_t)(FRAME_PIXELS * full_area_ratio)) {
                    send_count = 1;
                    send_list[0] = bbox;
                } else {
                    send_count = 1;
                    send_list[0].x0 = 0;
                    send_list[0].y0 = 0;
                    send_list[0].x1 = LCD_WIDTH - 1;
                    send_list[0].y1 = LCD_HEIGHT - 1;
                }
            } else {
                send_count = rect_count;
                memcpy(send_list, rects, sizeof(rects[0]) * rect_count);
            }
        }

        /* Count the actual GRAM payload selected for this pass.  This is
         * deliberately separate from dirty_area: the latter is a damage
         * estimate, while merged/overlapping rectangles determine the bytes
         * physically written to the panel. */
        frame_sent_pixels = rect_list_pixels(send_list, send_count);
        frame_full_refresh = rect_list_has_full_refresh(send_list, send_count);

        for (unsigned int i = 0; i < send_count; i++) {
            if (send_rect(fd, slots, depth, frame, scratch, &send_list[i],
                        packet_delay_us) < 0) {
                fprintf(stderr,
                        "LCD rect write failed frame=%u rect=%u [%u,%u]-[%u,%u]\n",
                        frames, i, send_list[i].x0, send_list[i].y0,
                        send_list[i].x1, send_list[i].y1);
                goto out;
            }
            mark_refreshed(tile_last, &send_list[i], now);
            pixels_sent += rect_pixels(&send_list[i]);
        }

        sent_pixels_total += (unsigned long long)frame_sent_pixels;
        last_sent_pixels = frame_sent_pixels;
        last_bbox_valid = rect_list_bbox(send_list, send_count, &last_bbox);
        if (send_count)
            sent_frames_total++;
        else
            zero_damage_frames++;
        if (frame_full_refresh)
            full_refreshes++;
        if (frame_sent_pixels >= (size_t)(FRAME_PIXELS * full_area_ratio))
            large_refreshes++;

        {
            uint8_t *swap = prev;
            prev = frame;
            frame = swap;
        }
        if (mailbox_mode) {
            struct frame_mailbox_header *header = mailbox_mapping;

            atomic_store_explicit(&header->consumed_seq, consumed_seq,
                    memory_order_release);
        }
        last_rects = send_count;
        last_dirty_pct = (double)dirty_area * 100.0 / FRAME_PIXELS;
        frames++;
        if (send_count)
            rate_sent_frames++;
        {
            double rate_now = now_sec();
            double rate_elapsed = rate_now - rate_time;

            if (rate_elapsed >= 0.50) {
                {
                    double capture_sample =
                        (captured_frames - rate_captured_base) / rate_elapsed;
                    double bus_sample = rate_sent_frames / rate_elapsed;

                    if (rate_capture_ema == 0.0) {
                        rate_capture_ema = capture_sample;
                        rate_bus_ema = bus_sample;
                    } else {
                        rate_capture_ema += 0.45 *
                            (capture_sample - rate_capture_ema);
                        rate_bus_ema += 0.45 * (bus_sample - rate_bus_ema);
                    }
                    rate_fps = rate_capture_ema;
                    rate_bus_fps = rate_bus_ema;
                }
                rate_captured_base = captured_frames;
                rate_time = rate_now;
                rate_sent_frames = 0;
            }
        }

        if (debug && (frames == 1 || (frames % 10) == 0)) {
            fprintf(stderr,
                    "dirty frame=%u captured=%u drop=%u sent_rects=%u dirty=%.1f%% bus_fps=%.2f out_fps=%.2f\n",
                    frames, captured_frames, dropped_frames, send_count,
                    last_dirty_pct,
                    rate_bus_fps, rate_fps);
        }
        if (frames == 1 || (frames % 30) == 0) {
            fprintf(stderr,
                    "dirty_stats frame=%u sent_frames=%u zero_damage=%u full_refreshes=%u large_refreshes=%u sent_pixels=%llu last_sent_pixels=%zu last_rects=%u\n",
                    frames, sent_frames_total, zero_damage_frames,
                    full_refreshes, large_refreshes, sent_pixels_total,
                    frame_sent_pixels, send_count);
        }

        if (touch.cal_done && touch.cal_exit &&
                now_sec() - touch.cal_done_time > 1.0) {
            exit_status = 0;
            break;
        }

        if (max_frames && frames >= max_frames) {
            exit_status = 0;
            break;
        }
    }

out:
    if (touch.down) {
        if (touch.input_mode == TOUCH_MODE_TOUCH)
            touch_uhid_report(&touch, touch.last_x, touch.last_y, 0);
        else
            x11_touch_button(&touch, 0);
    }
    touch_uhid_close(&touch);
    x11_touch_close(&touch);
    if (reader_started) {
        pthread_mutex_lock(&reader.lock);
        reader.eof = 1;
        reader.stop = 1;
        pthread_cond_signal(&reader.cond);
        pthread_mutex_unlock(&reader.lock);
    }
    (void)lcd_cs(fd, 0);
    ioctl(fd, USBDEVFS_RELEASEINTERFACE, &iface);
    close(fd);
    if (mailbox_mapping != MAP_FAILED)
        munmap(mailbox_mapping, mailbox_mapping_size);
    if (mailbox_fd >= 0)
        close(mailbox_fd);
    if (input_path && input_fd >= 0)
        close(input_fd);

    if (!rect_protocol) {
        fprintf(stderr, "dirty_usb_sink frames=%u captured=%u dropped=%u total=%.3fs out_fps=%.2f bus_fps=%.2f sent_frames=%u zero_damage=%u full_refreshes=%u large_refreshes=%u sent_pixels=%llu\n",
                frames, captured_frames, dropped_frames, now_sec() - start,
                frames / (now_sec() - start + 0.000001),
                pixels_sent / (FRAME_PIXELS * (now_sec() - start + 0.000001)),
                sent_frames_total, zero_damage_frames, full_refreshes,
                large_refreshes, sent_pixels_total);
    }

    free(slots);
    free(scratch);
    free(prev);
    free(frame);
    free(reader_tmp);
    free(base_frame);
    free(latest);
    return exit_status;
}
