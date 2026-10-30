#include <android/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stddef.h>
static FILE *probe_out;
static uint32_t probe_vals[8];
static uint32_t dev_rc = 0xFFFF, swap_rc = 0xFFFF;
static void PROBE_LOG(const char *fmt, ...) {
    if (!probe_out) return;
    va_list ap; va_start(ap, fmt); vfprintf(probe_out, fmt, ap); va_end(ap);
    fflush(probe_out);
}
#include <android/native_window.h>
#include <android_native_app_glue.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#define TAG "wsitest"
#define LOG(...)  __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define INSTANCE_FNS(X) \
    X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkCreateDevice) \
    X(vkCreateAndroidSurfaceKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR)

#define DEVICE_FNS(X) \
    X(vkGetDeviceQueue) X(vkCreateSwapchainKHR) X(vkGetSwapchainImagesKHR) \
    X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) X(vkCreateCommandPool) \
    X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) \
    X(vkResetCommandBuffer) X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage) \
    X(vkQueueSubmit) X(vkQueueWaitIdle) X(vkCreateSemaphore) X(vkCreateBuffer) \
    X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkBindBufferMemory) \
    X(vkMapMemory) \
    X(vkCreateImage) X(vkBindImageMemory2) X(vkCmdClearColorImage) \
    X(vkGetSemaphoreFdKHR) X(vkGetAndroidHardwareBufferPropertiesANDROID) \
    X(vkCreateFence) X(vkWaitForFences)

#define DECLARE(n) static PFN_##n p_##n;
#define LOAD(n) p_##n = (PFN_##n)gipa(inst, #n);
INSTANCE_FNS(DECLARE)
DEVICE_FNS(DECLARE)

static PFN_vkGetInstanceProcAddr gipa;
static int g_bgra;

static void poll_events(struct android_app *app, int timeout_ms) {
    int events = 0;
    struct android_poll_source *src = NULL;
    if (ALooper_pollOnce(timeout_ms, NULL, &events, (void **)&src) >= 0 && src)
        src->process(app, src);
}

static void wait_window(struct android_app *app) {
    while (!app->window && !app->destroyRequested)
        poll_events(app, -1);
}

static uint32_t find_mem(const VkPhysicalDeviceMemoryProperties *mp, uint32_t bits) {
    VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp->memoryTypes[i].propertyFlags & want) == want)
            return i;
    return 0;
}

static uint32_t col(uint32_t r, uint32_t g, uint32_t b) {
    uint32_t c = g_bgra ? ((r << 16) | (g << 8) | b) : ((b << 16) | (g << 8) | r);
    return 0xFF000000u | c;
}

static void put(uint32_t *px, uint32_t w, uint32_t h, int x, int y, uint32_t c) {
    if (x < 0 || y < 0 || x >= (int)w || y >= (int)h) return;
    px[(uint32_t)y * w + (uint32_t)x] = c;
}

/* One row of 16 px cells, LSB on the left. White = 1, black = 0. */
static void draw_bits(uint32_t *px, uint32_t w, uint32_t h, int y0, uint32_t value, int nbits) {
    const int cell = 28, x0 = 24;
    for (int i = 0; i < nbits; i++) {
        uint32_t c = ((value >> i) & 1u) ? col(255, 255, 255) : col(0, 0, 0);
        for (int yy = 0; yy < cell; yy++)
            for (int xx = 0; xx < cell; xx++)
                put(px, w, h, x0 + i * cell + xx, y0 + yy, c);
    }
}

static long long edge(long long ax, long long ay, long long bx, long long by,
                      long long qx, long long qy) {
    return (bx - ax) * (qy - ay) - (by - ay) * (qx - ax);
}

static volatile int probe_connect_rc = 9999, probe_deq_rc = 9999, probe_w = 0, probe_fence = -9, probe_ww = 0, probe_stride = 0;
typedef int (*pfn_perform)(void *win, int op, ...);
typedef int (*pfn_dequeue)(void *win, void **buf, int *fence);
typedef int (*pfn_cancel)(void *win, void *buf, int fence);

static void draw_frame(uint32_t *px, uint32_t w, uint32_t h, uint32_t idx, uint32_t f,
                       int32_t acq, int32_t pres) {
    /* Background: checkerboard over a gradient, tinted per swapchain index. */
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint32_t r = x * 255u / w;
            uint32_t g = y * 255u / h;
            uint32_t b = (idx * 60u) & 255u;
            if (((x >> 6) + (y >> 6)) & 1u)
                r = 255u - r;
            px[y * w + x] = col(r, g, b);
        }

    /* Moving triangle: slides left to right and back, period 240 frames. */
    long long s = w / 5;
    uint32_t p = f % 240u;
    long long pos = p < 120u ? (long long)p : (long long)(240u - p);
    long long xc = s + pos * ((long long)w - 2 * s) / 120;
    long long cy = h / 2;
    long long ax = xc, ay = cy - s;
    long long bx = xc - s, by = cy + s;
    long long cx = xc + s, cyy = cy + s;
    uint32_t tc = col(255, (idx * 70u) & 255u, 255u - ((idx * 70u) & 255u));

    long long minx = bx < 0 ? 0 : bx, maxx = cx >= (long long)w ? (long long)w - 1 : cx;
    long long miny = ay < 0 ? 0 : ay, maxy = cyy >= (long long)h ? (long long)h - 1 : cyy;
    for (long long y = miny; y <= maxy; y++)
        for (long long x = minx; x <= maxx; x++) {
            long long e0 = edge(ax, ay, bx, by, x, y);
            long long e1 = edge(bx, by, cx, cyy, x, y);
            long long e2 = edge(cx, cyy, ax, ay, x, y);
            if ((e0 >= 0 && e1 >= 0 && e2 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0))
                put(px, w, h, (int)x, (int)y, tc);
        }

    /* Status readout rows at the top of the frame. */
    draw_bits(px, w, h, 160, f, 16);
    draw_bits(px, w, h, 200, (uint32_t)acq & 0xFFFFu, 16);
    draw_bits(px, w, h, 240, (uint32_t)pres & 0xFFFFu, 16);
    draw_bits(px, w, h, 280, idx, 8);
    draw_bits(px, w, h, 320, dev_rc, 16);
    draw_bits(px, w, h, 360, swap_rc, 16);
    draw_bits(px, w, h, 400, (uint32_t)(probe_connect_rc & 0xFFFF), 16);
    draw_bits(px, w, h, 440, (uint32_t)(probe_deq_rc & 0xFFFF), 16);
    draw_bits(px, w, h, 480, (uint32_t)probe_w, 16);
    draw_bits(px, w, h, 520, (uint32_t)probe_ww, 16);
    draw_bits(px, w, h, 560, (uint32_t)probe_stride, 16);
    for (int pi = 0; pi < 8; pi++)
        draw_bits(px, w, h, (int)h - 460 + pi * 40, probe_vals[pi], 16);
}

static void window_dequeue_probe(ANativeWindow *win) {
    void *lib = dlopen("libnativewindow.so", RTLD_NOW);
    if (!lib) { probe_connect_rc = -1000; return; }
    pfn_dequeue deq = (pfn_dequeue)dlsym(lib, "ANativeWindow_dequeueBuffer");
    pfn_cancel can = (pfn_cancel)dlsym(lib, "ANativeWindow_cancelBuffer");
    if (!deq || !can) { probe_connect_rc = -1001; return; }
    /* perform hook at byte 152 in struct ANativeWindow (Mesa stub matches the device) */
    pfn_perform perform = *(pfn_perform *)((char *)win + 152);
    probe_connect_rc = perform(win, 13 /* CONNECT */, 2 /* CPU */);
    if (probe_connect_rc != 0) return;
    probe_ww = ANativeWindow_getWidth(win);
    void *buf = NULL; int fence = -1;
    probe_deq_rc = deq(win, &buf, &fence);
    if (probe_deq_rc == 0 && buf) {
        probe_w = *(int *)((char *)buf + 56);
        probe_stride = *(int *)((char *)buf + 64);
        probe_fence = fence;
        can(win, buf, fence);
    }
    perform(win, 14 /* DISCONNECT */, 2);
}

static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to,
                    VkPipelineStageFlags src, VkPipelineStageFlags dst,
                    VkAccessFlags sa, VkAccessFlags da) {
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = sa, .dstAccessMask = da,
        .oldLayout = from, .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    p_vkCmdPipelineBarrier(cb, src, dst, 0, 0, NULL, 0, NULL, 1, &b);
}

struct android_native_base_t {
    int magic; int version; void *reserved[4];
    void (*incRef)(struct android_native_base_t *);
    void (*decRef)(struct android_native_base_t *);
};
typedef struct ANativeWindowBuffer {
    struct android_native_base_t common;
    int width, height, stride, format, usage;
    void *reserved[2];
    const void *handle;
    void *reserved_proc[8];
} ANativeWindowBuffer;
typedef int (*dq_t)(ANativeWindow *, ANativeWindowBuffer **, int *);
typedef int (*cancel_t)(ANativeWindow *, ANativeWindowBuffer *, int);
static void probe(ANativeWindow *win) {
    void *h = dlopen("libnativewindow.so", RTLD_NOW);
    dq_t dq = h ? (dq_t)dlsym(h, "ANativeWindow_dequeueBuffer") : NULL;
    cancel_t cancel = h ? (cancel_t)dlsym(h, "ANativeWindow_cancelBuffer") : NULL;
    ANativeWindowBuffer *buf = NULL;
    int fence = -1;
    int rc = dq ? dq(win, &buf, &fence) : -999;
    probe_vals[0] = (uint32_t)(rc & 0xFFFF);
    probe_vals[1] = (uint32_t)ANativeWindow_getWidth(win);
    probe_vals[2] = (uint32_t)ANativeWindow_getHeight(win);
    probe_vals[3] = buf ? (uint32_t)buf->width : 0;
    probe_vals[4] = buf ? (uint32_t)buf->height : 0;
    probe_vals[5] = buf ? (uint32_t)buf->stride : 0;
    probe_vals[6] = (uint32_t)__builtin_offsetof(ANativeWindowBuffer, handle);
    probe_vals[7] = 1; /* probe ran */
    if (rc == 0 && buf && cancel)
        cancel(win, buf, fence);
}

/* Declared directly: the NDK 27 header uses C++-only overloads that a C build can't parse.
 * Signatures match android/surface_control.h (API 29+), linked from libandroid. */
typedef struct ASurfaceControl ASurfaceControl;
typedef struct ASurfaceTransaction ASurfaceTransaction;
ASurfaceControl *ASurfaceControl_createFromWindow(ANativeWindow *parent, const char *debug_name);
void ASurfaceControl_release(ASurfaceControl *surface_control);
ASurfaceTransaction *ASurfaceTransaction_create(void);
void ASurfaceTransaction_delete(ASurfaceTransaction *transaction);
void ASurfaceTransaction_apply(ASurfaceTransaction *transaction);
void ASurfaceTransaction_setBuffer(ASurfaceTransaction *transaction, ASurfaceControl *surface_control,
                                   AHardwareBuffer *buffer, int acquire_fence_fd);
typedef void (*panvk_txn_complete_t)(void *ctx, void *stats);
void ASurfaceTransaction_setOnComplete(ASurfaceTransaction *transaction, void *context,
                                       panvk_txn_complete_t func);
#include <android/hardware_buffer.h>
#include <android/hardware_buffer_jni.h>
static void surface_probe(ANativeWindow *win, uint32_t w, uint32_t h) {
    ASurfaceControl *ctl = ASurfaceControl_createFromWindow(win, "wsitest-probe");
    probe_vals[7] = ctl ? 1 : 0;
    if (!ctl) return;
    AHardwareBuffer *ahb = NULL;
    AHardwareBuffer_Desc desc = {
        .width = w, .height = h, .layers = 1,
        .format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
        .usage = AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
                 AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                 AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN,
    };
    int arc = AHardwareBuffer_allocate(&desc, &ahb);
    probe_vals[0] = (uint32_t)(arc & 0xFFFF);
    if (arc == 0 && ahb) {
        void *data = NULL;
        if (AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, NULL, &data) == 0 && data) {
            uint32_t *px = (uint32_t *)data;
            for (uint32_t y = 0; y < h; y++)
                for (uint32_t x = 0; x < w; x++)
                    px[y * (desc.stride ? desc.stride : w) + x] = 0xFFFF00FFu;
            AHardwareBuffer_unlock(ahb, NULL);
        }
        ASurfaceTransaction *txn = ASurfaceTransaction_create();
        ASurfaceTransaction_setBuffer(txn, ctl, ahb, -1);
        ASurfaceTransaction_apply(txn);
        ASurfaceTransaction_delete(txn);
        probe_vals[6] = 96;
    }
    ASurfaceControl_release(ctl);
}

#include <time.h>
#include <unistd.h>
#include <stdint.h>
static volatile int pl_busy[3];
static volatile int pl_done;
static void pl_on_complete(void *ctx, void *stats) {
    (void)stats;
    int slot = (int)(intptr_t)ctx;
    __atomic_store_n(&pl_busy[slot], 0, __ATOMIC_SEQ_CST);
    __atomic_add_fetch(&pl_done, 1, __ATOMIC_SEQ_CST);
}
static uint64_t pl_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static void paint_row(uint32_t *px, uint32_t stride, uint32_t h, int y0, uint32_t value) {
    const int x0 = 40, cell = 20, rowh = 24;
    for (int y = y0; y < y0 + rowh && (uint32_t)y < h; y++)
        for (int x = x0; x < x0 + 16 * cell; x++)
            px[(uint32_t)y * stride + (uint32_t)x] = 0xFF000000u;
    for (int i = 0; i < 16; i++) {
        if (!((value >> i) & 1)) continue;
        for (int y = y0 + 4; y < y0 + rowh - 4 && (uint32_t)y < h; y++)
            for (int x = x0 + i * cell + 4; x < x0 + i * cell + cell - 4; x++)
                px[(uint32_t)y * stride + (uint32_t)x] = 0xFFFFFFFFu;
    }
}
static void present_loop(ANativeWindow *win) {
    const int N = 600;
    uint32_t w = (uint32_t)ANativeWindow_getWidth(win);
    uint32_t h = (uint32_t)ANativeWindow_getHeight(win);
    ASurfaceControl *ctl = ASurfaceControl_createFromWindow(win, "wsitest-present");
    if (!ctl) { probe_vals[4] = 0xDEAD; return; }
    AHardwareBuffer *ring[3] = {0, 0, 0};
    AHardwareBuffer_Desc d = {
        .width = w, .height = h, .layers = 1,
        .format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
        .usage = AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
                 AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                 AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN,
    };
    for (int i = 0; i < 3; i++) {
        if (AHardwareBuffer_allocate(&d, &ring[i]) != 0 || !ring[i]) {
            probe_vals[4] = 0xE000 | i;
            ASurfaceControl_release(ctl);
            return;
        }
    }
    uint64_t apply_ns = 0;
    int frames = 0, timeouts = 0;
    pl_done = 0;
    for (int f = 0; f < N; f++) {
        int slot = f % 3;
        int waited = 0;
        while (__atomic_load_n(&pl_busy[slot], __ATOMIC_SEQ_CST) && waited < 3000) {
            usleep(1000);
            waited++;
        }
        if (waited >= 3000) timeouts++;
        AHardwareBuffer_Desc dd;
        AHardwareBuffer_describe(ring[slot], &dd);
        uint32_t stride = dd.stride ? dd.stride : w;
        void *data = NULL;
        if (AHardwareBuffer_lock(ring[slot], AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, NULL, &data) == 0 && data) {
            uint32_t *px = (uint32_t *)data;
            uint32_t c0 = (f & 1) ? 0xFF00FFFFu : 0xFFFF00FFu;
            uint32_t c1 = (f & 1) ? 0xFFFF00FFu : 0xFF00FFFFu;
            for (uint32_t y = 0; y < h; y++)
                for (uint32_t x = 0; x < w; x++)
                    px[y * stride + x] = (((x >> 6) + (y >> 6)) & 1) ? c0 : c1;
            AHardwareBuffer_unlock(ring[slot], NULL);
        }
        __atomic_store_n(&pl_busy[slot], 1, __ATOMIC_SEQ_CST);
        ASurfaceTransaction *txn = ASurfaceTransaction_create();
        ASurfaceTransaction_setOnComplete(txn, (void *)(intptr_t)slot, pl_on_complete);
        ASurfaceTransaction_setBuffer(txn, ctl, ring[slot], -1);
        uint64_t t0 = pl_now_ns();
        ASurfaceTransaction_apply(txn);
        apply_ns += pl_now_ns() - t0;
        ASurfaceTransaction_delete(txn);
        frames++;
    }
    for (int i = 0; i < 3000 && pl_done < frames; i++) usleep(1000);
    probe_vals[4] = (uint32_t)((apply_ns / (uint64_t)(frames ? frames : 1)) / 1000u);
    probe_vals[5] = (uint32_t)pl_done;
    probe_vals[6] = (uint32_t)frames;
    probe_vals[7] = (uint32_t)timeouts;
    /* Hold the last frame with the readout painted on it, for a photo. */
    {
        int slot = (N - 1) % 3;
        int waited = 0;
        while (__atomic_load_n(&pl_busy[slot], __ATOMIC_SEQ_CST) && waited < 3000) {
            usleep(1000);
            waited++;
        }
        AHardwareBuffer_Desc dd;
        AHardwareBuffer_describe(ring[slot], &dd);
        uint32_t stride = dd.stride ? dd.stride : w;
        void *data = NULL;
        if (AHardwareBuffer_lock(ring[slot], AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, NULL, &data) == 0 && data) {
            uint32_t *px = (uint32_t *)data;
            for (int pi = 0; pi < 4; pi++)
                paint_row(px, stride, h, (int)h - 520 + pi * 40, probe_vals[4 + pi]);
            AHardwareBuffer_unlock(ring[slot], NULL);
        }
        __atomic_store_n(&pl_busy[slot], 1, __ATOMIC_SEQ_CST);
        ASurfaceTransaction *txn = ASurfaceTransaction_create();
        ASurfaceTransaction_setBuffer(txn, ctl, ring[slot], -1);
        ASurfaceTransaction_apply(txn);
        ASurfaceTransaction_delete(txn);
    }
    for (int i = 0; i < 10; i++) usleep(1000000); /* hold 10 s */
    for (int i = 0; i < 3; i++) AHardwareBuffer_release(ring[i]);
    ASurfaceControl_release(ctl);
}

#define ZC_N 3
static volatile int zc_free[ZC_N];
static volatile uint32_t zc_released, zc_skipped, zc_submitted;
static void zc_on_release(void *ctx, void *stats) {
    (void)stats;
    __atomic_store_n(&zc_free[(intptr_t)ctx], 1, __ATOMIC_SEQ_CST);
    __atomic_add_fetch(&zc_released, 1, __ATOMIC_SEQ_CST);
}
static void zero_copy_loop(struct android_app *app, VkDevice dev, VkQueue q, VkCommandBuffer cb,
                           VkPhysicalDeviceMemoryProperties *mp, uint32_t w, uint32_t h) {
    ASurfaceControl *ctl = ASurfaceControl_createFromWindow(app->window, "wsitest-zc");
    if (!ctl) { LOGE("zc: surface control failed"); return; }
    AHardwareBuffer *ahb[ZC_N] = {0};
    VkImage img[ZC_N] = {0};
    VkDeviceMemory mem[ZC_N] = {0};
    VkSemaphore sem[ZC_N] = {0};
    AHardwareBuffer_Desc d = {
        .width = w, .height = h, .layers = 1,
        .format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
        .usage = AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE,
    };
    for (int i = 0; i < ZC_N; i++) {
        zc_free[i] = 1;
        if (AHardwareBuffer_allocate(&d, &ahb[i]) != 0 || !ahb[i]) { LOGE("zc: AHB alloc %d failed", i); return; }
        VkAndroidHardwareBufferPropertiesANDROID props = {
            .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID };
        VkResult r = p_vkGetAndroidHardwareBufferPropertiesANDROID(dev, ahb[i], &props);
        if (r != VK_SUCCESS) { LOGE("zc: AHB properties %d failed: %d", i, r); return; }
        VkExternalMemoryImageCreateInfo emici = {
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID };
        VkImageCreateInfo ici = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &emici,
            .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
            .extent = { w, h, 1 }, .mipLevels = 1, .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
        r = p_vkCreateImage(dev, &ici, NULL, &img[i]);
        if (r != VK_SUCCESS) { LOGE("zc: vkCreateImage %d failed: %d", i, r); return; }
        VkImportAndroidHardwareBufferInfoANDROID imp = {
            .sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
            .buffer = ahb[i] };
        VkMemoryDedicatedAllocateInfo dai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
            .pNext = &imp, .image = img[i] };
        VkMemoryAllocateInfo mai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &dai,
            .allocationSize = props.allocationSize,
            .memoryTypeIndex = find_mem(mp, props.memoryTypeBits) };
        r = p_vkAllocateMemory(dev, &mai, NULL, &mem[i]);
        if (r != VK_SUCCESS) { LOGE("zc: import memory %d failed: %d", i, r); return; }
        VkBindImageMemoryInfo bmi = {
            .sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO,
            .image = img[i], .memory = mem[i], .memoryOffset = 0 };
        r = p_vkBindImageMemory2(dev, 1, &bmi);
        if (r != VK_SUCCESS) { LOGE("zc: bind %d failed: %d", i, r); return; }
        VkExportSemaphoreCreateInfo esci = {
            .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
            .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT };
        VkSemaphoreCreateInfo sci = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &esci };
        r = p_vkCreateSemaphore(dev, &sci, NULL, &sem[i]);
        if (r != VK_SUCCESS) { LOGE("zc: semaphore %d failed: %d", i, r); return; }
    }
    LOG("zc: ring of %d AHB images ready (%ux%u)", ZC_N, w, h);

    for (uint32_t f = 0; f < 100000 && !app->destroyRequested; f++) {
        poll_events(app, 0);
        int slot = (int)(f % ZC_N);
        if (!__atomic_load_n(&zc_free[slot], __ATOMIC_SEQ_CST)) {
            __atomic_add_fetch(&zc_skipped, 1, __ATOMIC_SEQ_CST);
            usleep(1000);
            continue;
        }
        __atomic_store_n(&zc_free[slot], 0, __ATOMIC_SEQ_CST);
        __atomic_add_fetch(&zc_submitted, 1, __ATOMIC_SEQ_CST);

        p_vkResetCommandBuffer(cb, 0);
        VkCommandBufferBeginInfo bi = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        p_vkBeginCommandBuffer(cb, &bi);
        barrier(cb, img[slot], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkClearColorValue clr = { .float32 = { ((f / 120) & 1) ? 1.0f : 0.0f, 0.5f,
                                               ((f / 120) & 1) ? 0.0f : 1.0f, 1.0f } };
        VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        p_vkCmdClearColorImage(cb, img[slot], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clr, 1, &rng);
        barrier(cb, img[slot], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT, 0);
        p_vkEndCommandBuffer(cb);

        VkSubmitInfo si = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1, .pCommandBuffers = &cb,
            .signalSemaphoreCount = 1, .pSignalSemaphores = &sem[slot] };
        p_vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE);

        VkSemaphoreGetFdInfoKHR gi = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
            .semaphore = sem[slot], .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT };
        int fence_fd = -1;
        VkResult fr = p_vkGetSemaphoreFdKHR(dev, &gi, &fence_fd);
        if (fr != VK_SUCCESS) { LOGE("zc: sync fd export failed: %d", fr); fence_fd = -1; }

        ASurfaceTransaction *txn = ASurfaceTransaction_create();
        ASurfaceTransaction_setOnComplete(txn, (void *)(intptr_t)slot, zc_on_release);
        ASurfaceTransaction_setBuffer(txn, ctl, ahb[slot], fence_fd);
        ASurfaceTransaction_apply(txn);
        ASurfaceTransaction_delete(txn);

        p_vkQueueWaitIdle(q);
        if (f % 120 == 0)
            LOG("zc frame %u slot %d fence=%d submitted=%u released=%u skipped=%u", f, slot, fence_fd,
                zc_submitted, zc_released, zc_skipped);
    }

    ASurfaceControl_release(ctl);
    for (int i = 0; i < ZC_N; i++) {
        AHardwareBuffer_release(ahb[i]);
    }
}

void android_main(struct android_app *app) {
    while (!app->window && !app->destroyRequested) { poll_events(app, 0); usleep(10000); }
    window_dequeue_probe(app->window);
    wait_window(app);
    if (app->destroyRequested) return;
    LOG("window ready");
    {
        char path[512];
        snprintf(path, sizeof path, "%s/window_probe.txt", app->activity->externalDataPath);
        probe_out = fopen(path, "w");
    }
    ANativeWindow_setBuffersGeometry(app->window, 1084, 715, WINDOW_FORMAT_RGBA_8888);
    probe(app->window);
    /* disabled while checking the Vulkan path: surface_probe(app->window, 1084, 715); */
    /* disabled while checking the Vulkan path: present_loop(app->window); */

    void *drv = dlopen("libvulkan_panfrost.so", RTLD_NOW | RTLD_LOCAL);
    if (!drv) { LOGE("dlopen: %s", dlerror()); return; }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(drv, "vk_icdGetInstanceProcAddr");
    if (!gipa) { LOGE("no vk_icdGetInstanceProcAddr"); return; }
    LOG("driver loaded");

    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "wsitest", .apiVersion = VK_API_VERSION_1_1 };
    const char *iexts[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &ai, .enabledExtensionCount = 2, .ppEnabledExtensionNames = iexts };
    VkInstance inst = VK_NULL_HANDLE;
    VkResult r = ((PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance"))(&ici, NULL, &inst);
    LOG("vkCreateInstance=%d", r);
    if (r != VK_SUCCESS) return;
    INSTANCE_FNS(LOAD)

    uint32_t npd = 1;
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    p_vkEnumeratePhysicalDevices(inst, &npd, &pd);
    VkPhysicalDeviceMemoryProperties mp;
    p_vkGetPhysicalDeviceMemoryProperties(pd, &mp);

    uint32_t qc = 0;
    p_vkGetPhysicalDeviceQueueFamilyProperties(pd, &qc, NULL);
    if (qc > 16) qc = 16;
    VkQueueFamilyProperties qp[16];
    p_vkGetPhysicalDeviceQueueFamilyProperties(pd, &qc, qp);
    uint32_t qf = 0;
    for (uint32_t i = 0; i < qc; i++)
        if (qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qf = i; break; }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = qf, .queueCount = 1, .pQueuePriorities = &prio };
    const char *dexts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                         VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
                         VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
                            "VK_KHR_external_memory",
                            "VK_KHR_external_semaphore",
                            "VK_KHR_dedicated_allocation",
                            "VK_ANDROID_external_memory_android_hardware_buffer" };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
        .enabledExtensionCount = 7, .ppEnabledExtensionNames = dexts };
    VkDevice dev = VK_NULL_HANDLE;
    r = p_vkCreateDevice(pd, &dci, NULL, &dev);
    LOG("vkCreateDevice=%d queueFamily=%u", r, qf);
    dev_rc = (uint32_t)(r & 0xFFFF);
    if (r != VK_SUCCESS) return;
    DEVICE_FNS(LOAD)

    VkQueue q = VK_NULL_HANDLE;
    p_vkGetDeviceQueue(dev, qf, 0, &q);

    VkAndroidSurfaceCreateInfoKHR asci = { .sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
        .window = app->window };
    VkSurfaceKHR surf = VK_NULL_HANDLE;
    r = p_vkCreateAndroidSurfaceKHR(inst, &asci, NULL, &surf);
    LOG("vkCreateAndroidSurfaceKHR=%d", r);
    if (r != VK_SUCCESS) return;

    VkSurfaceCapabilitiesKHR caps;
    p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surf, &caps);
    LOG("caps: minImages=%u maxImages=%u curExtent=%ux%u transform=0x%x",
        caps.minImageCount, caps.maxImageCount, caps.currentExtent.width,
        caps.currentExtent.height, (unsigned)caps.currentTransform);

    uint32_t nf = 0;
    p_vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &nf, NULL);
    if (nf > 32) nf = 32;
    VkSurfaceFormatKHR fm[32];
    p_vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &nf, fm);
    VkSurfaceFormatKHR chosen = fm[0];
    for (uint32_t i = 0; i < nf; i++) {
        LOG("format[%u]=%d colorspace=%d", i, (int)fm[i].format, (int)fm[i].colorSpace);
        if (fm[i].format == VK_FORMAT_B8G8R8A8_UNORM) chosen = fm[i];
    }
    g_bgra = (chosen.format == VK_FORMAT_B8G8R8A8_UNORM);

    uint32_t w = caps.currentExtent.width, h = caps.currentExtent.height;
    if (w == 0xFFFFFFFFu) {
        w = (uint32_t)ANativeWindow_getWidth(app->window);
        h = (uint32_t)ANativeWindow_getHeight(app->window);
    }
    VkSwapchainCreateInfoKHR swi = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = surf, .minImageCount = caps.minImageCount,
        .imageFormat = chosen.format, .imageColorSpace = chosen.colorSpace,
        .imageExtent = { w, h }, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE };
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    r = p_vkCreateSwapchainKHR(dev, &swi, NULL, &sc);
    LOG("vkCreateSwapchainKHR=%d %ux%u format=%d", r, w, h, (int)chosen.format);
    swap_rc = (uint32_t)(r & 0xFFFF);
    if (r != VK_SUCCESS) return;

    uint32_t nimg = 0;
    p_vkGetSwapchainImagesKHR(dev, sc, &nimg, NULL);
    if (nimg > 8) nimg = 8;
    VkImage imgs[8];
    p_vkGetSwapchainImagesKHR(dev, sc, &nimg, imgs);
    LOG("swapchain images=%u", nimg);

    VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = qf };
    VkCommandPool cp = VK_NULL_HANDLE;
    p_vkCreateCommandPool(dev, &cpci, NULL, &cp);
    VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cb = VK_NULL_HANDLE;
    p_vkAllocateCommandBuffers(dev, &cbai, &cb);

    VkSemaphoreCreateInfo semci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkExportSemaphoreCreateInfo esci = { .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT };
    VkSemaphoreCreateInfo semci_exp = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &esci };
    VkSemaphore acq = VK_NULL_HANDLE, rend = VK_NULL_HANDLE;
    p_vkCreateSemaphore(dev, &semci, NULL, &acq);
    p_vkCreateSemaphore(dev, &semci_exp, NULL, &rend);

    VkDeviceSize bsz = (VkDeviceSize)w * h * 4;
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bsz,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkBuffer buf = VK_NULL_HANDLE;
    p_vkCreateBuffer(dev, &bci, NULL, &buf);
    VkMemoryRequirements mr;
    p_vkGetBufferMemoryRequirements(dev, buf, &mr);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size, .memoryTypeIndex = find_mem(&mp, mr.memoryTypeBits) };
    VkDeviceMemory mem = VK_NULL_HANDLE;
    p_vkAllocateMemory(dev, &mai, NULL, &mem);
    p_vkBindBufferMemory(dev, buf, mem, 0);
    void *map = NULL;
    p_vkMapMemory(dev, mem, 0, bsz, 0, &map);
    uint32_t *px = (uint32_t *)map;
    static uint32_t dummy_dev_rows[1];

    const int ZC = 0;
    if (ZC) zero_copy_loop(app, dev, q, cb, &mp, w, h);
    VkResult last_pres = VK_SUCCESS;
    uint32_t f = 0;
    while (!ZC && !app->destroyRequested) {
        poll_events(app, 0);

        uint32_t idx = 0;
        VkResult ar = p_vkAcquireNextImageKHR(dev, sc, UINT64_MAX, acq, VK_NULL_HANDLE, &idx);
        if (ar < 0 && ar != VK_SUBOPTIMAL_KHR) {
            LOGE("frame %u acquire failed: %d", f, ar);
            break;
        }

        draw_frame(px, w, h, idx, f, (int32_t)ar, (int32_t)last_pres);

        VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        p_vkResetCommandBuffer(cb, 0);
        p_vkBeginCommandBuffer(cb, &bi);
        barrier(cb, imgs[idx], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy reg = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageExtent = { w, h, 1 } };
        VkClearColorValue clr = { .float32 = { ((f / 120) & 1) ? 1.0f : 0.0f, 0.5f,
                                              ((f / 120) & 1) ? 0.0f : 1.0f, 1.0f } };
        VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        p_vkCmdClearColorImage(cb, imgs[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clr, 1, &rng);
        {
            /* Copy the readout band from the staging buffer on top of the clear. */
            VkBufferImageCopy band = { .bufferOffset = (VkDeviceSize)320 * w * 4,
                .bufferRowLength = w, .bufferImageHeight = 0,
                .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                .imageOffset = { 0, 320, 0 },
                .imageExtent = { w, 280, 1 } };
            barrier(cb, imgs[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            p_vkCmdCopyBufferToImage(cb, buf, imgs[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &band);
        }
        barrier(cb, imgs[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT, 0);
        p_vkEndCommandBuffer(cb);

        VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = 1, .pWaitSemaphores = &acq, .pWaitDstStageMask = &ws,
            .commandBufferCount = 1, .pCommandBuffers = &cb,
            .signalSemaphoreCount = 1, .pSignalSemaphores = &rend };
        VkResult sr = p_vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE);

        VkResult pres = VK_SUCCESS;
        VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1, .pWaitSemaphores = &rend,
            .swapchainCount = 1, .pSwapchains = &sc, .pImageIndices = &idx, .pResults = &pres };
        VkResult qr = p_vkQueuePresentKHR(q, &pi);
        p_vkQueueWaitIdle(q);

        last_pres = qr;
        if (f % 60 == 0 || (qr < 0 && qr != VK_SUBOPTIMAL_KHR))
            LOG("frame %u idx=%u acquire=%d submit=%d present=%d", f, idx, ar, sr, qr);
        if (qr < 0 && qr != VK_SUBOPTIMAL_KHR) {
            LOGE("frame %u present failed: %d", f, qr);
            break;
        }
        f++;
    }
    LOG("loop ended at frame %u", f);

    while (!app->destroyRequested)
        poll_events(app, 100);
}
