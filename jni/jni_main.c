#include <android/log.h>
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
    X(vkMapMemory)

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

void android_main(struct android_app *app) {
    wait_window(app);
    if (app->destroyRequested) return;
    LOG("window ready");

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
    const char *dexts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
        .enabledExtensionCount = 1, .ppEnabledExtensionNames = dexts };
    VkDevice dev = VK_NULL_HANDLE;
    r = p_vkCreateDevice(pd, &dci, NULL, &dev);
    LOG("vkCreateDevice=%d queueFamily=%u", r, qf);
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
    VkSemaphore acq = VK_NULL_HANDLE, rend = VK_NULL_HANDLE;
    p_vkCreateSemaphore(dev, &semci, NULL, &acq);
    p_vkCreateSemaphore(dev, &semci, NULL, &rend);

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

    VkResult last_pres = VK_SUCCESS;
    uint32_t f = 0;
    while (!app->destroyRequested) {
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
        p_vkCmdCopyBufferToImage(cb, buf, imgs[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &reg);
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
