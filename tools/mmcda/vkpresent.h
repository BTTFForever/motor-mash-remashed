/* vkpresent.h - Vulkan output for the new renderer ([render] backend=vulkan).
 *
 * Stage 1: the game's 2D frame (its DirectDraw back buffer, 8- or 16-bit) is converted to
 * BGRA, uploaded and blitted to the swapchain, scaled to fit the window with black bars.
 * No shaders yet: copy + vkCmdBlitImage. The 3D renderer will draw into the same frame.
 *
 * vulkan-1.dll (32-bit, from the GPU driver) is loaded at run time; nothing is linked.
 */
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>   /* third_party/vulkan/include on the include path */

#define VKP_FUNCS(X) \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkCreateDevice) X(vkGetDeviceProcAddr) X(vkDestroySurfaceKHR) X(vkCreateWin32SurfaceKHR) \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR)
#define VKP_DFUNCS(X) \
    X(vkGetDeviceQueue) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) \
    X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) X(vkQueueSubmit) X(vkDeviceWaitIdle) \
    X(vkCreateCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) \
    X(vkResetCommandBuffer) X(vkCreateFence) X(vkWaitForFences) X(vkResetFences) X(vkCreateSemaphore) \
    X(vkCreateBuffer) X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkBindBufferMemory) \
    X(vkMapMemory) X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) \
    X(vkFreeMemory) X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage) X(vkCmdBlitImage) \
    X(vkCmdClearColorImage) X(vkCmdCopyImageToBuffer)

static PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
static PFN_vkCreateInstance vkCreateInstance;
#define X(f) static PFN_##f f;
VKP_FUNCS(X) VKP_DFUNCS(X)
#undef X

static struct {
    int ok, failed;
    HWND wnd;
    VkInstance inst;
    VkPhysicalDevice phys;
    VkDevice dev;
    VkQueue queue;
    uint32_t qfam;
    VkSurfaceKHR surf;
    VkSwapchainKHR swap;
    VkFormat swap_fmt;
    VkExtent2D extent;
    uint32_t nimg;
    VkImage img[8];
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkFence fence;
    VkSemaphore acquired, done;
    VkBuffer staging;
    VkDeviceMemory staging_mem;
    BYTE *staging_ptr;
    VkImage src;                 /* the game frame, BGRA */
    VkImage src_under;           /* the 2D that was there before the frame's 3D */
    int use_under;
    VkDeviceMemory src_mem;
    int src_w, src_h;
    int filter_linear;
    int shot_pending;
    VkBuffer shot_buf;
    VkDeviceMemory shot_mem;
    BYTE *shot_ptr;
} g_vk;

static uint32_t vkp_memtype(uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;
    uint32_t i;
    vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &mp);
    for (i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return 0xFFFFFFFF;
}

static int vkp_fail(const char *what, VkResult r)
{
    log_msg("vulkan: %s failed (%d)", what, (int)r);
    g_vk.failed = 1;
    return 0;
}

static int vkp_make_swapchain(void)
{
    VkSurfaceCapabilitiesKHR caps;
    VkSwapchainCreateInfoKHR ci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    VkSwapchainKHR old = g_vk.swap;
    VkResult r;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_vk.phys, g_vk.surf, &caps);
    if (caps.currentExtent.width == 0 || caps.currentExtent.height == 0) return 0;   /* minimised */
    g_vk.extent = caps.currentExtent;
    if (g_vk.extent.width == 0xFFFFFFFF) {
        RECT rc; GetClientRect(g_vk.wnd, &rc);
        g_vk.extent.width = rc.right; g_vk.extent.height = rc.bottom;
    }
    ci.surface = g_vk.surf;
    ci.minImageCount = caps.minImageCount + 1;
    if (caps.maxImageCount && ci.minImageCount > caps.maxImageCount) ci.minImageCount = caps.maxImageCount;
    ci.imageFormat = g_vk.swap_fmt;
    ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    ci.imageExtent = g_vk.extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;   /* vsync; the game paces itself */
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = old;
    if ((r = vkCreateSwapchainKHR(g_vk.dev, &ci, NULL, &g_vk.swap)) != VK_SUCCESS) return vkp_fail("swapchain", r);
    if (old) vkDestroySwapchainKHR(g_vk.dev, old, NULL);
    g_vk.nimg = 8;
    vkGetSwapchainImagesKHR(g_vk.dev, g_vk.swap, &g_vk.nimg, g_vk.img);
    log_msg("vulkan: swapchain %ux%u, %u images", g_vk.extent.width, g_vk.extent.height, g_vk.nimg);
    return 1;
}

static int vkp_init(HWND wnd)
{
    HMODULE lib;
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    const char *iext[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
    const char *dext[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkWin32SurfaceCreateInfoKHR sci = { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
    VkPhysicalDevice phys[8];
    uint32_t n = 8, i, q;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    VkSurfaceFormatKHR fmts[32];
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkSemaphoreCreateInfo semi = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryRequirements mr;
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkResult r;
    int best = -1;

    if (g_vk.ok) return 1;
    if (g_vk.failed) return 0;
    g_vk.wnd = wnd;
    if (!(lib = LoadLibraryA("vulkan-1.dll"))) { log_msg("vulkan: vulkan-1.dll not found"); g_vk.failed = 1; return 0; }
    vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)GetProcAddress(lib, "vkGetInstanceProcAddr");
    vkCreateInstance = (PFN_vkCreateInstance)vkGetInstanceProcAddr(NULL, "vkCreateInstance");
    app.pApplicationName = "Motor Mash";
    app.pEngineName = "MMCDA";
    app.apiVersion = VK_API_VERSION_1_1;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = iext;
    if ((r = vkCreateInstance(&ici, NULL, &g_vk.inst)) != VK_SUCCESS) return vkp_fail("instance", r);
#define X(f) f = (PFN_##f)vkGetInstanceProcAddr(g_vk.inst, #f);
    VKP_FUNCS(X)
#undef X
    sci.hinstance = GetModuleHandleA(NULL);
    sci.hwnd = wnd;
    if ((r = vkCreateWin32SurfaceKHR(g_vk.inst, &sci, NULL, &g_vk.surf)) != VK_SUCCESS) return vkp_fail("surface", r);
    vkEnumeratePhysicalDevices(g_vk.inst, &n, phys);
    for (i = 0; i < n && best < 0; i++) {           /* prefer a discrete GPU, else the first that works */
        VkPhysicalDeviceProperties pp;
        VkQueueFamilyProperties qf[16];
        uint32_t nq = 16;
        vkGetPhysicalDeviceProperties(phys[i], &pp);
        vkGetPhysicalDeviceQueueFamilyProperties(phys[i], &nq, qf);
        for (q = 0; q < nq; q++) {
            VkBool32 pres = 0;
            vkGetPhysicalDeviceSurfaceSupportKHR(phys[i], q, g_vk.surf, &pres);
            if ((qf[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && pres) {
                if (pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU || i == n - 1 || best < 0) {
                    g_vk.phys = phys[i]; g_vk.qfam = q;
                    if (pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) best = (int)i;
                    log_msg("vulkan: using %s", pp.deviceName);
                }
                break;
            }
        }
    }
    if (!g_vk.phys) { log_msg("vulkan: no usable GPU"); g_vk.failed = 1; return 0; }
    qci.queueFamilyIndex = g_vk.qfam;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = dext;
    if ((r = vkCreateDevice(g_vk.phys, &dci, NULL, &g_vk.dev)) != VK_SUCCESS) return vkp_fail("device", r);
#define X(f) f = (PFN_##f)vkGetDeviceProcAddr(g_vk.dev, #f);
    VKP_DFUNCS(X)
#undef X
    vkGetDeviceQueue(g_vk.dev, g_vk.qfam, 0, &g_vk.queue);
    n = 32;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_vk.surf, &n, fmts);
    g_vk.swap_fmt = fmts[0].format;
    for (i = 0; i < n; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM) g_vk.swap_fmt = VK_FORMAT_B8G8R8A8_UNORM;
    if (!vkp_make_swapchain()) return 0;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = g_vk.qfam;
    vkCreateCommandPool(g_vk.dev, &pci, NULL, &g_vk.pool);
    cai.commandPool = g_vk.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    vkAllocateCommandBuffers(g_vk.dev, &cai, &g_vk.cmd);
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateFence(g_vk.dev, &fci, NULL, &g_vk.fence);
    vkCreateSemaphore(g_vk.dev, &semi, NULL, &g_vk.acquired);
    vkCreateSemaphore(g_vk.dev, &semi, NULL, &g_vk.done);
    /* staging: largest game mode (1024x768) as BGRA */
    bci.size = 1024 * 768 * 4 * 2;   /* the 2D layers drawn over and under the 3D */
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    vkCreateBuffer(g_vk.dev, &bci, NULL, &g_vk.staging);
    vkGetBufferMemoryRequirements(g_vk.dev, g_vk.staging, &mr);
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = vkp_memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if ((r = vkAllocateMemory(g_vk.dev, &mai, NULL, &g_vk.staging_mem)) != VK_SUCCESS) return vkp_fail("staging memory", r);
    vkBindBufferMemory(g_vk.dev, g_vk.staging, g_vk.staging_mem, 0);
    vkMapMemory(g_vk.dev, g_vk.staging_mem, 0, bci.size, 0, (void **)&g_vk.staging_ptr);
    /* debug capture buffer (up to 4096x3072, or 4x the largest mode) */
    bci.size = 1024u * 768 * 4 * 4; bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(g_vk.dev, &bci, NULL, &g_vk.shot_buf) == VK_SUCCESS) {
        vkGetBufferMemoryRequirements(g_vk.dev, g_vk.shot_buf, &mr);
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = vkp_memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(g_vk.dev, &mai, NULL, &g_vk.shot_mem) == VK_SUCCESS) {
            vkBindBufferMemory(g_vk.dev, g_vk.shot_buf, g_vk.shot_mem, 0);
            vkMapMemory(g_vk.dev, g_vk.shot_mem, 0, bci.size, 0, (void **)&g_vk.shot_ptr);
        }
    }
    g_vk.ok = 1;
    log_msg("vulkan: ready");
    return 1;
}

static int vkp_make_src(int w, int h)
{
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkMemoryRequirements mr;
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    if (g_vk.src && g_vk.src_w == w && g_vk.src_h == h) return 1;
    vkDeviceWaitIdle(g_vk.dev);
    if (g_vk.src) { vkDestroyImage(g_vk.dev, g_vk.src, NULL); vkFreeMemory(g_vk.dev, g_vk.src_mem, NULL); }
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_B8G8R8A8_UNORM;
    ii.extent.width = w; ii.extent.height = h; ii.extent.depth = 1;
    ii.mipLevels = 1; ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(g_vk.dev, &ii, NULL, &g_vk.src) != VK_SUCCESS) return 0;
    vkGetImageMemoryRequirements(g_vk.dev, g_vk.src, &mr);
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = vkp_memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(g_vk.dev, &mai, NULL, &g_vk.src_mem) != VK_SUCCESS) return 0;
    vkBindImageMemory(g_vk.dev, g_vk.src, g_vk.src_mem, 0);
    g_vk.src_w = w; g_vk.src_h = h;
    return 1;
}

static void vkp_barrier(VkImage img, VkImageLayout from, VkImageLayout to,
                        VkAccessFlags sa, VkAccessFlags da, VkPipelineStageFlags ss, VkPipelineStageFlags ds)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask = sa; b.dstAccessMask = da;
    b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(g_vk.cmd, ss, ds, 0, 0, NULL, 0, NULL, 1, &b);
}

/* vkrender.h: draws the 3D batches and the 2D layer inside the frame's render pass */
static int vr_init(void);
static int vr_make_targets(void);
static int vr_overlay(int w, int h);
static int vr_underlay(int w, int h);
static const BYTE *vr_under_get(int w, int h, int pitch, int bpp);
static void vr_record(VkCommandBuffer cmd, uint32_t idx, int dx, int dy, int dw, int dh, int mode_w, int mode_h);
static void vr_end_frame(void);

/* present a game frame: the 2D layer (bits/pitch, 8-bit with palette or RGB565) over the 3D
   batches recorded since the last present. (aw, ah) is the aspect the picture should have on
   screen (e.g. 4:3 for 320x200). */
static void vkp_present(const BYTE *bits, int w, int h, int pitch, int bpp, const DWORD *pal, int aw, int ah)
{
    uint32_t idx;
    VkResult r;
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkBufferImageCopy cp = {0};
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    int x, y, dw, dh, dx, dy;
    const BYTE *under;
    static DWORD lut565[65536];
    static int lut_ok;

    if (!g_vk.ok || w <= 0 || h <= 0 || w > 1024 || h > 768) { vr_end_frame(); return; }
    {   /* window resized (mode change / fit_window)? */
        RECT rc;
        GetClientRect(g_vk.wnd, &rc);
        if (rc.right <= 0 || rc.bottom <= 0) { vr_end_frame(); return; }
        if ((uint32_t)rc.right != g_vk.extent.width || (uint32_t)rc.bottom != g_vk.extent.height) {
            vkDeviceWaitIdle(g_vk.dev);
            if (!vkp_make_swapchain() || !vr_make_targets()) { vr_end_frame(); return; }
        }
    }
    if (!vr_init()) { vr_end_frame(); return; }
    vkWaitForFences(g_vk.dev, 1, &g_vk.fence, VK_TRUE, UINT64_MAX);
    if (!vr_overlay(w, h)) { vr_end_frame(); return; }

    /* the 2D layer as BGRA; colour 0 (nothing drawn) is transparent */
    if (!lut_ok) {
        int v;
        for (v = 0; v < 65536; v++) {
            int r5 = (v >> 11) & 31, g6 = (v >> 5) & 63, b5 = v & 31;
            lut565[v] = (v ? 0xFF000000u : 0) | ((r5 * 255 / 31) << 16) | ((g6 * 255 / 63) << 8) | (b5 * 255 / 31);
        }
        lut_ok = 1;
    }
    /* With a snapshot from the frame's first Render (vr_under_get), the 2D is split in two:
       what was already there goes under the 3D (as D3DRM rendered over it: the menu
       background), what changed after it goes over (HUD, menu sprites drawn later). */
    under = g_vr_2d_only ? NULL : vr_under_get(w, h, pitch, bpp);
    for (y = 0; y < h; y++) {
        DWORD *d = (DWORD *)(g_vk.staging_ptr + (size_t)y * w * 4);
        DWORD *u = (DWORD *)(g_vk.staging_ptr + (size_t)w * h * 4 + (size_t)y * w * 4);
        if (bpp == 16) {
            const WORD *s = (const WORD *)(bits + (size_t)y * pitch);
            if (under) {
                const WORD *o = (const WORD *)(under + (size_t)y * pitch);
                for (x = 0; x < w; x++) { u[x] = lut565[o[x]]; d[x] = s[x] != o[x] ? lut565[s[x]] : 0; }
            } else if (g_vr_2d_only) for (x = 0; x < w; x++) d[x] = lut565[s[x]] | 0xFF000000u;
            else for (x = 0; x < w; x++) d[x] = lut565[s[x]];
        } else if (bpp == 8 && pal) {
            const BYTE *s = bits + (size_t)y * pitch;
            if (under) {
                const BYTE *o = under + (size_t)y * pitch;
                for (x = 0; x < w; x++) { u[x] = o[x] ? pal[o[x]] : 0; d[x] = (s[x] != o[x] && s[x]) ? pal[s[x]] : 0; }
            } else for (x = 0; x < w; x++) d[x] = s[x] ? pal[s[x]] : 0;
        } else memset(d, 0, w * 4);
    }
    if (under && !vr_underlay(w, h)) under = NULL;

    r = vkAcquireNextImageKHR(g_vk.dev, g_vk.swap, UINT64_MAX, g_vk.acquired, VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { vkDeviceWaitIdle(g_vk.dev); vkp_make_swapchain(); vr_make_targets(); vr_end_frame(); return; }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { vr_end_frame(); return; }
    vkResetFences(g_vk.dev, 1, &g_vk.fence);
    vkResetCommandBuffer(g_vk.cmd, 0);
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(g_vk.cmd, &bi);
    vkp_barrier(g_vk.src, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    cp.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    cp.imageSubresource.layerCount = 1;
    cp.imageExtent.width = w; cp.imageExtent.height = h; cp.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(g_vk.cmd, g_vk.staging, g_vk.src, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    vkp_barrier(g_vk.src, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    if (under) {
        vkp_barrier(g_vk.src_under, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        cp.bufferOffset = (VkDeviceSize)w * h * 4;
        vkCmdCopyBufferToImage(g_vk.cmd, g_vk.staging, g_vk.src_under, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
        cp.bufferOffset = 0;
        vkp_barrier(g_vk.src_under, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }
    g_vk.use_under = under != NULL;
    {   /* aspect fit */
        double k = min((double)g_vk.extent.width / aw, (double)g_vk.extent.height / ah);
        dw = (int)(aw * k); dh = (int)(ah * k);
        dx = ((int)g_vk.extent.width - dw) / 2; dy = ((int)g_vk.extent.height - dh) / 2;
    }
    vr_record(g_vk.cmd, idx, dx, dy, dw, dh, w, h);
    {   /* debug capture: a file vkshot.req next to mmcda.ini asks for the next frame as vkshot.bmp */
        static DWORD last_check;
        char req[MAX_PATH];
        if (g_shot_force || GetTickCount() - last_check > 500) {
            last_check = GetTickCount();
            snprintf(req, sizeof req, "%svkshot.req", g_home);
            if ((g_shot_force || GetFileAttributesA(req) != INVALID_FILE_ATTRIBUTES) && g_vk.shot_pending == 0 &&
                g_vk.extent.width * g_vk.extent.height * 4 <= 1024u * 768 * 4 * 4) {
                DeleteFileA(req);
                g_shot_force = 0;
                g_vk.shot_pending = 1;
                {   /* the 2D layers too: vkshot2d.bin (over) and vkshot2u.bin (under) */
                    FILE *f;
                    int hdr[4] = { w, h, bpp, pitch };
                    snprintf(req, sizeof req, "%svkshot2d.bin", g_home);
                    if ((f = fopen(req, "wb"))) { fwrite(hdr, sizeof hdr, 1, f); fwrite(bits, pitch, h, f); fclose(f); }
                    snprintf(req, sizeof req, "%svkshot2u.bin", g_home);
                    if (under && (f = fopen(req, "wb"))) { fwrite(hdr, sizeof hdr, 1, f); fwrite(under, pitch, h, f); fclose(f); }
                    else DeleteFileA(req);
                }
            }
        }
        if (g_vk.shot_pending == 1) {
            VkBufferImageCopy sc = {0};
            vkp_barrier(g_vk.img[idx], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            sc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; sc.imageSubresource.layerCount = 1;
            sc.imageExtent.width = g_vk.extent.width; sc.imageExtent.height = g_vk.extent.height; sc.imageExtent.depth = 1;
            vkCmdCopyImageToBuffer(g_vk.cmd, g_vk.img[idx], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_vk.shot_buf, 1, &sc);
            vkp_barrier(g_vk.img[idx], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                        VK_ACCESS_TRANSFER_READ_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
            g_vk.shot_pending = 2;
        }
    }
    vkEndCommandBuffer(g_vk.cmd);
    si.waitSemaphoreCount = 1; si.pWaitSemaphores = &g_vk.acquired; si.pWaitDstStageMask = &ws;
    si.commandBufferCount = 1; si.pCommandBuffers = &g_vk.cmd;
    si.signalSemaphoreCount = 1; si.pSignalSemaphores = &g_vk.done;
    vkQueueSubmit(g_vk.queue, 1, &si, g_vk.fence);
    pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &g_vk.done;
    pi.swapchainCount = 1; pi.pSwapchains = &g_vk.swap; pi.pImageIndices = &idx;
    r = vkQueuePresentKHR(g_vk.queue, &pi);
    if (g_vk.shot_pending == 2) {   /* write vkshot.bmp (top-down 32-bit) */
        char path[MAX_PATH];
        FILE *f;
        vkWaitForFences(g_vk.dev, 1, &g_vk.fence, VK_TRUE, UINT64_MAX);
        snprintf(path, sizeof path, "%svkshot.bmp", g_home);
        if ((f = fopen(path, "wb"))) {
            BITMAPFILEHEADER fh = { 0x4D42 };
            BITMAPINFOHEADER ih = { sizeof ih };
            DWORD bytes = g_vk.extent.width * g_vk.extent.height * 4;
            ih.biWidth = g_vk.extent.width; ih.biHeight = -(LONG)g_vk.extent.height;
            ih.biPlanes = 1; ih.biBitCount = 32;
            fh.bfOffBits = sizeof fh + sizeof ih; fh.bfSize = fh.bfOffBits + bytes;
            fwrite(&fh, sizeof fh, 1, f); fwrite(&ih, sizeof ih, 1, f);
            fwrite(g_vk.shot_ptr, bytes, 1, f);
            fclose(f);
        }
        g_vk.shot_pending = 0;
    }
    vr_end_frame();
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) { vkDeviceWaitIdle(g_vk.dev); vkp_make_swapchain(); vr_make_targets(); }
}
