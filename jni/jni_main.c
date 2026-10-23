#include <android/log.h>
#include <stdio.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#define TAG "wsitest"
#define LOG(...)  do { __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__); if (logf) { fprintf(logf, __VA_ARGS__); fputc(10, logf); fflush(logf); } } while (0)
#define LOGE(...) do { __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__); if (logf) { fprintf(logf, __VA_ARGS__); fputc(10, logf); fflush(logf); } } while (0)
static FILE *logf;
#define FRAMES 300

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

static void wait_window(struct android_app *app) {
    while (!app->window && !app->destroyRequested) {
        int events = 0;
        struct android_poll_source *src = NULL;
        if (ALooper_pollOnce(-1, NULL, &events, (void **)&src) >= 0 && src)
            src->process(app, src);
    }
}

static uint32_t find_mem(const VkPhysicalDeviceMemoryProperties *mp, uint32_t bits) {
    VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp->memoryTypes[i].propertyFlags & want) == want)
            return i;
    return 0;
}

/* Checkerboard plus gradient, tinted per swapchain index so each image is distinguishable. */
static void fill(uint32_t *px, uint32_t w, uint32_t h, uint32_t idx, int bgra) {
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint32_t r = x * 255u / w;
            uint32_t g = y * 255u / h;
            uint32_t b = (idx * 60u) & 255u;
            if (((x >> 6) + (y >> 6)) & 1u)
                r = 255u - r;
            uint32_t c = bgra ? ((r << 16) | (g << 8) | b) : ((b << 16) | (g << 8) | r);
            px[y * w + x] = 0xFF000000u | c;
        }
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
    {
        char path[512];
        snprintf(path, sizeof path, "%s/wsitest.log", app->activity->externalDataPath);
        logf = fopen(path, "w");
        if (logf) LOG("log file: %s", path);
    }
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
    LOG("caps: minImages=%u maxImages=%u curExtent=%ux%u transform=0x%x usage=0x%x",
        caps.minImageCount, caps.maxImageCount, caps.currentExtent.width,
        caps.currentExtent.height, (unsigned)caps.currentTransform,
        (unsigned)caps.supportedUsageFlags);

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
    int bgra = (chosen.format == VK_FORMAT_B8G8R8A8_UNORM);

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

    for (int f = 0; f < FRAMES; f++) {
        uint32_t idx = 0;
        r = p_vkAcquireNextImageKHR(dev, sc, UINT64_MAX, acq, VK_NULL_HANDLE, &idx);
        if (r < 0) { LOGE("frame %d acquire failed: %d", f, r); break; }

        fill(px, w, h, idx, bgra);

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

        LOG("frame %d idx=%u acquire=%d submit=%d present=%d perSwapchain=%d", f, idx, r, sr, qr, pres);
        if (qr < 0) break;
    }
    LOG("test finished");

    while (!app->destroyRequested) {
        int events = 0;
        struct android_poll_source *src = NULL;
        if (ALooper_pollOnce(100, NULL, &events, (void **)&src) >= 0 && src)
            src->process(app, src);
    }
}
