// lagtrace: a Vulkan layer that measures click-to-photon latency in software.
//
// For every frame the game presents, the layer copies a small probe region of the
// finished image (64x64 pixels by default) into memory the CPU can read. It logs:
//   C/R  key and button presses/releases with their kernel timestamps (evdev, CLOCK_MONOTONIC)
//   F    when the game handed each frame to vkQueuePresentKHR
//   P    the probe region's mean luma once the GPU finished that frame, and when it finished
//   S    when the frame's first pixel went out to the display (VK_EXT_present_timing)
// The `lagtrace` CLI turns that into latencies: the first frame after a click whose probe
// region changed is the frame that answered the click, and the first frame at or after it
// that reached the screen is when you could have seen it. No light sensor needed.
//
// It only logs frames around presses (a few before, up to LAGTRACE_WINDOW_MS after), so a
// game can run with it for hours. Probing costs one small copy and one extra queue
// submission per frame; the game's threads never wait on the GPU because of it.
//
// Enable: LAGTRACE=1. Off switch: DISABLE_LAGTRACE=1.
// Options: LAGTRACE_PROBE=cx,cy[,size]  probe centre as fractions of the frame, size in px
//          LAGTRACE_WINDOW_MS=300       how long after a press frames are logged
//          LAGTRACE_LOG=path            default /dev/shm/lagtrace-<uid>/<pid>.log (%p = pid)
//          LAGTRACE_DEVICES=a,b         only watch input devices whose name contains one of these
//          LAGTRACE_NO_TIMING=1         don't ask for display timing (end point = GPU done)
//          LAGTRACE_DEBUG=1             messages on stderr
// Reading /dev/input needs your user in the `input` group.
#define _GNU_SOURCE
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <linux/input.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#define LAGTRACE_VERSION "0.1.0"
#define EXPORT __attribute__((visibility("default")))
#define KEY(h) (*(void **)(h))
#define RING 1024           // input events waiting for the next present
#define FRING 512           // recent frames per swapchain
#define MAX_IMG 16          // swapchain images we can probe
#define TIMING_QUEUE 64     // driver-side timing report queue we reserve
#define PRE_FRAMES 12       // frames logged from before each press (the baseline)
#define MAX_QUEUES 64
#define MAX_DEVS 64
#define WAIT_SLICE_NS 10000000ull

static int g_debug;
#define DBG(...) do { if (g_debug) fprintf(stderr, "lagtrace: " __VA_ARGS__); } while (0)

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// ---------------------------------------------------------------- options
static double g_probe_cx = 0.5, g_probe_cy = 0.5;
static uint32_t g_probe_size = 64;
static uint64_t g_window_ns = 300000000ull;
static bool g_no_timing;

static void read_options(void)
{
    const char *e = getenv("LAGTRACE_DEBUG");
    g_debug = e ? atoi(e) : 0;
    if ((e = getenv("LAGTRACE_PROBE"))) {
        double cx, cy;
        int size = 0;
        int n = sscanf(e, "%lf,%lf,%d", &cx, &cy, &size);
        if (n >= 2 && cx >= 0 && cx <= 1 && cy >= 0 && cy <= 1) {
            g_probe_cx = cx;
            g_probe_cy = cy;
        }
        if (n == 3 && size >= 4 && size <= 512)
            g_probe_size = (uint32_t)size;
    }
    if ((e = getenv("LAGTRACE_WINDOW_MS")) && atoi(e) >= 20 && atoi(e) <= 5000)
        g_window_ns = (uint64_t)atoi(e) * 1000000ull;
    g_no_timing = getenv("LAGTRACE_NO_TIMING") != NULL;
}

// ---------------------------------------------------------------- log
// Lines are appended to a memory buffer (a few hundred ns under a lock) and written to
// the file by a separate thread, so no file I/O ever happens on the game's threads.
static pthread_mutex_t g_log_mtx = PTHREAD_MUTEX_INITIALIZER;
static char *g_lbuf;
static size_t g_llen, g_lcap;
static FILE *g_lfile;
static char g_log_path[512];

static void lt_vlog(const char *fmt, va_list ap) // g_log_mtx held
{
    if (!g_lfile)
        return;
    for (;;) {
        size_t room = g_lcap - g_llen;
        va_list cp;
        va_copy(cp, ap);
        int n = vsnprintf(g_lbuf ? g_lbuf + g_llen : NULL, room, fmt, cp);
        va_end(cp);
        if (n < 0)
            return;
        if ((size_t)n < room) {
            g_llen += (size_t)n;
            return;
        }
        size_t ncap = g_lcap ? g_lcap * 2 : 65536;
        while (ncap - g_llen <= (size_t)n)
            ncap *= 2;
        if (ncap > (64u << 20))
            return; // the writer is stuck; drop rather than grow forever
        char *nb = realloc(g_lbuf, ncap);
        if (!nb)
            return;
        g_lbuf = nb;
        g_lcap = ncap;
    }
}

__attribute__((format(printf, 1, 2))) static void lt_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&g_log_mtx);
    lt_vlog(fmt, ap);
    pthread_mutex_unlock(&g_log_mtx);
    va_end(ap);
}

static pthread_mutex_t g_write_mtx = PTHREAD_MUTEX_INITIALIZER;
static char *g_spare;
static size_t g_spare_cap;

static void log_flush(void)
{
    pthread_mutex_lock(&g_write_mtx);
    pthread_mutex_lock(&g_log_mtx);
    if (!g_llen || !g_lfile) {
        pthread_mutex_unlock(&g_log_mtx);
        pthread_mutex_unlock(&g_write_mtx);
        return;
    }
    char *full = g_lbuf;
    size_t n = g_llen, cap = g_lcap;
    g_lbuf = g_spare;
    g_lcap = g_spare_cap;
    g_llen = 0;
    pthread_mutex_unlock(&g_log_mtx);
    fwrite(full, 1, n, g_lfile);
    fflush(g_lfile);
    g_spare = full;
    g_spare_cap = cap;
    pthread_mutex_unlock(&g_write_mtx);
}

static void *log_thread(void *arg)
{
    (void)arg;
    for (;;) {
        struct timespec ts = { 0, 25000000 };
        nanosleep(&ts, NULL);
        log_flush();
    }
    return NULL;
}

static void open_log(void)
{
    const char *path = getenv("LAGTRACE_LOG");
    if (path && *path) {
        // "%p" becomes the pid, so a game and the gamescope around it get separate logs.
        const char *pp = strstr(path, "%p");
        if (pp)
            snprintf(g_log_path, sizeof g_log_path, "%.*s%d%s", (int)(pp - path), path, (int)getpid(), pp + 2);
        else
            snprintf(g_log_path, sizeof g_log_path, "%s", path);
    } else {
        // /dev/shm is shared with the Steam Linux Runtime container, so `lagtrace attach`
        // finds logs from Steam games too.
        char dir[64];
        snprintf(dir, sizeof dir, "/dev/shm/lagtrace-%u", (unsigned)getuid());
        mkdir(dir, 0700);
        DIR *dh = opendir(dir); // sweep logs of games that are gone
        if (dh) {
            struct dirent *e;
            while ((e = readdir(dh))) {
                int pid = atoi(e->d_name);
                if (pid > 0 && kill(pid, 0) < 0 && errno == ESRCH) {
                    char old[400];
                    snprintf(old, sizeof old, "%s/%s", dir, e->d_name);
                    unlink(old);
                }
            }
            closedir(dh);
        }
        snprintf(g_log_path, sizeof g_log_path, "%s/%d.log", dir, (int)getpid());
    }
    FILE *f = fopen(g_log_path, "we");
    if (!f) {
        DBG("can't open %s: %s\n", g_log_path, strerror(errno));
        return;
    }
    char comm[64] = "?";
    FILE *c = fopen("/proc/self/comm", "re");
    if (c) {
        if (fgets(comm, sizeof comm, c))
            comm[strcspn(comm, "\n")] = 0;
        fclose(c);
    }
    pthread_mutex_lock(&g_log_mtx);
    g_lfile = f;
    pthread_mutex_unlock(&g_log_mtx);
    lt_log("# lagtrace log 1\nI %d %lu %s\nO %s %.4f %.4f %u %lu\n", (int)getpid(), (unsigned long)now_ns(), comm,
           LAGTRACE_VERSION, g_probe_cx, g_probe_cy, g_probe_size, (unsigned long)(g_window_ns / 1000000ull));
    if (getenv("GAMESCOPE_WAYLAND_DISPLAY"))
        lt_log("E gamescope\n"); // display times then come from gamescope, see the README
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &a, log_thread, NULL) == 0)
        pthread_setname_np(t, "lagtrace-log");
    pthread_attr_destroy(&a);
    atexit(log_flush);
    DBG("logging to %s\n", g_log_path);
}

// ---------------------------------------------------------------- input
typedef struct {
    uint64_t t;
    uint16_t code;
    uint8_t dev;
    uint8_t down;
} ev_t;

static ev_t g_ring[RING];
static _Atomic uint64_t g_head; // written by the input thread only
static uint64_t g_tail;         // under g_tail_mtx
static pthread_mutex_t g_tail_mtx = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t g_window_end; // frames presented before this are logged

static void push_ev(uint64_t t, uint16_t code, uint8_t dev, bool down)
{
    uint64_t h = atomic_load_explicit(&g_head, memory_order_relaxed);
    pthread_mutex_lock(&g_tail_mtx);
    bool full = h - g_tail >= RING;
    pthread_mutex_unlock(&g_tail_mtx);
    if (full)
        return; // nobody is presenting; drop
    g_ring[h % RING] = (ev_t){ t, code, dev, down };
    atomic_store_explicit(&g_head, h + 1, memory_order_release);
}

#define BITS_LONG (8 * sizeof(long))
#define TEST_BIT(arr, b) ((arr)[(b) / BITS_LONG] & (1ul << ((b) % BITS_LONG)))

static bool want_device(int fd, const char *name)
{
    unsigned long keys[KEY_CNT / BITS_LONG + 1] = { 0 };
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys) < 0)
        return false;
    if (!(TEST_BIT(keys, BTN_LEFT) || TEST_BIT(keys, BTN_SOUTH) || (TEST_BIT(keys, KEY_A) && TEST_BIT(keys, KEY_SPACE))))
        return false;
    const char *filter = getenv("LAGTRACE_DEVICES");
    if (!filter || !*filter)
        return true;
    char buf[256];
    snprintf(buf, sizeof buf, "%s", filter);
    for (char *save = NULL, *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save))
        if (*tok && strcasestr(name, tok))
            return true;
    return false;
}

static int g_dev_fd[MAX_DEVS];
static dev_t g_dev_rdev[MAX_DEVS];
static uint8_t g_dev_idx[MAX_DEVS];
static int g_ndevs;
static unsigned g_next_dev_idx;

static void drop_device(int ep, int fd)
{
    epoll_ctl(ep, EPOLL_CTL_DEL, fd, NULL);
    close(fd);
    for (int i = 0; i < g_ndevs; i++)
        if (g_dev_fd[i] == fd) {
            g_ndevs--;
            g_dev_fd[i] = g_dev_fd[g_ndevs];
            g_dev_rdev[i] = g_dev_rdev[g_ndevs];
            g_dev_idx[i] = g_dev_idx[g_ndevs];
            break;
        }
}

static void add_device(int ep, const char *node)
{
    if (strncmp(node, "event", 5) != 0)
        return;
    char path[64];
    snprintf(path, sizeof path, "/dev/input/%s", node);
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return;
    struct stat st;
    bool seen = fstat(fd, &st) != 0 || g_ndevs >= MAX_DEVS;
    for (int i = 0; !seen && i < g_ndevs; i++)
        seen = g_dev_rdev[i] == st.st_rdev; // IN_ATTRIB after IN_CREATE: already watching it
    char name[128] = "?";
    if (!seen && ioctl(fd, EVIOCGNAME(sizeof name), name) < 0)
        snprintf(name, sizeof name, "?");
    name[sizeof name - 1] = 0;
    for (char *p = name; *p; p++)
        if (*p == '\n' || *p == '\r')
            *p = ' ';
    if (seen || !want_device(fd, name)) {
        close(fd);
        return;
    }
    int clk = CLOCK_MONOTONIC;
    ioctl(fd, EVIOCSCLOCKID, &clk);
    // Kernel-side filter: only EV_KEY reaches us, so mouse motion never wakes this thread.
    unsigned long types[EV_CNT / BITS_LONG + 1] = { 0 };
    types[EV_KEY / BITS_LONG] |= 1ul << (EV_KEY % BITS_LONG);
    struct input_mask m = { .type = EV_SYN, .codes_size = sizeof types, .codes_ptr = (uint64_t)(uintptr_t)types };
    if (ioctl(fd, EVIOCSMASK, &m) < 0)
        DBG("EVIOCSMASK failed on %s (%s); motion will wake us\n", path, strerror(errno));
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = fd };
    if (epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev) < 0) {
        close(fd);
        return;
    }
    uint8_t idx = (uint8_t)(g_next_dev_idx++ & 0xff);
    g_dev_fd[g_ndevs] = fd;
    g_dev_rdev[g_ndevs] = st.st_rdev;
    g_dev_idx[g_ndevs++] = idx;
    lt_log("D %u %s\n", (unsigned)idx, name);
    DBG("watching %s (%s)\n", path, name);
}

static uint8_t dev_idx_of(int fd)
{
    for (int i = 0; i < g_ndevs; i++)
        if (g_dev_fd[i] == fd)
            return g_dev_idx[i];
    return 255;
}

static void *input_thread(void *arg)
{
    (void)arg;
    int ep = epoll_create1(EPOLL_CLOEXEC);
    if (ep < 0)
        return NULL;
    int ino = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ino >= 0) {
        // New devices (like the CLI's virtual mouse) show up while the game runs.
        inotify_add_watch(ino, "/dev/input", IN_CREATE | IN_ATTRIB);
        struct epoll_event ev = { .events = EPOLLIN, .data.fd = ino };
        epoll_ctl(ep, EPOLL_CTL_ADD, ino, &ev);
    }
    DIR *d = opendir("/dev/input");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)))
            add_device(ep, e->d_name);
        closedir(d);
    } else {
        lt_log("X no-input %s\n", strerror(errno));
    }
    struct epoll_event evs[8];
    for (;;) {
        int n = epoll_wait(ep, evs, 8, -1);
        if (n < 0 && errno != EINTR)
            break;
        for (int i = 0; i < n; i++) {
            int fd = evs[i].data.fd;
            if (fd == ino) {
                char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
                ssize_t len;
                while ((len = read(ino, buf, sizeof buf)) > 0)
                    for (char *p = buf; p < buf + len;) {
                        struct inotify_event *ie = (struct inotify_event *)p;
                        if (ie->len) // IN_ATTRIB: udev fixes permissions just after IN_CREATE
                            add_device(ep, ie->name);
                        p += sizeof *ie + ie->len;
                    }
                continue;
            }
            struct input_event ie[64];
            ssize_t len = read(fd, ie, sizeof ie);
            if (len < 0 && errno != EAGAIN) { // unplugged
                drop_device(ep, fd);
                continue;
            }
            uint8_t idx = dev_idx_of(fd);
            for (ssize_t k = 0; k < len / (ssize_t)sizeof ie[0]; k++)
                if (ie[k].type == EV_KEY && ie[k].value != 2) // press or release, not autorepeat
                    push_ev((uint64_t)ie[k].input_event_sec * 1000000000ull + (uint64_t)ie[k].input_event_usec * 1000ull,
                            ie[k].code, idx, ie[k].value == 1);
        }
    }
    return NULL;
}

static pthread_once_t g_start_once = PTHREAD_ONCE_INIT;
static void start_once(void)
{
    open_log();
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &a, input_thread, NULL) == 0)
        pthread_setname_np(t, "lagtrace-input");
    pthread_attr_destroy(&a);
}

// ---------------------------------------------------------------- pixels
static uint32_t fmt_bpp(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        return 4;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        return 8;
    default:
        return 0;
    }
}

static float half_to_float(uint16_t h)
{
    uint32_t e = (h >> 10) & 0x1f, m = h & 0x3ff;
    float v;
    if (e == 0)
        v = (float)m / 16777216.0f;
    else if (e == 31)
        v = m ? 0.0f : 65504.0f;
    else if (e >= 15)
        v = (1.0f + (float)m / 1024.0f) * (float)(1u << (e - 15));
    else
        v = (1.0f + (float)m / 1024.0f) / (float)(1u << (15 - e));
    return (h & 0x8000) ? -v : v;
}

// Mean luma of the probe region on a 0-255 scale (gamma-encoded values; that's fine for
// telling whether the picture changed).
static float probe_luma(const uint8_t *p, uint32_t npx, VkFormat f)
{
    double sum = 0;
    for (uint32_t i = 0; i < npx; i++) {
        float r, g, b;
        uint32_t v;
        switch (f) {
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB:
            b = p[4 * i], g = p[4 * i + 1], r = p[4 * i + 2];
            break;
        case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
            memcpy(&v, p + 4 * i, 4);
            b = (float)(v & 0x3ff) / 4.0118f, g = (float)((v >> 10) & 0x3ff) / 4.0118f, r = (float)((v >> 20) & 0x3ff) / 4.0118f;
            break;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
            memcpy(&v, p + 4 * i, 4);
            r = (float)(v & 0x3ff) / 4.0118f, g = (float)((v >> 10) & 0x3ff) / 4.0118f, b = (float)((v >> 20) & 0x3ff) / 4.0118f;
            break;
        case VK_FORMAT_R16G16B16A16_SFLOAT: {
            uint16_t hv[3];
            memcpy(hv, p + 8 * i, 6);
            float c[3];
            for (int k = 0; k < 3; k++) {
                c[k] = half_to_float(hv[k]);
                c[k] = c[k] < 0 ? 0 : c[k] > 1 ? 1 : c[k];
            }
            r = c[0] * 255, g = c[1] * 255, b = c[2] * 255;
            break;
        }
        default: // RGBA byte order
            r = p[4 * i], g = p[4 * i + 1], b = p[4 * i + 2];
            break;
        }
        sum += 0.2126 * r + 0.7152 * g + 0.0722 * b;
    }
    return npx ? (float)(sum / npx) : 0.0f;
}

// ---------------------------------------------------------------- dispatch
typedef struct inst_s {
    void *key;
    VkInstance inst;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkDestroyInstance destroy;
    PFN_vkEnumerateDeviceExtensionProperties enum_ext;
    PFN_vkGetPhysicalDeviceFeatures2 feat2;
    PFN_vkGetPhysicalDeviceProperties props;
    PFN_vkGetPhysicalDeviceProperties2 props2;
    PFN_vkGetPhysicalDeviceMemoryProperties memprops;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties qfprops;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR surf_caps;
    struct inst_s *next;
} inst_t;

enum { SLOT_IDLE, SLOT_SUBMITTED, SLOT_DONE };

typedef struct {
    VkCommandBuffer cb;
    VkFence fence;
    VkSemaphore sem;
    int state;
    uint64_t seq;    // frame this copy belongs to
    uint64_t t_done; // when the worker saw the fence signal = GPU finished the frame
} slot_t;

typedef struct {
    uint64_t seq, t_present, t_gpu;
    float luma;
    uint8_t probe; // 0 not probed, 1 copy in flight, 2 read back
    bool logged;   // F was written, so P and S get written when they arrive
} frame_t;

typedef struct sc_s {
    VkSwapchainKHR h;
    int id;
    VkFormat fmt;
    VkExtent2D ext;
    VkPresentModeKHR mode;
    uint32_t nimg;
    VkImage img[MAX_IMG];
    slot_t slot[MAX_IMG];
    bool probe;            // TRANSFER_SRC usage + supported format + buffers ready
    bool pool_failed;
    VkOffset2D poff;
    VkExtent2D pext;
    VkDeviceSize stride;
    VkBuffer buf;
    VkDeviceMemory mem;
    uint8_t *map;
    VkCommandPool pool;
    uint32_t family;
    bool timing;           // VK_EXT_present_timing usable
    uint64_t domain_id;
    uint32_t outstanding;  // our timing requests still in the driver's queue
    uint64_t tq[TIMING_QUEUE]; // which frame each outstanding request belongs to, in order
    uint32_t tq_head, tq_n;
    uint64_t seq;
    frame_t fr[FRING];
    bool dead;             // retired (oldSwapchain) or being destroyed
    int busy;              // worker waiting on one of our fences
    struct sc_s *next;
} sc_t;

typedef struct ldev_s {
    void *key;
    VkDevice dev;
    VkPhysicalDevice pd;
    inst_t *in;
    PFN_vkGetDeviceProcAddr gdpa;
    PFN_vkSetDeviceLoaderData set_loader_data;
    PFN_vkDestroyDevice destroy;
    PFN_vkQueuePresentKHR present;
    PFN_vkCreateSwapchainKHR create_sc;
    PFN_vkDestroySwapchainKHR destroy_sc;
    PFN_vkGetSwapchainImagesKHR sc_images;
    PFN_vkGetDeviceQueue get_queue;
    PFN_vkGetDeviceQueue2 get_queue2;
    PFN_vkSetSwapchainPresentTimingQueueSizeEXT timing_qsize;
    PFN_vkGetSwapchainTimeDomainPropertiesEXT timing_domains;
    PFN_vkGetPastPresentationTimingEXT timing_past;
    PFN_vkQueueSubmit queue_submit;
    PFN_vkCreateFence create_fence;
    PFN_vkDestroyFence destroy_fence;
    PFN_vkResetFences reset_fences;
    PFN_vkWaitForFences wait_fences;
    PFN_vkCreateSemaphore create_sem;
    PFN_vkDestroySemaphore destroy_sem;
    PFN_vkCreateCommandPool create_pool;
    PFN_vkDestroyCommandPool destroy_pool;
    PFN_vkAllocateCommandBuffers alloc_cb;
    PFN_vkResetCommandBuffer reset_cb;
    PFN_vkBeginCommandBuffer begin_cb;
    PFN_vkEndCommandBuffer end_cb;
    PFN_vkCmdPipelineBarrier cmd_barrier;
    PFN_vkCmdCopyImageToBuffer cmd_copy;
    PFN_vkCreateBuffer create_buf;
    PFN_vkDestroyBuffer destroy_buf;
    PFN_vkGetBufferMemoryRequirements buf_reqs;
    PFN_vkAllocateMemory alloc_mem;
    PFN_vkFreeMemory free_mem;
    PFN_vkBindBufferMemory bind_mem;
    PFN_vkMapMemory map_mem;
    VkPhysicalDeviceMemoryProperties memp;
    uint32_t nfam;
    VkQueueFlags fam_flags[32];
    VkQueue queues[MAX_QUEUES];
    uint32_t queue_fam[MAX_QUEUES];
    int nqueues;
    _Atomic bool active;
    bool probe_ok;  // the functions for probing are all there
    pthread_mutex_t mtx;
    pthread_cond_t cv;      // new copies to wait for / quit
    pthread_cond_t idle_cv; // a swapchain's busy -> 0
    sc_t *scs;
    int next_sc_id;
    bool quit, has_thread;
    pthread_t thread;
    struct ldev_s *next;
} ldev_t;

static pthread_mutex_t g_map_mtx = PTHREAD_MUTEX_INITIALIZER;
static inst_t *g_insts;
static ldev_t *g_devs;

static inst_t *find_inst(void *key)
{
    pthread_mutex_lock(&g_map_mtx);
    inst_t *i = g_insts;
    while (i && i->key != key)
        i = i->next;
    pthread_mutex_unlock(&g_map_mtx);
    return i;
}

// Device records are never freed (a few KB per destroyed device), so the last one used
// can be cached and read without a lock on every present.
static _Atomic(ldev_t *) g_dev_cache;

static ldev_t *find_dev(void *key)
{
    ldev_t *c = atomic_load_explicit(&g_dev_cache, memory_order_acquire);
    if (c && c->key == key)
        return c;
    pthread_mutex_lock(&g_map_mtx);
    ldev_t *d = g_devs;
    while (d && d->key != key)
        d = d->next;
    pthread_mutex_unlock(&g_map_mtx);
    if (d)
        atomic_store_explicit(&g_dev_cache, d, memory_order_release);
    return d;
}

static sc_t *find_sc(ldev_t *d, VkSwapchainKHR h) // d->mtx held
{
    sc_t *s = d->scs;
    while (s && s->h != h)
        s = s->next;
    return s;
}

static uint32_t queue_family(ldev_t *d, VkQueue q) // d->mtx held
{
    for (int i = 0; i < d->nqueues; i++)
        if (d->queues[i] == q)
            return d->queue_fam[i];
    return UINT32_MAX;
}

// ---------------------------------------------------------------- probe resources
static void probe_free(ldev_t *d, sc_t *s) // device must not be using them anymore
{
    for (uint32_t i = 0; i < s->nimg; i++) {
        if (s->slot[i].fence)
            d->destroy_fence(d->dev, s->slot[i].fence, NULL);
        if (s->slot[i].sem)
            d->destroy_sem(d->dev, s->slot[i].sem, NULL);
        s->slot[i].fence = VK_NULL_HANDLE;
        s->slot[i].sem = VK_NULL_HANDLE;
    }
    if (s->pool)
        d->destroy_pool(d->dev, s->pool, NULL); // frees the command buffers too
    if (s->buf)
        d->destroy_buf(d->dev, s->buf, NULL);
    if (s->mem)
        d->free_mem(d->dev, s->mem, NULL); // unmaps
    s->pool = VK_NULL_HANDLE;
    s->buf = VK_NULL_HANDLE;
    s->mem = VK_NULL_HANDLE;
    s->map = NULL;
    s->probe = false;
}

static bool probe_setup(ldev_t *d, sc_t *s)
{
    uint32_t bpp = fmt_bpp(s->fmt);
    if (!bpp || !s->nimg || !s->ext.width || !s->ext.height)
        return false;
    uint32_t w = g_probe_size < s->ext.width ? g_probe_size : s->ext.width;
    uint32_t h = g_probe_size < s->ext.height ? g_probe_size : s->ext.height;
    int64_t x = (int64_t)(g_probe_cx * s->ext.width) - w / 2, y = (int64_t)(g_probe_cy * s->ext.height) - h / 2;
    x = x < 0 ? 0 : x > (int64_t)(s->ext.width - w) ? (int64_t)(s->ext.width - w) : x;
    y = y < 0 ? 0 : y > (int64_t)(s->ext.height - h) ? (int64_t)(s->ext.height - h) : y;
    s->poff = (VkOffset2D){ (int32_t)x, (int32_t)y };
    s->pext = (VkExtent2D){ w, h };
    s->stride = ((VkDeviceSize)w * h * bpp + 255) & ~(VkDeviceSize)255;

    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = s->stride * s->nimg,
                               .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    if (d->create_buf(d->dev, &bci, NULL, &s->buf) != VK_SUCCESS)
        return false;
    VkMemoryRequirements req;
    d->buf_reqs(d->dev, s->buf, &req);
    // Host-visible and coherent; cached if there is such a type (CPU reads are faster).
    const VkMemoryPropertyFlags need = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t type = UINT32_MAX;
    for (int pass = 0; pass < 2 && type == UINT32_MAX; pass++)
        for (uint32_t i = 0; i < d->memp.memoryTypeCount; i++) {
            VkMemoryPropertyFlags f = d->memp.memoryTypes[i].propertyFlags;
            bool ok = (req.memoryTypeBits & (1u << i)) && (f & need) == need;
            if (pass == 0)
                ok = ok && (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
            if (ok) {
                type = i;
                break;
            }
        }
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = type };
    void *map = NULL;
    if (type == UINT32_MAX || d->alloc_mem(d->dev, &mai, NULL, &s->mem) != VK_SUCCESS ||
        d->bind_mem(d->dev, s->buf, s->mem, 0) != VK_SUCCESS || d->map_mem(d->dev, s->mem, 0, VK_WHOLE_SIZE, 0, &map) != VK_SUCCESS) {
        probe_free(d, s);
        return false;
    }
    s->map = map;
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (uint32_t i = 0; i < s->nimg; i++)
        if (d->create_fence(d->dev, &fci, NULL, &s->slot[i].fence) != VK_SUCCESS ||
            d->create_sem(d->dev, &sci, NULL, &s->slot[i].sem) != VK_SUCCESS) {
            probe_free(d, s);
            return false;
        }
    return true;
}

// The command pool needs the queue family, which we only learn at the first present.
static bool probe_pool(ldev_t *d, sc_t *s, uint32_t fam) // d->mtx held
{
    const VkQueueFlags copy_ok = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
    if (fam >= d->nfam || !(d->fam_flags[fam] & copy_ok))
        return false;
    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                    .queueFamilyIndex = fam };
    if (d->create_pool(d->dev, &pci, NULL, &s->pool) != VK_SUCCESS)
        return false;
    VkCommandBuffer cbs[MAX_IMG];
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = s->pool,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = s->nimg };
    if (d->alloc_cb(d->dev, &cai, cbs) != VK_SUCCESS) {
        d->destroy_pool(d->dev, s->pool, NULL);
        s->pool = VK_NULL_HANDLE;
        return false;
    }
    for (uint32_t i = 0; i < s->nimg; i++) {
        // Command buffers are dispatchable: the loader's dispatch pointer must be set.
        if (d->set_loader_data)
            d->set_loader_data(d->dev, cbs[i]);
        else
            *(void **)cbs[i] = *(void **)d->dev;
        s->slot[i].cb = cbs[i];
    }
    s->family = fam;
    return true;
}

// Queue the copy of this frame's probe region behind the game's rendering. It waits on
// the game's "rendering done" semaphores and signals ours, which the present then waits on.
static bool probe_submit(ldev_t *d, sc_t *s, VkQueue q, uint32_t idx, uint64_t seq, const VkPresentInfoKHR *pi) // d->mtx held
{
    if (!s->probe || idx >= s->nimg || pi->waitSemaphoreCount > 16)
        return false;
    uint32_t fam = queue_family(d, q);
    if (!s->pool) {
        if (s->pool_failed || fam == UINT32_MAX)
            return false;
        if (!probe_pool(d, s, fam)) {
            s->pool_failed = true;
            lt_log("X no-pool %d\n", s->id);
            return false;
        }
    } else if (fam != s->family) {
        return false;
    }
    slot_t *sl = &s->slot[idx];
    if (sl->state != SLOT_IDLE)
        return false; // last copy of this image not collected yet; skip rather than wait
    VkCommandBuffer cb = sl->cb;
    if (d->reset_fences(d->dev, 1, &sl->fence) != VK_SUCCESS || d->reset_cb(cb, 0) != VK_SUCCESS)
        return false;
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    if (d->begin_cb(cb, &bi) != VK_SUCCESS)
        return false;
    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkImageMemoryBarrier to_src = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                    .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                                    .oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .image = s->img[idx], .subresourceRange = range };
    d->cmd_barrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_src);
    VkBufferImageCopy region = { .bufferOffset = s->stride * idx,
                                 .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                 .imageOffset = { s->poff.x, s->poff.y, 0 },
                                 .imageExtent = { s->pext.width, s->pext.height, 1 } };
    d->cmd_copy(cb, s->img[idx], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s->buf, 1, &region);
    VkImageMemoryBarrier to_present = to_src;
    to_present.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    to_present.dstAccessMask = 0;
    to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkBufferMemoryBarrier to_host = { .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                                      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
                                      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                      .buffer = s->buf, .offset = s->stride * idx, .size = s->stride };
    d->cmd_barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 0,
                   NULL, 1, &to_host, 1, &to_present);
    if (d->end_cb(cb) != VK_SUCCESS)
        return false;
    VkPipelineStageFlags stages[16];
    for (uint32_t i = 0; i < pi->waitSemaphoreCount; i++)
        stages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                        .waitSemaphoreCount = pi->waitSemaphoreCount, .pWaitSemaphores = pi->pWaitSemaphores,
                        .pWaitDstStageMask = stages, .commandBufferCount = 1, .pCommandBuffers = &cb,
                        .signalSemaphoreCount = 1, .pSignalSemaphores = &sl->sem };
    if (d->queue_submit(q, 1, &si, sl->fence) != VK_SUCCESS)
        return false; // nothing queued, the game's semaphores are untouched: present as asked
    sl->state = SLOT_SUBMITTED;
    sl->seq = seq;
    pthread_cond_signal(&d->cv);
    return true;
}

// Read back copies the worker saw finish.
static void collect_probes(sc_t *s) // d->mtx held
{
    uint32_t npx = s->pext.width * s->pext.height;
    for (uint32_t i = 0; i < s->nimg; i++) {
        slot_t *sl = &s->slot[i];
        if (sl->state != SLOT_DONE)
            continue;
        frame_t *f = &s->fr[sl->seq % FRING];
        if (f->seq == sl->seq) {
            f->luma = probe_luma(s->map + s->stride * i, npx, s->fmt);
            f->t_gpu = sl->t_done;
            f->probe = 2;
            if (f->logged)
                lt_log("P %d %lu %.2f %lu\n", s->id, (unsigned long)f->seq, f->luma, (unsigned long)f->t_gpu);
        }
        sl->state = SLOT_IDLE;
    }
}

// ---------------------------------------------------------------- present timing
static void drain_timing(ldev_t *d, sc_t *s, uint64_t now) // d->mtx held
{
    enum { N = 16 };
    VkPresentStageTimeEXT stages[N][4];
    VkPastPresentationTimingEXT recs[N];
    for (int i = 0; i < N; i++)
        recs[i] = (VkPastPresentationTimingEXT){ .sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_EXT,
                                                 .presentStageCount = 4, .pPresentStages = stages[i] };
    VkPastPresentationTimingPropertiesEXT props = { .sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_PROPERTIES_EXT,
                                                    .presentationTimingCount = N, .pPresentationTimings = recs };
    VkPastPresentationTimingInfoEXT info = { .sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_INFO_EXT, .swapchain = s->h };
    VkResult r = d->timing_past(d->dev, &info, &props);
    if (r < 0) {
        lt_log("X timing-failed %d %d\n", s->id, r);
        s->timing = false;
        s->outstanding = 0;
        s->tq_n = 0;
        return;
    }
    uint32_t got = props.presentationTimingCount;
    for (uint32_t k = 0; k < got && k < N && s->tq_n; k++) {
        // Reports come back in present order, one per request.
        uint64_t seq = s->tq[s->tq_head];
        s->tq_head = (s->tq_head + 1) % TIMING_QUEUE;
        s->tq_n--;
        s->outstanding--;
        // First-pixel-out = the compositor's flip (Wayland, gamescope). XWayland only
        // reports "dequeued" (X Present complete): real with vsync, a stale vblank without.
        uint64_t t = 0, t_deq = 0;
        for (uint32_t st = 0; st < recs[k].presentStageCount && st < 4; st++) {
            if (stages[k][st].stage & VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT)
                t = stages[k][st].time;
            else if (stages[k][st].stage & VK_PRESENT_STAGE_REQUEST_DEQUEUED_BIT_EXT)
                t_deq = stages[k][st].time;
        }
        const char *kind = "px";
        if (!t && t_deq) {
            t = t_deq;
            kind = "dq";
        }
        frame_t *f = &s->fr[seq % FRING];
        if (t && f->seq == seq && (t + 1000000ull < f->t_present || t > now + 50000000ull)) {
            // Scanout can't be before the present call, nor far in the future (gamescope
            // reports its scheduled vblank, a few ms ahead). Otherwise the driver's clock
            // isn't CLOCK_MONOTONIC and the numbers would be garbage.
            lt_log("X timing-clock %d %lu %lu\n", s->id, (unsigned long)t, (unsigned long)f->t_present);
            s->timing = false;
            s->outstanding = 0;
            s->tq_n = 0;
            return;
        }
        if (f->seq == seq && f->logged)
            lt_log("S %d %lu %lu %s\n", s->id, (unsigned long)seq, (unsigned long)t, t ? kind : "drop");
    }
}

// ---------------------------------------------------------------- worker thread
// Waits for our copies to finish, oldest first, and notes when each did: that's when the
// GPU finished the game's frame (our copy only runs after it).
static void *worker_thread(void *arg)
{
    ldev_t *d = arg;
    pthread_mutex_lock(&d->mtx);
    while (!d->quit) {
        sc_t *bs = NULL;
        slot_t *bsl = NULL;
        for (sc_t *s = d->scs; s; s = s->next)
            for (uint32_t i = 0; i < s->nimg; i++)
                if (s->slot[i].state == SLOT_SUBMITTED && (!bsl || s->slot[i].seq < bsl->seq)) {
                    bs = s;
                    bsl = &s->slot[i];
                }
        if (!bsl) {
            pthread_cond_wait(&d->cv, &d->mtx);
            continue;
        }
        VkFence fence = bsl->fence;
        uint64_t seq = bsl->seq;
        bs->busy++;
        pthread_mutex_unlock(&d->mtx);
        VkResult r = d->wait_fences(d->dev, 1, &fence, VK_TRUE, WAIT_SLICE_NS);
        uint64_t t = now_ns();
        pthread_mutex_lock(&d->mtx);
        if (r == VK_SUCCESS && bsl->state == SLOT_SUBMITTED && bsl->seq == seq) {
            bsl->state = SLOT_DONE;
            bsl->t_done = t;
        } else if (r != VK_SUCCESS && r != VK_TIMEOUT) {
            lt_log("X wait-failed %d\n", r);
            atomic_store(&d->active, false);
            d->quit = true;
        }
        if (--bs->busy == 0)
            pthread_cond_broadcast(&d->idle_cv);
    }
    pthread_mutex_unlock(&d->mtx);
    return NULL;
}

// ---------------------------------------------------------------- per-present work
static void take_events(sc_t *s) // d->mtx held
{
    uint64_t head = atomic_load_explicit(&g_head, memory_order_acquire);
    bool press = false;
    pthread_mutex_lock(&g_tail_mtx);
    while (g_tail != head) {
        ev_t e = g_ring[g_tail % RING];
        g_tail++;
        lt_log("%c %lu %u %u\n", e.down ? 'C' : 'R', (unsigned long)e.t, (unsigned)e.dev, (unsigned)e.code);
        if (e.down) {
            press = true;
            uint64_t end = e.t + g_window_ns;
            if (end > atomic_load(&g_window_end))
                atomic_store(&g_window_end, end);
        }
    }
    pthread_mutex_unlock(&g_tail_mtx);
    if (!press)
        return;
    // Log the frames just before the press too: they're the baseline the CLI compares to.
    uint64_t from = s->seq > PRE_FRAMES ? s->seq - PRE_FRAMES + 1 : 1;
    for (uint64_t q = from; q <= s->seq; q++) {
        frame_t *f = &s->fr[q % FRING];
        if (f->seq != q || f->logged)
            continue;
        f->logged = true;
        lt_log("F %d %lu %lu\n", s->id, (unsigned long)q, (unsigned long)f->t_present);
        if (f->probe == 2)
            lt_log("P %d %lu %.2f %lu\n", s->id, (unsigned long)q, f->luma, (unsigned long)f->t_gpu);
    }
}

static VKAPI_ATTR VkResult VKAPI_CALL lt_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pi)
{
    ldev_t *d = find_dev(KEY(queue));
    if (!atomic_load_explicit(&d->active, memory_order_relaxed) || pi->swapchainCount != 1)
        return d->present(queue, pi);
    uint64_t t0 = now_ns();
    bool app_timing = false;
    for (const VkBaseInStructure *p = pi->pNext; p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT)
            app_timing = true;

    VkPresentInfoKHR mine = *pi;
    VkPresentTimingInfoEXT tinfo;
    VkPresentTimingsInfoEXT tinfos;
    bool modified = false, timed = false;
    sc_t *ts = NULL;

    pthread_mutex_lock(&d->mtx);
    sc_t *s = find_sc(d, pi->pSwapchains[0]);
    if (s && !s->dead) {
        take_events(s);
        if (s->timing && s->outstanding)
            drain_timing(d, s, t0);
        collect_probes(s);
        uint64_t seq = ++s->seq;
        frame_t *f = &s->fr[seq % FRING];
        *f = (frame_t){ .seq = seq, .t_present = t0, .luma = -1 };
        bool in_window = t0 <= atomic_load(&g_window_end);
        if (probe_submit(d, s, queue, pi->pImageIndices[0], seq, pi)) {
            f->probe = 1;
            mine.waitSemaphoreCount = 1;
            mine.pWaitSemaphores = &s->slot[pi->pImageIndices[0]].sem;
            modified = true;
        }
        // Display timing for frames after a press (a full queue would fail the present,
        // so we stay well under the size we reserved).
        if (in_window && s->timing && !app_timing && s->outstanding + 2 < TIMING_QUEUE) {
            tinfo = (VkPresentTimingInfoEXT){ .sType = VK_STRUCTURE_TYPE_PRESENT_TIMING_INFO_EXT,
                                              .timeDomainId = s->domain_id,
                                              .presentStageQueries = VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT |
                                                                     VK_PRESENT_STAGE_REQUEST_DEQUEUED_BIT_EXT };
            tinfos = (VkPresentTimingsInfoEXT){ .sType = VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT,
                                                .pNext = pi->pNext, .swapchainCount = 1, .pTimingInfos = &tinfo };
            mine.pNext = &tinfos;
            s->tq[(s->tq_head + s->tq_n) % TIMING_QUEUE] = seq;
            s->tq_n++;
            s->outstanding++;
            modified = timed = true;
            ts = s;
        }
        if (in_window) {
            f->logged = true;
            lt_log("F %d %lu %lu\n", s->id, (unsigned long)seq, (unsigned long)t0);
        }
    }
    pthread_mutex_unlock(&d->mtx);

    VkResult r = d->present(queue, modified ? &mine : pi);
    if (timed && r < 0 && r != VK_ERROR_OUT_OF_DATE_KHR) {
        // Rejected outright: no report will come for it.
        pthread_mutex_lock(&d->mtx);
        if (ts->tq_n) {
            ts->tq_n--;
            ts->outstanding--;
        }
        pthread_mutex_unlock(&d->mtx);
    }
    return r;
}

// ---------------------------------------------------------------- swapchains
static void retire_sc(ldev_t *d, sc_t *s) // d->mtx held
{
    s->dead = true;
    while (s->busy)
        pthread_cond_wait(&d->idle_cv, &d->mtx);
}

static void destroy_sc_data(ldev_t *d, sc_t *s) // d->mtx held, s retired
{
    // Our copies must be finished before their objects go away (bounded wait).
    for (uint32_t i = 0; i < s->nimg; i++)
        if (s->slot[i].state == SLOT_SUBMITTED && s->slot[i].fence)
            d->wait_fences(d->dev, 1, &s->slot[i].fence, VK_TRUE, 1000000000ull);
    probe_free(d, s);
    lt_log("Z %d\n", s->id);
}

static VKAPI_ATTR VkResult VKAPI_CALL lt_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *ci,
                                                             const VkAllocationCallbacks *alloc, VkSwapchainKHR *out)
{
    ldev_t *d = find_dev(KEY(device));
    if (ci->oldSwapchain != VK_NULL_HANDLE) { // the old one is retired by this call
        pthread_mutex_lock(&d->mtx);
        sc_t *old = find_sc(d, ci->oldSwapchain);
        if (old)
            retire_sc(d, old);
        pthread_mutex_unlock(&d->mtx);
    }
    // We need to copy out of the images (TRANSFER_SRC) and want display timing, unless
    // the app uses present timing itself (then the reports are its own).
    bool shared = ci->presentMode == VK_PRESENT_MODE_SHARED_DEMAND_REFRESH_KHR ||
                  ci->presentMode == VK_PRESENT_MODE_SHARED_CONTINUOUS_REFRESH_KHR;
    bool want_src = d->probe_ok && !shared && fmt_bpp(ci->imageFormat) && !(ci->imageUsage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    bool have_src = d->probe_ok && !shared && fmt_bpp(ci->imageFormat) && (ci->imageUsage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (want_src) {
        VkSurfaceCapabilitiesKHR caps;
        if (!d->in->surf_caps || d->in->surf_caps(d->pd, ci->surface, &caps) != VK_SUCCESS ||
            !(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
            want_src = false;
    }
    bool timing = d->timing_past && !g_no_timing && !(ci->flags & VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT);
    VkSwapchainCreateInfoKHR mod = *ci;
    if (timing)
        mod.flags |= VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT;
    if (want_src)
        mod.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkResult r = d->create_sc(device, (timing || want_src) ? &mod : ci, alloc, out);
    if ((timing || want_src) && r != VK_SUCCESS) {
        // Be invisible: retry exactly as asked (the old swapchain is already retired).
        DBG("swapchain with our changes failed (%d), retrying untouched\n", r);
        timing = want_src = false;
        mod = *ci;
        mod.oldSwapchain = VK_NULL_HANDLE;
        r = d->create_sc(device, &mod, alloc, out);
    }
    if (r != VK_SUCCESS)
        return r;
    sc_t *s = calloc(1, sizeof *s);
    if (!s)
        return r;
    s->h = *out;
    s->fmt = ci->imageFormat;
    s->ext = ci->imageExtent;
    s->mode = ci->presentMode;
    uint32_t n = 0;
    if ((want_src || have_src) && d->sc_images(device, *out, &n, NULL) == VK_SUCCESS && n && n <= MAX_IMG &&
        d->sc_images(device, *out, &n, s->img) == VK_SUCCESS) {
        s->nimg = n;
        s->probe = probe_setup(d, s);
    }
    if (timing && d->timing_qsize(device, *out, TIMING_QUEUE) == VK_SUCCESS) {
        VkTimeDomainKHR dom = VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT;
        uint64_t dom_id = 0;
        VkSwapchainTimeDomainPropertiesEXT p = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_TIME_DOMAIN_PROPERTIES_EXT,
                                                 .timeDomainCount = 1, .pTimeDomains = &dom, .pTimeDomainIds = &dom_id };
        if (d->timing_domains(device, *out, &p, NULL) >= 0) {
            s->timing = true;
            s->domain_id = dom_id;
        }
    }
    pthread_mutex_lock(&d->mtx);
    s->id = d->next_sc_id++;
    s->next = d->scs;
    d->scs = s;
    pthread_mutex_unlock(&d->mtx);
    lt_log("W %d %u %u %d %d %d %d %d %d %u %u %u\n", s->id, s->ext.width, s->ext.height, (int)s->fmt, (int)s->mode,
           s->timing, s->probe, s->poff.x, s->poff.y, s->pext.width, s->pext.height, s->nimg);
    DBG("swapchain %d %ux%u fmt %d mode %d timing %d probe %d\n", s->id, s->ext.width, s->ext.height, s->fmt, s->mode, s->timing, s->probe);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL lt_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR sc, const VkAllocationCallbacks *alloc)
{
    ldev_t *d = find_dev(KEY(device));
    pthread_mutex_lock(&d->mtx);
    for (sc_t **pp = &d->scs; *pp; pp = &(*pp)->next) {
        if ((*pp)->h == sc) {
            sc_t *s = *pp;
            retire_sc(d, s);
            *pp = s->next;
            destroy_sc_data(d, s);
            free(s);
            break;
        }
    }
    pthread_mutex_unlock(&d->mtx);
    d->destroy_sc(device, sc, alloc);
}

static void note_queue(ldev_t *d, VkQueue q, uint32_t fam)
{
    if (!q)
        return;
    pthread_mutex_lock(&d->mtx);
    bool known = false;
    for (int i = 0; i < d->nqueues; i++)
        known |= d->queues[i] == q;
    if (!known && d->nqueues < MAX_QUEUES) {
        d->queues[d->nqueues] = q;
        d->queue_fam[d->nqueues++] = fam;
    }
    pthread_mutex_unlock(&d->mtx);
}

static VKAPI_ATTR void VKAPI_CALL lt_GetDeviceQueue(VkDevice device, uint32_t fam, uint32_t index, VkQueue *q)
{
    ldev_t *d = find_dev(KEY(device));
    d->get_queue(device, fam, index, q);
    note_queue(d, *q, fam);
}

static VKAPI_ATTR void VKAPI_CALL lt_GetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2 *info, VkQueue *q)
{
    ldev_t *d = find_dev(KEY(device));
    d->get_queue2(device, info, q);
    note_queue(d, *q, info->queueFamilyIndex);
}

// ---------------------------------------------------------------- device
static bool has_ext(const char *const *list, uint32_t n, const char *name)
{
    for (uint32_t i = 0; i < n; i++)
        if (strcmp(list[i], name) == 0)
            return true;
    return false;
}

// One device feature we switch on: reuse the app's struct if it has one (duplicates in a
// pNext chain are invalid), else put ours in front of the chain.
typedef struct {
    VkStructureType stype;
    size_t offset;  // of the VkBool32 we need
    void *ours;     // our struct (sType + flag set)
    VkBool32 *app;  // the app's flag, if it chained the struct itself
    VkBool32 app_old;
} feat_t;

static const void *enable_features(feat_t *f, int nf, const void *chain)
{
    for (int i = 0; i < nf; i++)
        for (VkBaseOutStructure *p = (VkBaseOutStructure *)chain; p; p = p->pNext)
            if (p->sType == f[i].stype)
                f[i].app = (VkBool32 *)((char *)p + f[i].offset);
    for (int i = 0; i < nf; i++) {
        if (f[i].app) {
            f[i].app_old = *f[i].app;
            *f[i].app = VK_TRUE;
        } else {
            ((VkBaseOutStructure *)f[i].ours)->pNext = (VkBaseOutStructure *)chain;
            chain = f[i].ours;
        }
    }
    return chain;
}

static void log_gpu(inst_t *in, VkPhysicalDevice pd)
{
    VkPhysicalDeviceDriverProperties drv = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
    VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drv };
    if (in->props2) {
        in->props2(pd, &p2);
    } else if (in->props) {
        in->props(pd, &p2.properties);
    } else {
        return;
    }
    char name[800];
    snprintf(name, sizeof name, "%s|%s %s", p2.properties.deviceName, drv.driverName, drv.driverInfo);
    for (char *c = name; *c; c++)
        if (*c == '\n')
            *c = ' ';
    lt_log("G %s\n", name);
}

static VKAPI_ATTR VkResult VKAPI_CALL lt_CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci,
                                                       const VkAllocationCallbacks *alloc, VkDevice *out)
{
    VkLayerDeviceCreateInfo *link = (VkLayerDeviceCreateInfo *)ci->pNext;
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && link->function == VK_LAYER_LINK_INFO))
        link = (VkLayerDeviceCreateInfo *)link->pNext;
    if (!link)
        return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkSetDeviceLoaderData set_data = NULL;
    for (VkLayerDeviceCreateInfo *c = (VkLayerDeviceCreateInfo *)ci->pNext; c; c = (VkLayerDeviceCreateInfo *)c->pNext)
        if (c->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && c->function == VK_LOADER_DATA_CALLBACK)
            set_data = c->u.pfnSetDeviceLoaderData;
    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;

    inst_t *in = find_inst(KEY(pd));
    PFN_vkCreateDevice next_create = (PFN_vkCreateDevice)gipa(in ? in->inst : VK_NULL_HANDLE, "vkCreateDevice");
    if (!next_create)
        return VK_ERROR_INITIALIZATION_FAILED;

    bool swapchain = has_ext(ci->ppEnabledExtensionNames, ci->enabledExtensionCount, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    bool use = in && swapchain && !getenv("DISABLE_LAGTRACE");

    // Display timing: VK_EXT_present_timing and what it builds on.
    bool has_timing = false, e_id2 = false, e_cal = false;
    if (use && in->enum_ext && in->feat2 && !g_no_timing) {
        uint32_t n = 0;
        in->enum_ext(pd, NULL, &n, NULL);
        VkExtensionProperties *props = calloc(n ? n : 1, sizeof *props);
        bool e_timing = false;
        if (props && in->enum_ext(pd, NULL, &n, props) >= 0)
            for (uint32_t i = 0; i < n; i++) {
                e_timing |= !strcmp(props[i].extensionName, VK_EXT_PRESENT_TIMING_EXTENSION_NAME);
                e_id2 |= !strcmp(props[i].extensionName, VK_KHR_PRESENT_ID_2_EXTENSION_NAME);
                e_cal |= !strcmp(props[i].extensionName, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
            }
        free(props);
        if (e_timing) {
            VkPhysicalDevicePresentTimingFeaturesEXT ft = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT };
            VkPhysicalDevicePresentId2FeaturesKHR fi = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR, .pNext = &ft };
            VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &fi };
            in->feat2(pd, &f2);
            has_timing = ft.presentTiming;
            e_id2 = e_id2 && fi.presentId2;
        }
    }

    VkDeviceCreateInfo mod = *ci;
    const char **exts = NULL;
    VkPhysicalDevicePresentTimingFeaturesEXT add_timing = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT, .presentTiming = VK_TRUE };
    VkPhysicalDevicePresentId2FeaturesKHR add_id2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR, .presentId2 = VK_TRUE };
    feat_t feats[2];
    int nf = 0;
    if (has_timing) {
        exts = malloc((ci->enabledExtensionCount + 3) * sizeof *exts);
        if (!exts) {
            has_timing = false;
        } else {
            uint32_t n = ci->enabledExtensionCount;
            memcpy(exts, ci->ppEnabledExtensionNames, n * sizeof *exts);
            if (!has_ext(exts, n, VK_EXT_PRESENT_TIMING_EXTENSION_NAME))
                exts[n++] = VK_EXT_PRESENT_TIMING_EXTENSION_NAME;
            feats[nf++] = (feat_t){ .stype = add_timing.sType, .offset = offsetof(VkPhysicalDevicePresentTimingFeaturesEXT, presentTiming), .ours = &add_timing };
            if (e_id2) {
                if (!has_ext(exts, n, VK_KHR_PRESENT_ID_2_EXTENSION_NAME))
                    exts[n++] = VK_KHR_PRESENT_ID_2_EXTENSION_NAME;
                feats[nf++] = (feat_t){ .stype = add_id2.sType, .offset = offsetof(VkPhysicalDevicePresentId2FeaturesKHR, presentId2), .ours = &add_id2 };
            }
            if (e_cal && !has_ext(exts, n, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME))
                exts[n++] = VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME;
            mod.enabledExtensionCount = n;
            mod.ppEnabledExtensionNames = exts;
            mod.pNext = enable_features(feats, nf, ci->pNext);
        }
    }
    VkResult r = next_create(pd, has_timing ? &mod : ci, alloc, out);
    for (int i = 0; i < nf; i++)
        if (feats[i].app)
            *feats[i].app = feats[i].app_old;
    free(exts);
    if (has_timing && r != VK_SUCCESS) { // be invisible if the driver didn't like it
        DBG("device creation with present timing failed (%d), retrying untouched\n", r);
        has_timing = false;
        r = next_create(pd, ci, alloc, out);
    }
    if (r != VK_SUCCESS)
        return r;

    ldev_t *d = calloc(1, sizeof *d);
    if (!d) {
        PFN_vkDestroyDevice destroy = (PFN_vkDestroyDevice)gdpa(*out, "vkDestroyDevice");
        destroy(*out, alloc);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    d->key = KEY(*out);
    d->dev = *out;
    d->pd = pd;
    d->in = in;
    d->gdpa = gdpa;
    d->set_loader_data = set_data;
#define GET(field, name) d->field = (void *)gdpa(*out, name)
    GET(destroy, "vkDestroyDevice");
    GET(get_queue, "vkGetDeviceQueue");
    GET(get_queue2, "vkGetDeviceQueue2");
    if (use) {
        GET(present, "vkQueuePresentKHR");
        GET(create_sc, "vkCreateSwapchainKHR");
        GET(destroy_sc, "vkDestroySwapchainKHR");
        GET(sc_images, "vkGetSwapchainImagesKHR");
        GET(queue_submit, "vkQueueSubmit");
        GET(create_fence, "vkCreateFence");
        GET(destroy_fence, "vkDestroyFence");
        GET(reset_fences, "vkResetFences");
        GET(wait_fences, "vkWaitForFences");
        GET(create_sem, "vkCreateSemaphore");
        GET(destroy_sem, "vkDestroySemaphore");
        GET(create_pool, "vkCreateCommandPool");
        GET(destroy_pool, "vkDestroyCommandPool");
        GET(alloc_cb, "vkAllocateCommandBuffers");
        GET(reset_cb, "vkResetCommandBuffer");
        GET(begin_cb, "vkBeginCommandBuffer");
        GET(end_cb, "vkEndCommandBuffer");
        GET(cmd_barrier, "vkCmdPipelineBarrier");
        GET(cmd_copy, "vkCmdCopyImageToBuffer");
        GET(create_buf, "vkCreateBuffer");
        GET(destroy_buf, "vkDestroyBuffer");
        GET(buf_reqs, "vkGetBufferMemoryRequirements");
        GET(alloc_mem, "vkAllocateMemory");
        GET(free_mem, "vkFreeMemory");
        GET(bind_mem, "vkBindBufferMemory");
        GET(map_mem, "vkMapMemory");
    }
    if (use && has_timing) {
        GET(timing_qsize, "vkSetSwapchainPresentTimingQueueSizeEXT");
        GET(timing_domains, "vkGetSwapchainTimeDomainPropertiesEXT");
        GET(timing_past, "vkGetPastPresentationTimingEXT");
        if (!d->timing_qsize || !d->timing_domains || !d->timing_past)
            d->timing_past = NULL;
    }
#undef GET
    d->probe_ok = d->queue_submit && d->create_fence && d->destroy_fence && d->reset_fences && d->wait_fences &&
                  d->create_sem && d->destroy_sem && d->create_pool && d->destroy_pool && d->alloc_cb && d->reset_cb &&
                  d->begin_cb && d->end_cb && d->cmd_barrier && d->cmd_copy && d->create_buf && d->destroy_buf &&
                  d->buf_reqs && d->alloc_mem && d->free_mem && d->bind_mem && d->map_mem && in->memprops && in->qfprops;
    bool active = use && d->present && d->create_sc && d->destroy_sc && d->sc_images && d->get_queue && d->probe_ok;
    if (active) {
        in->memprops(pd, &d->memp);
        uint32_t nfam = 0;
        in->qfprops(pd, &nfam, NULL);
        VkQueueFamilyProperties qf[32];
        d->nfam = nfam < 32 ? nfam : 32;
        in->qfprops(pd, &d->nfam, qf);
        for (uint32_t i = 0; i < d->nfam; i++)
            d->fam_flags[i] = qf[i].queueFlags;
    }
    pthread_mutex_init(&d->mtx, NULL);
    pthread_cond_init(&d->cv, NULL);
    pthread_cond_init(&d->idle_cv, NULL);
    if (active && pthread_create(&d->thread, NULL, worker_thread, d) == 0) {
        pthread_setname_np(d->thread, "lagtrace-gpu");
        d->has_thread = true;
        pthread_once(&g_start_once, start_once);
        log_gpu(in, pd);
    } else {
        active = false;
    }
    atomic_store(&d->active, active);
    DBG("device %p active=%d timing=%d\n", (void *)*out, active, d->timing_past != NULL);
    pthread_mutex_lock(&g_map_mtx);
    d->next = g_devs;
    g_devs = d;
    pthread_mutex_unlock(&g_map_mtx);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL lt_DestroyDevice(VkDevice device, const VkAllocationCallbacks *alloc)
{
    if (!device)
        return;
    pthread_mutex_lock(&g_map_mtx);
    ldev_t *d = NULL;
    for (ldev_t **pp = &g_devs; *pp; pp = &(*pp)->next)
        if ((*pp)->key == KEY(device)) {
            d = *pp;
            *pp = d->next;
            break;
        }
    pthread_mutex_unlock(&g_map_mtx);
    if (!d)
        return;
    if (d->has_thread) {
        pthread_mutex_lock(&d->mtx);
        d->quit = true;
        pthread_cond_broadcast(&d->cv);
        pthread_mutex_unlock(&d->mtx);
        pthread_join(d->thread, NULL);
    }
    // The app made the device idle before destroying it, so our objects are free now.
    pthread_mutex_lock(&d->mtx);
    while (d->scs) {
        sc_t *s = d->scs;
        d->scs = s->next;
        s->dead = true;
        destroy_sc_data(d, s);
        free(s);
    }
    pthread_mutex_unlock(&d->mtx);
    PFN_vkDestroyDevice destroy = d->destroy;
    // Not freed (see find_dev): just make sure no lookup can match it again.
    atomic_store(&d->active, false);
    d->key = NULL;
    log_flush();
    destroy(device, alloc);
}

// ---------------------------------------------------------------- instance
static VKAPI_ATTR VkResult VKAPI_CALL lt_CreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *alloc,
                                                         VkInstance *out)
{
    VkLayerInstanceCreateInfo *link = (VkLayerInstanceCreateInfo *)ci->pNext;
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && link->function == VK_LAYER_LINK_INFO))
        link = (VkLayerInstanceCreateInfo *)link->pNext;
    if (!link)
        return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    PFN_vkCreateInstance next = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
    read_options();
    // VK_EXT_present_timing builds on VK_KHR_get_surface_capabilities2; add it if the app
    // didn't (every Linux loader has it). If that fails, create the instance as asked.
    VkInstanceCreateInfo mod = *ci;
    const char **exts = NULL;
    bool added = false;
    if (!getenv("DISABLE_LAGTRACE") &&
        !has_ext(ci->ppEnabledExtensionNames, ci->enabledExtensionCount, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME) &&
        has_ext(ci->ppEnabledExtensionNames, ci->enabledExtensionCount, VK_KHR_SURFACE_EXTENSION_NAME) &&
        (exts = malloc((ci->enabledExtensionCount + 1) * sizeof *exts))) {
        memcpy(exts, ci->ppEnabledExtensionNames, ci->enabledExtensionCount * sizeof *exts);
        exts[ci->enabledExtensionCount] = VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME;
        mod.enabledExtensionCount = ci->enabledExtensionCount + 1;
        mod.ppEnabledExtensionNames = exts;
        added = true;
    }
    VkResult r = next(added ? &mod : ci, alloc, out);
    if (added && r == VK_ERROR_EXTENSION_NOT_PRESENT)
        r = next(ci, alloc, out);
    free(exts);
    if (r != VK_SUCCESS)
        return r;
    inst_t *i = calloc(1, sizeof *i);
    if (!i)
        return r; // vkCreateDevice then runs untouched
    i->key = KEY(*out);
    i->inst = *out;
    i->gipa = gipa;
#define IGET(field, name) i->field = (void *)gipa(*out, name)
    IGET(destroy, "vkDestroyInstance");
    IGET(enum_ext, "vkEnumerateDeviceExtensionProperties");
    IGET(feat2, "vkGetPhysicalDeviceFeatures2");
    if (!i->feat2)
        IGET(feat2, "vkGetPhysicalDeviceFeatures2KHR");
    IGET(props, "vkGetPhysicalDeviceProperties");
    IGET(props2, "vkGetPhysicalDeviceProperties2");
    IGET(memprops, "vkGetPhysicalDeviceMemoryProperties");
    IGET(qfprops, "vkGetPhysicalDeviceQueueFamilyProperties");
    IGET(surf_caps, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
#undef IGET
    pthread_mutex_lock(&g_map_mtx);
    i->next = g_insts;
    g_insts = i;
    pthread_mutex_unlock(&g_map_mtx);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL lt_DestroyInstance(VkInstance instance, const VkAllocationCallbacks *alloc)
{
    if (!instance)
        return;
    pthread_mutex_lock(&g_map_mtx);
    inst_t *i = NULL;
    for (inst_t **pp = &g_insts; *pp; pp = &(*pp)->next)
        if ((*pp)->key == KEY(instance)) {
            i = *pp;
            *pp = i->next;
            break;
        }
    pthread_mutex_unlock(&g_map_mtx);
    if (!i)
        return;
    PFN_vkDestroyInstance destroy = i->destroy;
    free(i); // device records keep a pointer, but devices die before their instance
    destroy(instance, alloc);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL lt_GetDeviceProcAddr(VkDevice device, const char *name);

static PFN_vkVoidFunction device_hook(const char *name, bool active)
{
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)lt_GetDeviceProcAddr;
    if (!strcmp(name, "vkDestroyDevice")) return (PFN_vkVoidFunction)lt_DestroyDevice;
    if (!active)
        return NULL;
    if (!strcmp(name, "vkQueuePresentKHR")) return (PFN_vkVoidFunction)lt_QueuePresentKHR;
    if (!strcmp(name, "vkCreateSwapchainKHR")) return (PFN_vkVoidFunction)lt_CreateSwapchainKHR;
    if (!strcmp(name, "vkDestroySwapchainKHR")) return (PFN_vkVoidFunction)lt_DestroySwapchainKHR;
    if (!strcmp(name, "vkGetDeviceQueue")) return (PFN_vkVoidFunction)lt_GetDeviceQueue;
    if (!strcmp(name, "vkGetDeviceQueue2")) return (PFN_vkVoidFunction)lt_GetDeviceQueue2;
    return NULL;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL lt_GetDeviceProcAddr(VkDevice device, const char *name)
{
    ldev_t *d = find_dev(KEY(device));
    if (!d)
        return NULL;
    // An inactive device gets the next layer's functions directly: zero overhead.
    PFN_vkVoidFunction f = device_hook(name, atomic_load(&d->active));
    if (f && !strcmp(name, "vkGetDeviceQueue2") && !d->get_queue2)
        return NULL;
    return f ? f : d->gdpa(device, name);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL lt_GetInstanceProcAddr(VkInstance instance, const char *name)
{
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)lt_GetInstanceProcAddr;
    if (!strcmp(name, "vkCreateInstance")) return (PFN_vkVoidFunction)lt_CreateInstance;
    if (!strcmp(name, "vkDestroyInstance")) return (PFN_vkVoidFunction)lt_DestroyInstance;
    if (!strcmp(name, "vkCreateDevice")) return (PFN_vkVoidFunction)lt_CreateDevice;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)lt_GetDeviceProcAddr;
    if (!strcmp(name, "vkDestroyDevice")) return (PFN_vkVoidFunction)lt_DestroyDevice;
    if (!instance)
        return NULL;
    inst_t *i = find_inst(KEY(instance));
    return i ? i->gipa(instance, name) : NULL;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *v)
{
    if (!v || v->loaderLayerInterfaceVersion < 2)
        return VK_ERROR_INITIALIZATION_FAILED;
    v->loaderLayerInterfaceVersion = 2;
    v->pfnGetInstanceProcAddr = lt_GetInstanceProcAddr;
    v->pfnGetDeviceProcAddr = lt_GetDeviceProcAddr;
    v->pfnGetPhysicalDeviceProcAddr = NULL;
    return VK_SUCCESS;
}
