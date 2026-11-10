// lagtrace-bench: the smallest possible "game" for measuring input latency.
//
// The whole window is black, and white while the left mouse button is held. Each frame
// waits for its swapchain image, then reads input, then renders: the same order a
// well-behaved game uses. Because the whole frame flips between black and white,
// lagtrace's probe sees the answer to a click no matter where it looks.
//
//   lagtrace-bench [--windowed] [--present-mode fifo|mailbox|immediate|fifo-relaxed]
//                  [--x11] [--fps-cap N] [--load-ms MS] [--images N] [--evdev NAME] [--no-grab]
//
// Esc or Q quits. `lagtrace bench` starts it for you.
#define _GNU_SOURCE
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include <linux/input.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define FRAMES 2
#define MAX_IMG 16

static void die(const char *what, VkResult r)
{
    fprintf(stderr, "lagtrace-bench: %s failed (%d)\n", what, r);
    exit(1);
}
#define VK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) die(#call, r_); } while (0)

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static volatile sig_atomic_t g_stop;
static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

// ---------------------------------------------------------------- options
static struct {
    bool windowed, x11, grab;
    VkPresentModeKHR mode;
    double fps_cap, load_ms;
    uint32_t images;
    const char *evdev;
} opt = { .grab = true, .mode = VK_PRESENT_MODE_FIFO_KHR };

static const char *mode_name(VkPresentModeKHR m)
{
    switch (m) {
    case VK_PRESENT_MODE_FIFO_KHR: return "fifo";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "fifo-relaxed";
    case VK_PRESENT_MODE_MAILBOX_KHR: return "mailbox";
    case VK_PRESENT_MODE_IMMEDIATE_KHR: return "immediate";
    default: return "other";
    }
}

static void usage(int code)
{
    printf("usage: lagtrace-bench [options]\n"
           "  --windowed              don't go fullscreen\n"
           "  --present-mode MODE     fifo (vsync, default), fifo-relaxed, mailbox, immediate\n"
           "  --x11                   use X11 (XWayland) instead of Wayland\n"
           "  --fps-cap N             limit the frame rate with a sleep, like an in-game cap\n"
           "  --load-ms MS            busy CPU work per frame between reading input and rendering\n"
           "  --images N              swapchain images (default: driver minimum + 1)\n"
           "  --evdev NAME            read the button straight from the input device whose name\n"
           "                          contains NAME (skips the compositor's input path)\n"
           "  --no-grab               don't lock the mouse pointer to the window\n"
           "Esc or Q quits.\n");
    exit(code);
}

static void parse_args(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--windowed")) {
            opt.windowed = true;
        } else if (!strcmp(a, "--x11")) {
            opt.x11 = true;
        } else if (!strcmp(a, "--no-grab")) {
            opt.grab = false;
        } else if (!strcmp(a, "--present-mode") && v) {
            if (!strcmp(v, "fifo")) opt.mode = VK_PRESENT_MODE_FIFO_KHR;
            else if (!strcmp(v, "fifo-relaxed")) opt.mode = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
            else if (!strcmp(v, "mailbox")) opt.mode = VK_PRESENT_MODE_MAILBOX_KHR;
            else if (!strcmp(v, "immediate")) opt.mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
            else usage(2);
            i++;
        } else if (!strcmp(a, "--fps-cap") && v) {
            opt.fps_cap = atof(v);
            i++;
        } else if (!strcmp(a, "--load-ms") && v) {
            opt.load_ms = atof(v);
            i++;
        } else if (!strcmp(a, "--images") && v) {
            opt.images = (uint32_t)atoi(v);
            i++;
        } else if (!strcmp(a, "--evdev") && v) {
            opt.evdev = v;
            i++;
        } else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage(0);
        } else {
            fprintf(stderr, "lagtrace-bench: unknown option %s\n", a);
            usage(2);
        }
    }
}

// ---------------------------------------------------------------- direct input
static int g_evfd = -1;

static void open_evdev(const char *want)
{
    // The device may appear a moment after we start (lagtrace creates it); retry for 5 s.
    for (int attempt = 0; attempt < 50 && g_evfd < 0; attempt++) {
        DIR *d = opendir("/dev/input");
        struct dirent *e;
        while (d && (e = readdir(d)) && g_evfd < 0) {
            if (strncmp(e->d_name, "event", 5))
                continue;
            char path[300], name[256] = "";
            snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
            int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0)
                continue;
            if (ioctl(fd, EVIOCGNAME(sizeof name), name) >= 0 && strstr(name, want))
                g_evfd = fd;
            else
                close(fd);
        }
        if (d)
            closedir(d);
        if (g_evfd < 0)
            usleep(100000);
    }
    if (g_evfd < 0) {
        fprintf(stderr, "lagtrace-bench: no input device named like \"%s\"\n", want);
        exit(1);
    }
}

static void poll_evdev(bool *held)
{
    struct input_event ev[64];
    ssize_t n;
    while ((n = read(g_evfd, ev, sizeof ev)) > 0)
        for (ssize_t i = 0; i < n / (ssize_t)sizeof ev[0]; i++)
            if (ev[i].type == EV_KEY && (ev[i].code == BTN_LEFT || ev[i].code == BTN_SOUTH) && ev[i].value != 2)
                *held = ev[i].value == 1;
}

// ---------------------------------------------------------------- vulkan
static VkInstance inst;
static VkSurfaceKHR surf;
static VkPhysicalDevice pd;
static VkDevice dev;
static VkQueue queue;
static uint32_t qfam;
static VkSwapchainKHR sc;
static VkFormat sc_fmt;
static VkExtent2D sc_ext;
static uint32_t nimg;
static VkImage imgs[MAX_IMG];
static VkSemaphore done_sem[MAX_IMG];
static VkSemaphore acq_sem[FRAMES];
static VkFence fences[FRAMES];
static VkCommandPool pool;
static VkCommandBuffer cbs[FRAMES];
static SDL_Window *win;
static VkPresentModeKHR used_mode;

static void pick_device(void)
{
    uint32_t n = 0;
    VK(vkEnumeratePhysicalDevices(inst, &n, NULL));
    VkPhysicalDevice list[16];
    n = n > 16 ? 16 : n;
    VkResult er = vkEnumeratePhysicalDevices(inst, &n, list);
    if (er != VK_SUCCESS && er != VK_INCOMPLETE)
        die("vkEnumeratePhysicalDevices", er);
    int best = -1, best_score = -1;
    uint32_t best_fam = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t nf = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(list[i], &nf, NULL);
        VkQueueFamilyProperties qf[32];
        nf = nf > 32 ? 32 : nf;
        vkGetPhysicalDeviceQueueFamilyProperties(list[i], &nf, qf);
        for (uint32_t f = 0; f < nf; f++) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(list[i], f, surf, &present);
            if (!(qf[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present)
                continue;
            VkPhysicalDeviceProperties p;
            vkGetPhysicalDeviceProperties(list[i], &p);
            int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1 : 0;
            if (score > best_score) {
                best = (int)i;
                best_score = score;
                best_fam = f;
            }
            break;
        }
    }
    if (best < 0) {
        fprintf(stderr, "lagtrace-bench: no GPU can present to this window\n");
        exit(1);
    }
    pd = list[best];
    qfam = best_fam;
}

static void create_swapchain(void)
{
    VkSwapchainKHR old = sc;
    VkSurfaceCapabilitiesKHR caps;
    VK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surf, &caps));
    uint32_t nf = 0;
    VK(vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &nf, NULL));
    VkSurfaceFormatKHR fmts[256];
    nf = nf > 256 ? 256 : nf;
    VkResult fr = vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &nf, fmts);
    if (fr != VK_SUCCESS && fr != VK_INCOMPLETE)
        die("vkGetPhysicalDeviceSurfaceFormatsKHR", fr);
    VkSurfaceFormatKHR fmt = fmts[0];
    for (uint32_t i = 0; i < nf; i++)
        if ((fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM || fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM) &&
            fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            fmt = fmts[i];
            break;
        }
    uint32_t nm = 0;
    VK(vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surf, &nm, NULL));
    VkPresentModeKHR modes[32];
    nm = nm > 32 ? 32 : nm;
    VkResult mr = vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surf, &nm, modes);
    if (mr != VK_SUCCESS && mr != VK_INCOMPLETE)
        die("vkGetPhysicalDeviceSurfacePresentModesKHR", mr);
    used_mode = VK_PRESENT_MODE_FIFO_KHR;
    for (uint32_t i = 0; i < nm; i++)
        if (modes[i] == opt.mode)
            used_mode = opt.mode;
    if (used_mode != opt.mode && !old)
        fprintf(stderr, "lagtrace-bench: present mode %s not supported here, using fifo\n", mode_name(opt.mode));
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xFFFFFFFFu) {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(win, &w, &h);
        ext.width = (uint32_t)(w > 0 ? w : 1280);
        ext.height = (uint32_t)(h > 0 ? h : 720);
        if (ext.width < caps.minImageExtent.width) ext.width = caps.minImageExtent.width;
        if (ext.height < caps.minImageExtent.height) ext.height = caps.minImageExtent.height;
        if (ext.width > caps.maxImageExtent.width) ext.width = caps.maxImageExtent.width;
        if (ext.height > caps.maxImageExtent.height) ext.height = caps.maxImageExtent.height;
    }
    uint32_t count = opt.images ? opt.images : caps.minImageCount + 1;
    if (count < caps.minImageCount) count = caps.minImageCount;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    if (count > MAX_IMG) count = MAX_IMG;
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha))
        for (uint32_t b = 1; b <= VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR; b <<= 1)
            if (caps.supportedCompositeAlpha & b) {
                alpha = (VkCompositeAlphaFlagBitsKHR)b;
                break;
            }
    VkSwapchainCreateInfoKHR ci = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = surf,
                                    .minImageCount = count, .imageFormat = fmt.format, .imageColorSpace = fmt.colorSpace,
                                    .imageExtent = ext, .imageArrayLayers = 1,
                                    .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                    .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = caps.currentTransform,
                                    .compositeAlpha = alpha, .presentMode = used_mode, .clipped = VK_TRUE, .oldSwapchain = old };
    VK(vkCreateSwapchainKHR(dev, &ci, NULL, &sc));
    if (old)
        vkDestroySwapchainKHR(dev, old, NULL);
    for (uint32_t i = 0; i < nimg; i++)
        vkDestroySemaphore(dev, done_sem[i], NULL);
    nimg = 0;
    VK(vkGetSwapchainImagesKHR(dev, sc, &nimg, NULL));
    if (nimg > MAX_IMG)
        die("too many swapchain images", VK_ERROR_UNKNOWN);
    VK(vkGetSwapchainImagesKHR(dev, sc, &nimg, imgs));
    VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (uint32_t i = 0; i < nimg; i++)
        VK(vkCreateSemaphore(dev, &sci, NULL, &done_sem[i]));
    sc_fmt = fmt.format;
    sc_ext = ext;
}

static void init_vulkan(void)
{
    Uint32 next = 0;
    const char *const *exts = SDL_Vulkan_GetInstanceExtensions(&next);
    if (!exts) {
        fprintf(stderr, "lagtrace-bench: %s\n", SDL_GetError());
        exit(1);
    }
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "lagtrace-bench",
                              .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                                 .enabledExtensionCount = next, .ppEnabledExtensionNames = exts };
    VK(vkCreateInstance(&ici, NULL, &inst));
    if (!SDL_Vulkan_CreateSurface(win, inst, NULL, &surf)) {
        fprintf(stderr, "lagtrace-bench: %s\n", SDL_GetError());
        exit(1);
    }
    pick_device();
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qfam,
                                    .queueCount = 1, .pQueuePriorities = &prio };
    const char *dexts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
                               .enabledExtensionCount = 1, .ppEnabledExtensionNames = dexts };
    VK(vkCreateDevice(pd, &dci, NULL, &dev));
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = qfam };
    VK(vkCreateCommandPool(dev, &pci, NULL, &pool));
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = FRAMES };
    VK(vkAllocateCommandBuffers(dev, &cai, cbs));
    VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
    for (int i = 0; i < FRAMES; i++) {
        VK(vkCreateSemaphore(dev, &sci, NULL, &acq_sem[i]));
        VK(vkCreateFence(dev, &fci, NULL, &fences[i]));
    }
    create_swapchain();
}

static void record(VkCommandBuffer cb, VkImage img, bool white)
{
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VK(vkBeginCommandBuffer(cb, &bi));
    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkImageMemoryBarrier to_dst = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0,
                                    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .image = img, .subresourceRange = range };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_dst);
    float v = white ? 1.0f : 0.0f;
    VkClearColorValue color = { .float32 = { v, v, v, 1.0f } };
    vkCmdClearColorImage(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
    VkImageMemoryBarrier to_present = to_dst;
    to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_present.dstAccessMask = 0;
    to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &to_present);
    VK(vkEndCommandBuffer(cb));
}

static void spin_ms(double ms)
{
    uint64_t end = now_ns() + (uint64_t)(ms * 1e6);
    while (now_ns() < end)
        ;
}

int main(int argc, char **argv)
{
    parse_args(argc, argv);
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    SDL_SetHint(SDL_HINT_APP_ID, "lagtrace-bench");
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, opt.x11 ? "x11" : "wayland,x11");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "lagtrace-bench: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE;
    if (!opt.windowed)
        flags |= SDL_WINDOW_FULLSCREEN;
    win = SDL_CreateWindow("lagtrace bench", 1280, 720, flags);
    if (!win) {
        fprintf(stderr, "lagtrace-bench: SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }
    if (opt.evdev)
        open_evdev(opt.evdev);
    init_vulkan();
    if (opt.grab)
        SDL_SetWindowRelativeMouseMode(win, true); // like a shooter: every click goes to us
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    printf("lagtrace-bench: %s, %ux%u, %s, %u images, %s%s\n", SDL_GetCurrentVideoDriver(), sc_ext.width, sc_ext.height,
           mode_name(used_mode), nimg, props.deviceName, opt.evdev ? ", direct evdev input" : "");
    fflush(stdout);

    bool held = false, recreate = false;
    uint64_t frames = 0, t_start = now_ns(), next_deadline = now_ns();
    int fi = 0;
    while (!g_stop) {
        if (recreate) {
            vkDeviceWaitIdle(dev);
            create_swapchain();
            recreate = false;
        }
        VK(vkWaitForFences(dev, 1, &fences[fi], VK_TRUE, UINT64_MAX));
        uint32_t idx;
        VkResult r = vkAcquireNextImageKHR(dev, sc, UINT64_MAX, acq_sem[fi], VK_NULL_HANDLE, &idx);
        if (r == VK_ERROR_OUT_OF_DATE_KHR) {
            recreate = true;
            continue;
        }
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
            die("vkAcquireNextImageKHR", r);

        // Read input as late as possible, right before rendering.
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT)
                g_stop = 1;
            else if (e.type == SDL_EVENT_KEY_DOWN && (e.key.key == SDLK_ESCAPE || e.key.key == SDLK_Q))
                g_stop = 1;
            else if (!opt.evdev && (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP) &&
                     e.button.button == SDL_BUTTON_LEFT)
                held = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            else if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
                recreate = true;
        }
        if (opt.evdev)
            poll_evdev(&held);
        if (opt.load_ms > 0)
            spin_ms(opt.load_ms);

        VK(vkResetFences(dev, 1, &fences[fi]));
        VK(vkResetCommandBuffer(cbs[fi], 0));
        record(cbs[fi], imgs[idx], held);
        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &acq_sem[fi],
                            .pWaitDstStageMask = &wait_stage, .commandBufferCount = 1, .pCommandBuffers = &cbs[fi],
                            .signalSemaphoreCount = 1, .pSignalSemaphores = &done_sem[idx] };
        VK(vkQueueSubmit(queue, 1, &si, fences[fi]));
        VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
                                .pWaitSemaphores = &done_sem[idx], .swapchainCount = 1, .pSwapchains = &sc, .pImageIndices = &idx };
        r = vkQueuePresentKHR(queue, &pi);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
            recreate = true;
        else if (r != VK_SUCCESS)
            die("vkQueuePresentKHR", r);
        fi = (fi + 1) % FRAMES;
        frames++;
        if (opt.fps_cap > 0) {
            next_deadline += (uint64_t)(1e9 / opt.fps_cap);
            uint64_t now = now_ns();
            if (next_deadline < now)
                next_deadline = now;
            struct timespec ts = { (time_t)(next_deadline / 1000000000ull), (long)(next_deadline % 1000000000ull) };
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
        }
    }
    double secs = (double)(now_ns() - t_start) / 1e9;
    printf("lagtrace-bench: %lu frames in %.1f s (%.0f fps)\n", (unsigned long)frames, secs, secs > 0 ? frames / secs : 0);
    vkDeviceWaitIdle(dev);
    for (int i = 0; i < FRAMES; i++) {
        vkDestroySemaphore(dev, acq_sem[i], NULL);
        vkDestroyFence(dev, fences[i], NULL);
    }
    for (uint32_t i = 0; i < nimg; i++)
        vkDestroySemaphore(dev, done_sem[i], NULL);
    vkDestroyCommandPool(dev, pool, NULL);
    vkDestroySwapchainKHR(dev, sc, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroySurfaceKHR(inst, surf, NULL);
    vkDestroyInstance(inst, NULL);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
