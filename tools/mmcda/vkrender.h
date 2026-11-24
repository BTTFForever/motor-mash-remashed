/* vkrender.h - draws the D3DRM scene with Vulkan (new renderer, stage 2).
 *
 * Like D3DRM itself, the scene work is done on the CPU: Viewport::Render walks the frame tree,
 * transforms and lights each mesh vertex (ambient + directional/point lights, D3DRM's model)
 * and turns decals (texture visuals: the menus, sprites, smoke) into screen-facing quads.
 * The results go into one vertex array; at present time everything is drawn in a single
 * render pass: viewport clears, the 3D batches (depth-tested, perspective-correct textures,
 * colour keys as alpha), then the game's 2D layer on top (vdd.h back buffer, transparent where
 * nothing was drawn).
 */
#include "shaders_spv.h"

#define VKR_FUNCS(X) \
    X(vkCreateRenderPass) X(vkCreateFramebuffer) X(vkDestroyFramebuffer) X(vkCreateImageView) \
    X(vkDestroyImageView) X(vkCreateShaderModule) X(vkCreatePipelineLayout) X(vkCreateGraphicsPipelines) \
    X(vkCreateDescriptorSetLayout) X(vkCreateDescriptorPool) X(vkAllocateDescriptorSets) \
    X(vkFreeDescriptorSets) X(vkUpdateDescriptorSets) X(vkCreateSampler) X(vkCmdBeginRenderPass) \
    X(vkCmdEndRenderPass) X(vkCmdBindPipeline) X(vkCmdBindVertexBuffers) X(vkCmdBindDescriptorSets) \
    X(vkCmdDraw) X(vkCmdDrawIndexed) X(vkCmdBindIndexBuffer) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdPushConstants) X(vkCmdClearAttachments) \
    X(vkCmdCopyBuffer)
#define X(f) static PFN_##f f;
VKR_FUNCS(X)
#undef X

typedef struct rvert { float p[4], uv[2]; DWORD col; float fog; } rvert;   /* 32 bytes */

typedef struct vkt {                    /* GPU copy of a texture */
    VkImage img;
    VkDeviceMemory mem;
    VkImageView view;
    VkDescriptorSet set;
    int w, h;
    DWORD gen, palgen;
    void *pal;
    int keyed;
    int filter;                         /* which sampler the descriptor set uses */
} vkt;

typedef struct rbatch {
    int clear;                          /* 1: clear rect to color (+ depth) */
    vtex *tex;                          /* AddRef'd; NULL = untextured */
    int first, count;
    LONG vx, vy, vw, vh;                /* viewport in game pixels */
    DWORD color;                        /* clear colour / fog colour */
    int fog;
    int nodepth;                        /* debug: drawn without the depth test */
    int cull;                           /* 1: back faces culled (meshes, as D3DRM) */
} rbatch;
typedef struct rmerged { rbatch *b; int first, count; } rmerged;   /* one draw of merged batches */

static struct {
    int ok;
    VkRenderPass pass;
    VkImage depth;
    VkDeviceMemory depth_mem;
    VkImageView depth_view, views[8];
    VkFramebuffer fb[8];
    uint32_t fb_w, fb_h;
    VkDescriptorSetLayout dsl;
    VkDescriptorPool pool;
    VkPipelineLayout layout;
    VkPipeline pipe3d, pipe2d, pipe3d_cull;
    VkSampler smp_tex, smp_2d, smp_tex_smooth;
    VkBuffer vb, up;
    VkDeviceMemory vb_mem, up_mem;
    BYTE *vb_ptr, *up_ptr, *ib_ptr;
    VkBuffer ib; VkDeviceMemory ib_mem;   /* merged draws: vertex numbers, grouped by state */
    size_t up_used;
    vkt white, overlay, underlay;
    BYTE *under;                 /* the 2D layer at the frame's first Render */
    int under_w, under_h, under_pitch, under_bpp, have_under;
    rvert *v; int nv, cv;
    rbatch *b; int nb, cb;
    int *ord, cord; rmerged *md; int nmd;   /* merged draws (vr_record) */
} g_vkr;

static int g_vkr_dec_seen, g_vkr_dec_behind, g_vkr_frames_seen;
#define VR_VB_SIZE (16u << 20)
#define VR_UP_SIZE (16u << 20)

static int vr_buffer(VkDeviceSize size, VkBufferUsageFlags use, VkBuffer *buf, VkDeviceMemory *mem, BYTE **ptr)
{
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryRequirements mr;
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    bci.size = size; bci.usage = use;
    if (vkCreateBuffer(g_vk.dev, &bci, NULL, buf) != VK_SUCCESS) return 0;
    vkGetBufferMemoryRequirements(g_vk.dev, *buf, &mr);
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = vkp_memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(g_vk.dev, &mai, NULL, mem) != VK_SUCCESS) return 0;
    vkBindBufferMemory(g_vk.dev, *buf, *mem, 0);
    vkMapMemory(g_vk.dev, *mem, 0, size, 0, (void **)ptr);
    return 1;
}

static int vr_image(int w, int h, VkFormat fmt, VkImageUsageFlags use, VkImageAspectFlags aspect,
                    VkImage *img, VkDeviceMemory *mem, VkImageView *view)
{
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    VkMemoryRequirements mr;
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_2D; ii.format = fmt;
    ii.extent.width = w; ii.extent.height = h; ii.extent.depth = 1;
    ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL; ii.usage = use;
    if (vkCreateImage(g_vk.dev, &ii, NULL, img) != VK_SUCCESS) return 0;
    vkGetImageMemoryRequirements(g_vk.dev, *img, &mr);
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = vkp_memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(g_vk.dev, &mai, NULL, mem) != VK_SUCCESS) return 0;
    vkBindImageMemory(g_vk.dev, *img, *mem, 0);
    vi.image = *img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt;
    vi.subresourceRange.aspectMask = aspect; vi.subresourceRange.levelCount = 1; vi.subresourceRange.layerCount = 1;
    return vkCreateImageView(g_vk.dev, &vi, NULL, view) == VK_SUCCESS;
}

static void vr_destroy_targets(void)
{
    uint32_t i;
    for (i = 0; i < 8; i++) {
        if (g_vkr.fb[i]) { vkDestroyFramebuffer(g_vk.dev, g_vkr.fb[i], NULL); g_vkr.fb[i] = 0; }
        if (g_vkr.views[i]) { vkDestroyImageView(g_vk.dev, g_vkr.views[i], NULL); g_vkr.views[i] = 0; }
    }
    if (g_vkr.depth_view) { vkDestroyImageView(g_vk.dev, g_vkr.depth_view, NULL); g_vkr.depth_view = 0; }
    if (g_vkr.depth) { vkDestroyImage(g_vk.dev, g_vkr.depth, NULL); vkFreeMemory(g_vk.dev, g_vkr.depth_mem, NULL); g_vkr.depth = 0; }
}

static int vr_make_targets(void)
{
    uint32_t i;
    vr_destroy_targets();
    if (!vr_image(g_vk.extent.width, g_vk.extent.height, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                  VK_IMAGE_ASPECT_DEPTH_BIT, &g_vkr.depth, &g_vkr.depth_mem, &g_vkr.depth_view)) return 0;
    for (i = 0; i < g_vk.nimg; i++) {
        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        VkImageView att[2];
        vi.image = g_vk.img[i]; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = g_vk.swap_fmt;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; vi.subresourceRange.levelCount = 1; vi.subresourceRange.layerCount = 1;
        if (vkCreateImageView(g_vk.dev, &vi, NULL, &g_vkr.views[i]) != VK_SUCCESS) return 0;
        att[0] = g_vkr.views[i]; att[1] = g_vkr.depth_view;
        fi.renderPass = g_vkr.pass; fi.attachmentCount = 2; fi.pAttachments = att;
        fi.width = g_vk.extent.width; fi.height = g_vk.extent.height; fi.layers = 1;
        if (vkCreateFramebuffer(g_vk.dev, &fi, NULL, &g_vkr.fb[i]) != VK_SUCCESS) return 0;
    }
    g_vkr.fb_w = g_vk.extent.width; g_vkr.fb_h = g_vk.extent.height;
    return 1;
}

static VkPipeline vr_pipeline(int depth, int cull)
{
    VkShaderModuleCreateInfo smi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    VkShaderModule vs, ps;
    VkPipelineShaderStageCreateInfo st[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
                                              { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
    VkVertexInputBindingDescription vb = { 0, sizeof(rvert), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription va[4] = {
        { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0 }, { 1, 0, VK_FORMAT_R32G32_SFLOAT, 16 },
        { 2, 0, VK_FORMAT_B8G8R8A8_UNORM, 24 }, { 3, 0, VK_FORMAT_R32_SFLOAT, 28 } };
    VkPipelineVertexInputStateCreateInfo vis = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ias = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState cba = { 0 };
    VkPipelineColorBlendStateCreateInfo cbs = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dys = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    VkGraphicsPipelineCreateInfo pci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    VkPipeline p = 0;
    smi.codeSize = sizeof k_vs_spv; smi.pCode = k_vs_spv;
    vkCreateShaderModule(g_vk.dev, &smi, NULL, &vs);
    smi.codeSize = sizeof k_ps_spv; smi.pCode = k_ps_spv;
    vkCreateShaderModule(g_vk.dev, &smi, NULL, &ps);
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = ps; st[1].pName = "main";
    vis.vertexBindingDescriptionCount = 1; vis.pVertexBindingDescriptions = &vb;
    vis.vertexAttributeDescriptionCount = 4; vis.pVertexAttributeDescriptions = va;
    ias.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    vps.viewportCount = 1; vps.scissorCount = 1;
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = cull ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_CLOCKWISE; rs.lineWidth = 1;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    ds.depthTestEnable = depth; ds.depthWriteEnable = depth; ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    cba.colorWriteMask = 0xF;
    cbs.attachmentCount = 1; cbs.pAttachments = &cba;
    dys.dynamicStateCount = 2; dys.pDynamicStates = dyn;
    pci.stageCount = 2; pci.pStages = st;
    pci.pVertexInputState = &vis; pci.pInputAssemblyState = &ias; pci.pViewportState = &vps;
    pci.pRasterizationState = &rs; pci.pMultisampleState = &ms; pci.pDepthStencilState = &ds;
    pci.pColorBlendState = &cbs; pci.pDynamicState = &dys;
    pci.layout = g_vkr.layout; pci.renderPass = g_vkr.pass;
    if (vkCreateGraphicsPipelines(g_vk.dev, VK_NULL_HANDLE, 1, &pci, NULL, &p) != VK_SUCCESS) log_msg("vulkan: pipeline failed");
    return p;
}

static void vr_set_texture_desc(vkt *t, VkSampler s)
{
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    VkDescriptorImageInfo ii;
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    ai.descriptorPool = g_vkr.pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &g_vkr.dsl;
    if (vkAllocateDescriptorSets(g_vk.dev, &ai, &t->set) != VK_SUCCESS) { t->set = 0; return; }
    ii.sampler = s; ii.imageView = t->view; ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    w.dstSet = t->set; w.dstBinding = 0; w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &ii;
    vkUpdateDescriptorSets(g_vk.dev, 1, &w, 0, NULL);
}

static void vr_free_texture(vkt *t)
{
    if (!t) return;
    vkDeviceWaitIdle(g_vk.dev);
    if (t->set) vkFreeDescriptorSets(g_vk.dev, g_vkr.pool, 1, &t->set);
    if (t->view) vkDestroyImageView(g_vk.dev, t->view, NULL);
    if (t->img) vkDestroyImage(g_vk.dev, t->img, NULL);
    if (t->mem) vkFreeMemory(g_vk.dev, t->mem, NULL);
    memset(t, 0, sizeof *t);
}

static int vr_init(void)
{
    VkAttachmentDescription at[2] = { 0 };
    VkAttachmentReference cr = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL }, dr = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sp = { 0 };
    VkSubpassDependency dep = { 0 };
    VkRenderPassCreateInfo rpi = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    VkDescriptorPoolSize psz = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8192 };
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    VkPushConstantRange pcr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16 };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    if (g_vkr.ok) return 1;
#define X(f) f = (PFN_##f)vkGetDeviceProcAddr(g_vk.dev, #f);
    VKR_FUNCS(X)
#undef X
    at[0].format = g_vk.swap_fmt; at[0].samples = VK_SAMPLE_COUNT_1_BIT;
    at[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; at[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    at[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; at[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; at[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    at[1] = at[0];
    at[1].format = VK_FORMAT_D32_SFLOAT; at[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1; sp.pColorAttachments = &cr; sp.pDepthStencilAttachment = &dr;
    dep.srcSubpass = VK_SUBPASS_EXTERNAL; dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.dstStageMask = dep.srcStageMask;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    rpi.attachmentCount = 2; rpi.pAttachments = at; rpi.subpassCount = 1; rpi.pSubpasses = &sp;
    rpi.dependencyCount = 1; rpi.pDependencies = &dep;
    if (vkCreateRenderPass(g_vk.dev, &rpi, NULL, &g_vkr.pass) != VK_SUCCESS) return 0;
    dli.bindingCount = 1; dli.pBindings = &b;
    vkCreateDescriptorSetLayout(g_vk.dev, &dli, NULL, &g_vkr.dsl);
    dpi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dpi.maxSets = 8192; dpi.poolSizeCount = 1; dpi.pPoolSizes = &psz;
    vkCreateDescriptorPool(g_vk.dev, &dpi, NULL, &g_vkr.pool);
    pli.setLayoutCount = 1; pli.pSetLayouts = &g_vkr.dsl;
    pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
    vkCreatePipelineLayout(g_vk.dev, &pli, NULL, &g_vkr.layout);
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;   /* the original look; linear is an option later */
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.maxLod = 0;
    vkCreateSampler(g_vk.dev, &si, NULL, &g_vkr.smp_tex);
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;     /* the "smooth" texture option */
    vkCreateSampler(g_vk.dev, &si, NULL, &g_vkr.smp_tex_smooth);
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(g_vk.dev, &si, NULL, &g_vkr.smp_2d);
    g_vkr.pipe3d = vr_pipeline(1, 0);
    g_vkr.pipe3d_cull = vr_pipeline(1, 1);
    g_vkr.pipe2d = vr_pipeline(0, 0);
    if (!vr_buffer(VR_VB_SIZE, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &g_vkr.vb, &g_vkr.vb_mem, &g_vkr.vb_ptr)) return 0;
    if (!vr_buffer(VR_VB_SIZE / sizeof(rvert) * 4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &g_vkr.ib, &g_vkr.ib_mem, &g_vkr.ib_ptr)) return 0;
    if (!vr_buffer(VR_UP_SIZE, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &g_vkr.up, &g_vkr.up_mem, &g_vkr.up_ptr)) return 0;
    if (!vr_make_targets()) return 0;
    g_vkr.ok = 1;
    log_msg("vulkan: 3D renderer ready");
    return 1;
}

/* ---- textures ---- */
static int vr_overlay(int w, int h)   /* the image the 2D layer is uploaded to (g_vk.src) */
{
    if (g_vkr.overlay.img && g_vkr.overlay.w == w && g_vkr.overlay.h == h) return 1;
    if (g_vkr.overlay.img) vr_free_texture(&g_vkr.overlay);
    if (!vr_image(w, h, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                  VK_IMAGE_ASPECT_COLOR_BIT, &g_vkr.overlay.img, &g_vkr.overlay.mem, &g_vkr.overlay.view)) return 0;
    g_vkr.overlay.w = w; g_vkr.overlay.h = h;
    vr_set_texture_desc(&g_vkr.overlay, g_vkr.smp_2d);
    g_vk.src = g_vkr.overlay.img;
    return 1;
}

static int vr_underlay(int w, int h)
{
    if (g_vkr.underlay.img && g_vkr.underlay.w == w && g_vkr.underlay.h == h) return 1;
    if (g_vkr.underlay.img) vr_free_texture(&g_vkr.underlay);
    if (!vr_image(w, h, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                  VK_IMAGE_ASPECT_COLOR_BIT, &g_vkr.underlay.img, &g_vkr.underlay.mem, &g_vkr.underlay.view)) return 0;
    g_vkr.underlay.w = w; g_vkr.underlay.h = h;
    vr_set_texture_desc(&g_vkr.underlay, g_vkr.smp_2d);
    g_vk.src_under = g_vkr.underlay.img;
    return 1;
}

static const BYTE *vr_under_get(int w, int h, int pitch, int bpp)
{
    if (!g_vkr.have_under || g_vkr.under_w != w || g_vkr.under_h != h || g_vkr.under_pitch != pitch ||
        g_vkr.under_bpp != bpp) return NULL;
    return g_vkr.under;
}

/* first Render of a frame: remember the 2D drawn so far (it goes under the 3D) */
static void vr_snapshot_under(vrmvp *vp)
{
    vsurf *t;
    size_t size;
    if (g_vkr.have_under || !vp->dev || !vp->dev->d3ddev || !(t = vp->dev->d3ddev->target) || !t->bits) return;
    size = (size_t)t->pitch * t->h;
    if ((size_t)g_vkr.under_pitch * g_vkr.under_h != size) { free(g_vkr.under); g_vkr.under = (BYTE *)malloc(size); }
    if (!g_vkr.under) return;
    memcpy(g_vkr.under, t->bits, size);
    g_vkr.under_w = t->w; g_vkr.under_h = t->h; g_vkr.under_pitch = t->pitch; g_vkr.under_bpp = t->bpp;
    g_vkr.have_under = 1;
}

static void vr_tex_free(vtex *t)
{
    if (t->gpu && g_vkr.ok) { vr_free_texture((vkt *)t->gpu); free(t->gpu); }
    t->gpu = NULL;
}
static DWORD vr_tex_palgen(vsurf *s) { return s->pal ? s->pal->gen : 0; }

/* convert a texture's surface to BGRA in the upload buffer; returns offset or -1 */
static long vr_stage_texture(vtex *t, int *keyed)
{
    vsurf *s = t->surf;
    int x, y, w = s->w, h = s->h;
    size_t need = (size_t)w * h * 4;
    DWORD *d;
    DWORD key = s->has_ck ? s->ck_src.dwColorSpaceLowValue : 0;
    int use_key = s->has_ck;
    if (g_vkr.up_used + need > VR_UP_SIZE) return -1;
    d = (DWORD *)(g_vkr.up_ptr + g_vkr.up_used);
    *keyed = 0;
    for (y = 0; y < h; y++) {
        const BYTE *row = s->bits + (size_t)y * s->pitch;
        for (x = 0; x < w; x++) {
            DWORD v, c;
            if (s->bpp == 8) {
                v = row[x];
                c = s->pal ? s->pal->bgra[v] : (0xFF000000u | v * 0x010101u);
            } else {
                v = ((const WORD *)row)[x];
                c = 0xFF000000u | ((((v >> 11) & 31) * 255 / 31) << 16) | ((((v >> 5) & 63) * 255 / 63) << 8) | ((v & 31) * 255 / 31);
            }
            if ((use_key && v == key) || (t->decal_transparent && (c & 0xFFFFFF) == (t->decal_tcolor & 0xFFFFFF))) {
                c &= 0x00FFFFFF;
                *keyed = 1;
            }
            *d++ = c;
        }
    }
    if (*keyed) {
        /* transparent texels take the colour of an opaque neighbour, so smooth filtering does
           not blend the (often black or magenta) key colour into the edges */
        DWORD *p = (DWORD *)(g_vkr.up_ptr + g_vkr.up_used);
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++) {
                static const int nx[4] = { -1, 1, 0, 0 }, ny[4] = { 0, 0, -1, 1 };
                int k2;
                if (p[y * w + x] >> 24) continue;
                for (k2 = 0; k2 < 4; k2++) {
                    int xx = x + nx[k2], yy = y + ny[k2];
                    if (xx >= 0 && yy >= 0 && xx < w && yy < h && (p[yy * w + xx] >> 24)) {
                        p[y * w + x] = p[yy * w + xx] & 0x00FFFFFF;
                        break;
                    }
                }
            }
    }
    {
        long off = (long)g_vkr.up_used;
        g_vkr.up_used = (g_vkr.up_used + need + 15) & ~(size_t)15;
        return off;
    }
}

static void vr_upload(VkCommandBuffer cmd, vkt *k, long off)
{
    VkBufferImageCopy cp = { 0 };
    vkp_barrier(k->img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    cp.bufferOffset = off;
    cp.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; cp.imageSubresource.layerCount = 1;
    cp.imageExtent.width = k->w; cp.imageExtent.height = k->h; cp.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cmd, g_vkr.up, k->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    vkp_barrier(k->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

/* make sure t has an up-to-date GPU image (records the upload into cmd) */
static vkt *vr_texture(VkCommandBuffer cmd, vtex *t)
{
    vkt *k;
    vsurf *s;
    if (!t || !(s = t->surf) || !s->bits) return &g_vkr.white;
    k = (vkt *)t->gpu;
    if (k && (k->w != s->w || k->h != s->h)) { vr_free_texture(k); free(k); k = NULL; t->gpu = NULL; }
    if (!k) {
        k = (vkt *)calloc(1, sizeof *k);
        k->w = s->w; k->h = s->h;
        if (!vr_image(k->w, k->h, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                      VK_IMAGE_ASPECT_COLOR_BIT, &k->img, &k->mem, &k->view)) { free(k); return &g_vkr.white; }
        k->filter = g_vr_filter;
        vr_set_texture_desc(k, k->filter ? g_vkr.smp_tex_smooth : g_vkr.smp_tex);
        k->gen = s->gen - 1;
        t->gpu = k;
    }
    if (k->filter != g_vr_filter && k->set) {   /* the texture option changed: point the set at the other sampler */
        static int idle_gen = -1;
        VkDescriptorImageInfo ii;
        VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        if (idle_gen != g_vr_filter) { vkDeviceWaitIdle(g_vk.dev); idle_gen = g_vr_filter; }   /* sets may be in use */
        k->filter = g_vr_filter;
        ii.sampler = k->filter ? g_vkr.smp_tex_smooth : g_vkr.smp_tex; ii.imageView = k->view;
        ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        w.dstSet = k->set; w.dstBinding = 0; w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &ii;
        vkUpdateDescriptorSets(g_vk.dev, 1, &w, 0, NULL);
    }
    if (k->gen != s->gen || k->pal != (void *)s->pal || k->palgen != vr_tex_palgen(s) || t->gpu_gen == 0) {
        long off = vr_stage_texture(t, &k->keyed);
        if (off >= 0) {
            vr_upload(cmd, k, off);
            k->gen = s->gen; k->pal = s->pal; k->palgen = vr_tex_palgen(s); t->gpu_gen = 1;
        }
    }
    return k->set ? k : &g_vkr.white;
}

/* ---- recording (game thread, during Viewport::Render / Clear) ---- */
static rvert *vr_alloc(int n)
{
    if (g_vkr.nv + n > (int)(VR_VB_SIZE / sizeof(rvert)) - 16) return NULL;
    if (g_vkr.nv + n > g_vkr.cv) {
        g_vkr.cv = max(g_vkr.cv * 2, g_vkr.nv + n + 4096);
        g_vkr.v = (rvert *)realloc(g_vkr.v, g_vkr.cv * sizeof *g_vkr.v);
    }
    g_vkr.nv += n;
    return g_vkr.v + g_vkr.nv - n;
}

static rbatch *vr_batch(void)
{
    if (g_vkr.nb == g_vkr.cb) { g_vkr.cb = g_vkr.cb ? g_vkr.cb * 2 : 256; g_vkr.b = (rbatch *)realloc(g_vkr.b, g_vkr.cb * sizeof *g_vkr.b); }
    memset(&g_vkr.b[g_vkr.nb], 0, sizeof g_vkr.b[0]);
    return &g_vkr.b[g_vkr.nb++];
}

/* the batch that the next triangles for (tex, viewport, fog) go into */
static rbatch *vr_target(vrmvp *vp, vtex *tex, int fog, DWORD fogcol, int cull)
{
    rbatch *b = g_vkr.nb ? &g_vkr.b[g_vkr.nb - 1] : NULL;
    if (b && !b->clear && b->tex == tex && b->vx == vp->x && b->vy == vp->y && b->vw == (LONG)vp->w &&
        b->vh == (LONG)vp->h && b->fog == fog && b->color == fogcol && b->cull == cull && b->first + b->count == g_vkr.nv) return b;
    b = vr_batch();
    b->tex = tex; VRM_ADDREF(tex);
    b->first = g_vkr.nv;
    b->vx = vp->x; b->vy = vp->y; b->vw = vp->w; b->vh = vp->h;
    b->fog = fog; b->color = fogcol; b->cull = cull;
    return b;
}

static vframe *vr_scene_of(vframe *f) { while (f && f->parent) f = f->parent; return f; }

static void vr_on_clear(vrmvp *vp)
{
    rbatch *b = vr_batch();
    vframe *scene = vr_scene_of(vp->camera);
    b->clear = 1;
    b->vx = vp->x; b->vy = vp->y; b->vw = vp->w; b->vh = vp->h;
    b->color = scene ? scene->bg : 0xFF000000u;
    /* The 2D layer is left alone: D3DRM only clears the areas it rendered into on the
       previous frame (its dirty rectangles), so 2D the game drew before Clear stays visible
       (the title screen is all 2D, drawn before the viewport is cleared). */
}

/* ---- scene traversal ---- */
typedef struct {
    vrmvp *vp;
    MAT view;                      /* world -> camera */
    float l, r, b, t, n, f;
    float amb[3];
    struct { float dir[3], col[3]; } dl[8]; int ndl;
    struct { float pos[3], col[3], range, a0, a1, a2; } pl[8]; int npl;
    int fog; float fog_s, fog_e; DWORD fog_col;
} rctx;

static void vr_clip(rctx *c, const float p[3], float out[4])
{
    float z = p[2];
    out[0] = (2 * p[0] - (c->l + c->r) * z) / (c->r - c->l);
    out[1] = ((c->t + c->b) * z - 2 * p[1]) / (c->t - c->b);
    out[2] = (z - c->n) * c->f / (c->f - c->n);
    out[3] = z;
}

static float vr_fog(rctx *c, float z)
{
    float f;
    if (!c->fog || c->fog_e <= c->fog_s) return 1;
    f = (c->fog_e - z) / (c->fog_e - c->fog_s);
    return f < 0 ? 0 : f > 1 ? 1 : f;
}

static void vr_collect_lights(rctx *c, vframe *f, MAT parent)
{
    MAT w;
    int i;
    m_mul(w, f->m, parent);
    for (i = 0; i < f->lights.n; i++) {
        vlight *l = (vlight *)f->lights.p[i];
        switch (l->type) {
        case 0: c->amb[0] += l->r; c->amb[1] += l->g; c->amb[2] += l->b; break;   /* ambient */
        case 3:                                                                     /* directional */
            if (c->ndl < 8) {
                float d[3] = { w[2][0], w[2][1], w[2][2] };
                v3_norm(d);
                memcpy(c->dl[c->ndl].dir, d, sizeof d);
                c->dl[c->ndl].col[0] = l->r; c->dl[c->ndl].col[1] = l->g; c->dl[c->ndl].col[2] = l->b;
                c->ndl++;
            }
            break;
        default:                                                                    /* point / spot / parallel point */
            if (c->npl < 8) {
                c->pl[c->npl].pos[0] = w[3][0]; c->pl[c->npl].pos[1] = w[3][1]; c->pl[c->npl].pos[2] = w[3][2];
                c->pl[c->npl].col[0] = l->r; c->pl[c->npl].col[1] = l->g; c->pl[c->npl].col[2] = l->b;
                c->pl[c->npl].range = l->range; c->pl[c->npl].a0 = l->att0; c->pl[c->npl].a1 = l->att1; c->pl[c->npl].a2 = l->att2;
                c->npl++;
            }
            break;
        }
    }
    for (i = 0; i < f->kids.n; i++) vr_collect_lights(c, (vframe *)f->kids.p[i], w);
}

static DWORD vr_pack(const float c[3], float a)
{
    int r = (int)(c[0] * 255 + 0.5f), g = (int)(c[1] * 255 + 0.5f), b = (int)(c[2] * 255 + 0.5f), al = (int)(a * 255 + 0.5f);
    r = r < 0 ? 0 : r > 255 ? 255 : r; g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b; al = al < 0 ? 0 : al > 255 ? 255 : al;
    return ((DWORD)al << 24) | (r << 16) | (g << 8) | b;
}

static void vr_draw_mesh(rctx *c, vmb *mb, MAT world)
{
    xgeo *geo = (xgeo *)mb->geo;
    MAT wv;
    int gi, i, lit = (mb->quality & 0x38) != 0;
    if (!geo) return;
    m_mul(wv, world, c->view);
    for (gi = 0; gi < geo->ng; gi++) {
        xgroup *g = &geo->g[gi];
        vtex *tex = (vtex *)(g->tex ? g->tex : mb->texture);
        rbatch *b;
        rvert *v;
        float base[3];
        if (!g->nv) continue;
        if (!(v = vr_alloc(g->nv))) return;
        g_vkr.nv -= g->nv;                       /* re-reserve after picking the batch */
        b = vr_target(c->vp, tex, c->fog, c->fog_col, !g_vr_backfaces);
        v = vr_alloc(g->nv);
        base[0] = g->col[0]; base[1] = g->col[1]; base[2] = g->col[2];
        if (mb->color != 0xFFFFFFFF && !g->tex) {
            base[0] = ((mb->color >> 16) & 255) / 255.f; base[1] = ((mb->color >> 8) & 255) / 255.f; base[2] = (mb->color & 255) / 255.f;
        }
        for (i = 0; i < g->nv; i++) {
            const xvert *s = &g->v[i];
            float pw[3], pc[3], col[3];
            m_xform(pw, s->p, world);
            m_xform(pc, s->p, wv);
            vr_clip(c, pc, v[i].p);
            v[i].uv[0] = s->uv[0]; v[i].uv[1] = s->uv[1];
            if (lit) {
                float n[3] = { s->n[0] * world[0][0] + s->n[1] * world[1][0] + s->n[2] * world[2][0],
                               s->n[0] * world[0][1] + s->n[1] * world[1][1] + s->n[2] * world[2][1],
                               s->n[0] * world[0][2] + s->n[1] * world[1][2] + s->n[2] * world[2][2] };
                float acc[3];
                int k;
                v3_norm(n);
                acc[0] = c->amb[0]; acc[1] = c->amb[1]; acc[2] = c->amb[2];
                for (k = 0; k < c->ndl; k++) {   /* light travels along dir: lit side faces -dir */
                    float d = -(n[0] * c->dl[k].dir[0] + n[1] * c->dl[k].dir[1] + n[2] * c->dl[k].dir[2]);
                    if (d > 0) { acc[0] += d * c->dl[k].col[0]; acc[1] += d * c->dl[k].col[1]; acc[2] += d * c->dl[k].col[2]; }
                }
                for (k = 0; k < c->npl; k++) {
                    float lv[3] = { c->pl[k].pos[0] - pw[0], c->pl[k].pos[1] - pw[1], c->pl[k].pos[2] - pw[2] };
                    float dist = sqrtf(lv[0] * lv[0] + lv[1] * lv[1] + lv[2] * lv[2]), d, att;
                    if (dist > c->pl[k].range || dist < 1e-6f) continue;
                    lv[0] /= dist; lv[1] /= dist; lv[2] /= dist;
                    d = n[0] * lv[0] + n[1] * lv[1] + n[2] * lv[2];
                    att = c->pl[k].a0 + c->pl[k].a1 * dist + c->pl[k].a2 * dist * dist;
                    if (d > 0 && att > 1e-6f) { d /= att; acc[0] += d * c->pl[k].col[0]; acc[1] += d * c->pl[k].col[1]; acc[2] += d * c->pl[k].col[2]; }
                }
                col[0] = g->emis[0] + base[0] * acc[0];
                col[1] = g->emis[1] + base[1] * acc[1];
                col[2] = g->emis[2] + base[2] * acc[2];
            } else memcpy(col, base, sizeof col);
            v[i].col = vr_pack(col, 1);
            v[i].fog = vr_fog(c, pc[2]);
        }
        b->count += g->nv;
    }
}

static void vr_draw_decal(rctx *c, vtex *t, MAT world)
{
    float o[3] = { 0, 0, 0 }, p[3], q[4][3];
    int tw, th, i;
    rbatch *b;
    rvert *v;
    static const int order[6] = { 0, 1, 2, 0, 2, 3 };
    static const float uv[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
    g_vkr_dec_seen++;
    if (!t->surf) return;
    tw = t->surf->w; th = t->surf->h;
    /* the decal sits at the frame origin: take it straight from the translation row, as
       D3DRM does. The game lets dust frames' scale compound every tick (AddScale AFTER,
       x40), which overflows to infinity, and 0 * inf in a full transform would be NaN. */
    (void)o;
    p[0] = world[3][0]; p[1] = world[3][1]; p[2] = world[3][2];
    m_xform(p, p, c->view);
    if (!(p[0] == p[0] && p[1] == p[1] && p[2] == p[2])) return;
    if (GetEnvironmentVariableA("MMCDA_VKDEBUG", NULL, 0)) {
        static int seen[8][8], n;
        int sx = tw >= 128 ? 7 : tw / 16, sy = th >= 128 ? 7 : th / 16;
        if (tw == 32 && seen[1][1] < 3) {
            int x2, y2, opaque = 0;
            for (y2 = 0; y2 < th; y2++) for (x2 = 0; x2 < tw; x2++) if (t->surf->bits[y2 * t->surf->pitch + x2] != (BYTE)t->surf->ck_src.dwColorSpaceLowValue) opaque++;
            log_msg("vkr: dust tex %p: %d of %d pixels opaque, pal %p, first entries %08lX %08lX, gpu %p", t, opaque, tw * th,
                    t->surf->pal, t->surf->pal ? t->surf->pal->bgra[0] : 0, t->surf->pal ? t->surf->pal->bgra[1] : 0, t->gpu);
        }
        if (seen[sx & 7][sy & 7]++ < 3 && n++ < 60)
            log_msg("vkr: decal %p tex %dx%d bpp %d ck %d/%lX size %.2f x %.2f scale %lu origin %ld,%ld transp %lu col %08lX cam %.1f %.1f %.1f",
                    t, tw, th, t->surf->bpp, t->surf->has_ck, t->surf->ck_src.dwColorSpaceLowValue, t->decal_w, t->decal_h,
                    t->decal_scale, t->decal_x, t->decal_y, t->decal_transparent, t->decal_tcolor, p[0], p[1], p[2]);
    }
    if (p[2] <= c->n) { g_vkr_dec_behind++; return; }
    if (t->decal_scale) {           /* world-sized: decal_w x decal_h units */
        float W = t->decal_w, H = t->decal_h;
        float x0 = p[0] - (float)t->decal_x / tw * W, y0 = p[1] + (float)t->decal_y / th * H;
        q[0][0] = x0;     q[0][1] = y0;     q[1][0] = x0 + W; q[1][1] = y0;
        q[2][0] = x0 + W; q[2][1] = y0 - H; q[3][0] = x0;     q[3][1] = y0 - H;
    } else {                        /* pixel-sized: the texture's own size on the device */
        float k = (c->r - c->l) / c->vp->w * p[2], kh = (c->t - c->b) / c->vp->h * p[2];
        float x0 = p[0] - t->decal_x * k, y0 = p[1] + t->decal_y * kh;
        q[0][0] = x0;          q[0][1] = y0;           q[1][0] = x0 + tw * k; q[1][1] = y0;
        q[2][0] = x0 + tw * k; q[2][1] = y0 - th * kh; q[3][0] = x0;          q[3][1] = y0 - th * kh;
    }
    {   /* a camera-facing square standing on the ground is half inside it: pull it towards
           the camera by half its size for the depth test (the dust puffs sit at the wheels) */
        float half = (q[1][0] - q[0][0]) * 0.5f, zq = p[2] - (half > 0 ? half : -half), k;
        if (zq < c->n * 1.01f) zq = c->n * 1.01f;
        k = zq / p[2];   /* scale the corners so the size on screen stays the same */
        for (i = 0; i < 4; i++) { q[i][0] *= k; q[i][1] *= k; q[i][2] = zq; }
    }
    b = vr_target(c->vp, t, c->fog, c->fog_col, 0);
    if (!(v = vr_alloc(6))) return;
    for (i = 0; i < 6; i++) {
        vr_clip(c, q[order[i]], v[i].p);
        v[i].uv[0] = uv[order[i]][0]; v[i].uv[1] = uv[order[i]][1];
        v[i].col = 0xFFFFFFFF;
        v[i].fog = vr_fog(c, p[2]);
    }
    b->count += 6;
}

static void vr_draw_frame(rctx *c, vframe *f, MAT parent, int depth)
{
    MAT w;
    int i;
    if (depth > 64) return;
    g_vkr_frames_seen++;
    m_mul(w, f->m, parent);
    for (i = 0; i < f->visuals.n; i++) {
        vrmobj *v = f->visuals.p[i];
        switch (v->kind) {
        case RK_MESHBUILDER: vr_draw_mesh(c, (vmb *)v, w); break;
        case RK_TEXTURE: vr_draw_decal(c, (vtex *)v, w); break;
        case RK_FRAME: vr_draw_frame(c, (vframe *)v, w, depth + 1); break;   /* frame used as a visual */
        default: break;
        }
    }
    for (i = 0; i < f->kids.n; i++) vr_draw_frame(c, (vframe *)f->kids.p[i], w, depth + 1);
}

static void vr_dump_tree(vframe *f, MAT parent, int d)
{
    MAT w;
    int i;
    m_mul(w, f->m, parent);
    if (d < 6)
        log_msg("vkr: %*sframe %p '%s' vis %d kids %d local t(%.1f %.1f %.1f) diag(%.2f %.2f %.2f) world t(%.1f %.1f %.1f)",
                d * 2, "", f, f->o.name, f->visuals.n, f->kids.n, f->m[3][0], f->m[3][1], f->m[3][2],
                f->m[0][0], f->m[1][1], f->m[2][2], w[3][0], w[3][1], w[3][2]);
    for (i = 0; i < f->kids.n && i < 12; i++) vr_dump_tree((vframe *)f->kids.p[i], w, d + 1);
}
static int g_vkr_renders;
static int vr_race_intro(void);   /* mmcda.c: RACE_INTRO */
/* debug (MMCDA_PERF): ms per frame in each stage, averaged */
static double g_perf_render, g_perf_record, g_perf_tex, g_perf_vcopy, g_perf_upbytes, g_perf_verts, g_perf_batches;
static void vr_on_render_body(vrmvp *vp, vframe *scene);
static void vr_on_render(vrmvp *vp, vframe *scene)
{
    double t0 = now_ms_qpc();
    vr_on_render_body(vp, scene);
    g_perf_render += now_ms_qpc() - t0;
}
static void vr_on_render_body(vrmvp *vp, vframe *scene)
{
    vrmvp intro;
    g_vkr_renders++;
    if (GetEnvironmentVariableA("MMCDA_VKDEBUG", NULL, 0) && vp->w == 320) {
        static int logged;
        if (logged++ % 300 == 0) {
            float l, r, b, t;
            vrmvp_plane(vp, &l, &r, &b, &t);
            log_msg("vkr: menu vp %lux%lu plane %d raw %.3f %.3f %.3f %.3f field %.3f front %.2f -> %.3f %.3f %.3f %.3f",
                    vp->w, vp->h, vp->has_plane, vp->pl, vp->pr, vp->pb, vp->pt, vp->field, vp->front, l, r, b, t);
        }
    }
    if (vr_race_intro() && vp->w * 3 > vp->h * 4 + 4) {
        /* the race intro's backdrop is laid out for 4:3: draw it in the middle 4:3 */
        {
            static int logged;
            if (logged++ < 3) {
                float l, r, b, t;
                MAT cw;
                vrmvp_plane(vp, &l, &r, &b, &t);
                vframe_world(vp->camera, cw);
                log_msg("vkr: intro vp %ld,%ld %lux%lu plane %d raw %.3f %.3f %.3f %.3f field %.3f front %.2f -> %.3f %.3f %.3f %.3f cam pos %.1f %.1f %.1f dir %.2f %.2f %.2f",
                        vp->x, vp->y, vp->w, vp->h, vp->has_plane, vp->pl, vp->pr, vp->pb, vp->pt, vp->field, vp->front,
                        l, r, b, t, cw[3][0], cw[3][1], cw[3][2], cw[2][0], cw[2][1], cw[2][2]);
            }
        }
        /* the game lays the backdrop out to fill the real (16:9) view: keep that framing
           (planes from the full viewport, no uniform scaling) and fit it into the 4:3 area,
           as the menu does at 320x240 */
        /* framed as on the original race display, 320x200 shown at 4:3 (the CRT stretched
           it): the intro's 3D items are placed for that view */
        float l = -vp->field, r = vp->field, b = -vp->field * 200.0f / 320.0f, t = vp->field * 200.0f / 320.0f;
        if (vp->has_plane) { l = vp->pl / vp->front; r = vp->pr / vp->front; t = (r - l) * 0.5f * 200.0f / 320.0f; b = -t; }
        intro = *vp;
        intro.w = vp->h * 4 / 3;
        intro.x = vp->x + (LONG)(vp->w - intro.w) / 2;
        intro.has_plane = 1; intro.uniform = 0;
        intro.pl = l * intro.front; intro.pr = r * intro.front; intro.pb = b * intro.front; intro.pt = t * intro.front;
        vp = &intro;
    }
    vr_snapshot_under(vp);
    rctx c;
    MAT cw, id;
    vframe *root;
    if (!scene || !vp->camera || !vp->w || !vp->h) return;
    memset(&c, 0, sizeof c);
    c.vp = vp;
    vframe_world(vp->camera, cw);
    m_inv(c.view, cw);
    {
        static int once;
        if (GetEnvironmentVariableA("MMCDA_VKDEBUG", NULL, 0) && once++ % 300 == 0) {
            vframe *p;
            int d = 0;
            log_msg("vkr: camera world [%.2f %.2f %.2f | %.2f %.2f %.2f | %.2f %.2f %.2f | %.2f %.2f %.2f] field %.3f front %.2f back %.2f",
                    cw[0][0], cw[0][1], cw[0][2], cw[1][0], cw[1][1], cw[1][2], cw[2][0], cw[2][1], cw[2][2], cw[3][0], cw[3][1], cw[3][2],
                    vp->field, vp->front, vp->back);
            { MAT id; m_ident(id); vr_dump_tree(vr_scene_of(scene), id, 0); }
            for (p = vp->camera; p; p = p->parent, d++)
                log_msg("vkr:   cam chain %d: %p local pos %.2f %.2f %.2f z-row %.2f %.2f %.2f", d, p, p->m[3][0], p->m[3][1], p->m[3][2], p->m[2][0], p->m[2][1], p->m[2][2]);
        }
    }
    vrmvp_plane(vp, &c.l, &c.r, &c.b, &c.t);
    c.n = vp->front > 0 ? vp->front : 0.01f;
    c.f = vp->back > c.n ? vp->back : c.n + 1;
    root = vr_scene_of(scene);
    if (root && root->fog_on) { c.fog = 1; c.fog_s = root->fog_start; c.fog_e = root->fog_end; c.fog_col = root->fog_color; }
    m_ident(id);
    vr_collect_lights(&c, root ? root : scene, id);
    {   /* scene is drawn with its parents' transform */
        MAT pw;
        vframe_world(scene->parent, pw);
        vr_draw_frame(&c, scene, pw, 0);
    }
}

/* ---- present: 3D batches + the 2D layer ---- */
static void vr_end_frame(void)
{
    int i;
    for (i = 0; i < g_vkr.nb; i++) VRM_RELEASE(g_vkr.b[i].tex);
    g_vkr.nb = 0; g_vkr.nv = 0;
    g_vkr.have_under = 0;
}

/* called by vkp_present between acquire and submit; the overlay image already holds the 2D frame */
/* same state (pipeline, viewport, fog colour, texture): the batches can share one draw */
static int vr_batch_key_cmp(const rbatch *a, const rbatch *b)
{
    if (a->nodepth != b->nodepth) return a->nodepth - b->nodepth;
    if (a->cull != b->cull) return a->cull - b->cull;
    if (a->vx != b->vx) return a->vx < b->vx ? -1 : 1;
    if (a->vy != b->vy) return a->vy < b->vy ? -1 : 1;
    if (a->vw != b->vw) return a->vw < b->vw ? -1 : 1;
    if (a->vh != b->vh) return a->vh < b->vh ? -1 : 1;
    if (a->color != b->color) return a->color < b->color ? -1 : 1;
    if (a->tex != b->tex) return (UINT_PTR)a->tex < (UINT_PTR)b->tex ? -1 : 1;
    return 0;
}
static void vr_batch_scissor(const rbatch *b, VkRect2D *sc, int dx, int dy, double kx, double ky)
{
    sc->offset.x = dx + (int)(b->vx * kx); sc->offset.y = dy + (int)(b->vy * ky);
    sc->extent.width = (uint32_t)(b->vw * kx + 0.5); sc->extent.height = (uint32_t)(b->vh * ky + 0.5);
    if (sc->offset.x < 0) sc->offset.x = 0;
    if (sc->offset.y < 0) sc->offset.y = 0;
    if (sc->offset.x + sc->extent.width > g_vk.extent.width) sc->extent.width = g_vk.extent.width - sc->offset.x;
    if (sc->offset.y + sc->extent.height > g_vk.extent.height) sc->extent.height = g_vk.extent.height - sc->offset.y;
}

static void vr_record_body(VkCommandBuffer cmd, uint32_t idx, int dx, int dy, int dw, int dh, int mode_w, int mode_h);
static void vr_record(VkCommandBuffer cmd, uint32_t idx, int dx, int dy, int dw, int dh, int mode_w, int mode_h)
{
    double t0 = now_ms_qpc();
    vr_record_body(cmd, idx, dx, dy, dw, dh, mode_w, mode_h);
    g_perf_record += now_ms_qpc() - t0;
}
static void vr_record_body(VkCommandBuffer cmd, uint32_t idx, int dx, int dy, int dw, int dh, int mode_w, int mode_h)
{
    VkRenderPassBeginInfo rb = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    VkClearValue cv[2];
    VkDeviceSize zero = 0;
    double kx = (double)dw / mode_w, ky = (double)dh / mode_h;
    int i, n2d;
    rvert *ov;
    /* texture uploads first (outside the render pass) */
    g_vkr.up_used = 0;
    if (!g_vkr.white.set &&
        vr_image(1, 1, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                 VK_IMAGE_ASPECT_COLOR_BIT, &g_vkr.white.img, &g_vkr.white.mem, &g_vkr.white.view)) {
        g_vkr.white.w = g_vkr.white.h = 1;
        *(DWORD *)g_vkr.up_ptr = 0xFFFFFFFF;
        g_vkr.up_used = 16;
        vr_upload(cmd, &g_vkr.white, 0);
        vr_set_texture_desc(&g_vkr.white, g_vkr.smp_tex);
    }
    {
        double t0 = now_ms_qpc();
        size_t up0 = g_vkr.up_used;
        for (i = 0; i < g_vkr.nb; i++) if (!g_vkr.b[i].clear) vr_texture(cmd, g_vkr.b[i].tex);
        g_perf_tex += now_ms_qpc() - t0; g_perf_upbytes += g_vkr.up_used - up0;
    }
    /* the 2D layer: a quad over the whole game picture */
    n2d = g_vkr.nv;
    if ((ov = vr_alloc(6))) {
        static const float q[6][4] = { { -1, -1, 0, 0 }, { 1, -1, 1, 0 }, { 1, 1, 1, 1 }, { -1, -1, 0, 0 }, { 1, 1, 1, 1 }, { -1, 1, 0, 1 } };
        for (i = 0; i < 6; i++) {
            ov[i].p[0] = q[i][0]; ov[i].p[1] = q[i][1]; ov[i].p[2] = 0; ov[i].p[3] = 1;
            ov[i].uv[0] = q[i][2]; ov[i].uv[1] = q[i][3];
            ov[i].col = 0xFFFFFFFF; ov[i].fog = 1;
        }
    }
    {   /* Merge: nothing in the game blends (transparency is alpha-tested) and everything is
           depth-tested, so draw order does not matter. Batches with the same texture and state
           become one indexed draw (vertex numbers listed group by group). ~4500 batches -> a few
           hundred draws. */
        double t0 = now_ms_qpc();
        DWORD *ib = (DWORD *)g_vkr.ib_ptr;
        int nd = 0, j, out = 0;
        memcpy(g_vkr.vb_ptr, g_vkr.v, g_vkr.nv * sizeof(rvert));   /* vertices stay in place */
        if (g_vkr.cord < g_vkr.nb) {
            g_vkr.cord = g_vkr.nb + 256;
            g_vkr.ord = (int *)realloc(g_vkr.ord, g_vkr.cord * sizeof(int));
            g_vkr.md = (rmerged *)realloc(g_vkr.md, g_vkr.cord * sizeof(rmerged));
        }
        /* group with a hash table (one pass) */
        {
            enum { HB = 2048 };
            static int head[HB], tail[HB], gfirst[HB], ngr;
            static int slot_of[HB];
            int *next = g_vkr.ord;               /* per batch: next batch in the same group */
            int gi, h;
            for (h = 0; h < ngr; h++) head[slot_of[h]] = -1;
            if (!ngr) for (h = 0; h < HB; h++) head[h] = -1;
            ngr = 0;
            for (i = 0; i < g_vkr.nb; i++) {
                rbatch *bb = &g_vkr.b[i];
                DWORD k;
                if (bb->clear || !bb->count) continue;
                k = (DWORD)(UINT_PTR)bb->tex * 2654435761u ^ (DWORD)bb->vx * 31 ^ (DWORD)bb->vy * 131 ^ (DWORD)bb->vw * 7 ^
                    (DWORD)bb->vh * 17 ^ bb->color * 3 ^ (DWORD)bb->cull * 0x9E37 ^ (DWORD)bb->nodepth * 0x7F4A;
                for (h = (k ^ (k >> 15)) & (HB - 1); head[h] >= 0 && vr_batch_key_cmp(&g_vkr.b[head[h]], bb); h = (h + 1) & (HB - 1)) ;
                if (head[h] < 0) {
                    if (ngr >= HB / 2) continue;     /* (never: far fewer distinct states) */
                    head[h] = tail[h] = i; slot_of[ngr] = h; gfirst[ngr++] = i;
                } else { next[tail[h]] = i; tail[h] = i; }
                next[i] = -1;
                nd++;
            }
            /* groups stay in the order the game first drew them: surfaces that overlap at the same
               depth (road edges, kerbs) resolve as in the original, where the later one wins */
            g_vkr.nmd = 0;
            for (gi = 0; gi < ngr; gi++) {
                int bi = gfirst[gi];
                g_vkr.md[g_vkr.nmd].b = &g_vkr.b[bi]; g_vkr.md[g_vkr.nmd].first = out; g_vkr.md[g_vkr.nmd].count = 0;
                for (; bi >= 0; bi = next[bi]) {
                    rbatch *bb = &g_vkr.b[bi];
                    int v0 = bb->first, c, n = bb->count;
                    DWORD *dst = ib + out;
                    for (c = 0; c < n; c++) dst[c] = v0 + c;
                    out += n; g_vkr.md[g_vkr.nmd].count += n;
                }
                g_vkr.nmd++;
            }
            (void)j;
        }
        g_perf_vcopy += now_ms_qpc() - t0; g_perf_verts += g_vkr.nv; g_perf_batches += g_vkr.nmd;
    }
    cv[0].color.float32[0] = cv[0].color.float32[1] = cv[0].color.float32[2] = 0; cv[0].color.float32[3] = 1;
    {
        static int dbg = -1, frames;
        if (dbg < 0) dbg = GetEnvironmentVariableA("MMCDA_VKDEBUG", NULL, 0) != 0;
        if (dbg) { cv[0].color.float32[0] = 1; cv[0].color.float32[2] = 1; }   /* magenta: shows what is not drawn */
        if (dbg && ++frames % 60 == 1) {
            int nc = 0, nd = 0, tris = 0;
            for (i = 0; i < g_vkr.nb; i++) { if (g_vkr.b[i].clear) nc++; else { nd++; tris += g_vkr.b[i].count / 3; } }
            log_msg("vkr: frame %d: %d clears, %d batches, %d triangles, renders %d, 2D %dx%d, frames %d decals %d (%d behind)",
                    frames, nc, nd, tris, g_vkr_renders, mode_w, mode_h, g_vkr_frames_seen, g_vkr_dec_seen, g_vkr_dec_behind);
            g_vkr_renders = 0;
            g_vkr_frames_seen = g_vkr_dec_seen = g_vkr_dec_behind = 0;
            for (i = 0; i < g_vkr.nb && i < 40; i += 8) {
                rbatch *b = &g_vkr.b[i];
                if (b->clear || !b->count) continue;
                {
                    rvert *v = &g_vkr.v[b->first];
                    log_msg("vkr:  batch %d tex %p %dx%d vp %ld,%ld %ldx%ld v0 (%.2f %.2f %.2f %.2f) uv %.2f,%.2f col %08lX",
                            i, b->tex, b->tex && b->tex->surf ? b->tex->surf->w : 0, b->tex && b->tex->surf ? b->tex->surf->h : 0,
                            b->vx, b->vy, b->vw, b->vh, v->p[0], v->p[1], v->p[2], v->p[3], v->uv[0], v->uv[1], v->col);
                }
            }
        }
    }
    cv[1].depthStencil.depth = 1; cv[1].depthStencil.stencil = 0;
    rb.renderPass = g_vkr.pass; rb.framebuffer = g_vkr.fb[idx];
    rb.renderArea.extent = g_vk.extent;
    rb.clearValueCount = 2; rb.pClearValues = cv;
    vkCmdBeginRenderPass(cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindVertexBuffers(cmd, 0, 1, &g_vkr.vb, &zero);
    vkCmdBindIndexBuffer(cmd, g_vkr.ib, 0, VK_INDEX_TYPE_UINT32);
    /* clears first (every viewport), then the 2D underlay, then the merged 3D draws */
    for (i = 0; i < g_vkr.nb; i++) {
        rbatch *b = &g_vkr.b[i];
        VkRect2D sc;
        VkClearAttachment ca[2];
        VkClearRect cr;
        if (!b->clear) continue;
        vr_batch_scissor(b, &sc, dx, dy, kx, ky);
        ca[0].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; ca[0].colorAttachment = 0;
        ca[0].clearValue.color.float32[0] = ((b->color >> 16) & 255) / 255.f;
        ca[0].clearValue.color.float32[1] = ((b->color >> 8) & 255) / 255.f;
        ca[0].clearValue.color.float32[2] = (b->color & 255) / 255.f;
        ca[0].clearValue.color.float32[3] = 1;
        ca[1].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT; ca[1].clearValue.depthStencil.depth = 1;
        cr.rect = sc; cr.baseArrayLayer = 0; cr.layerCount = 1;
        if (sc.extent.width && sc.extent.height) vkCmdClearAttachments(cmd, 2, ca, 1, &cr);
    }
    if (g_vk.use_under && g_vkr.underlay.set && ov) {
        /* the 2D that was there before the 3D (e.g. the menu background), after the clears */
        VkViewport uv = { (float)dx, (float)dy, (float)dw, (float)dh, 0, 1 };
        VkRect2D us = { { dx, dy }, { (uint32_t)dw, (uint32_t)dh } };
        float fogc[4] = { 0, 0, 0, 1 };
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_vkr.pipe2d);
        vkCmdSetViewport(cmd, 0, 1, &uv);
        vkCmdSetScissor(cmd, 0, 1, &us);
        vkCmdPushConstants(cmd, g_vkr.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, fogc);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_vkr.layout, 0, 1, &g_vkr.underlay.set, 0, NULL);
        vkCmdDraw(cmd, 6, 1, n2d, 0);
    }
    {   /* the merged draws, re-binding only what changes */
        VkPipeline cur_pipe = VK_NULL_HANDLE;
        VkDescriptorSet cur_set = VK_NULL_HANDLE;
        rbatch *cur_vp = NULL;
        DWORD cur_col = 0;
        int have_col = 0;
        for (i = 0; i < g_vkr.nmd; i++) {
            rbatch *b = g_vkr.md[i].b;
            VkRect2D sc;
            VkPipeline pp = b->nodepth ? g_vkr.pipe2d : b->cull ? g_vkr.pipe3d_cull : g_vkr.pipe3d;
            vkt *k = (b->tex && b->tex->gpu && ((vkt *)b->tex->gpu)->set) ? (vkt *)b->tex->gpu : &g_vkr.white;
            vr_batch_scissor(b, &sc, dx, dy, kx, ky);
            if (!sc.extent.width || !sc.extent.height) continue;
            if (pp != cur_pipe) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pp);
                cur_pipe = pp; cur_vp = NULL; have_col = 0; cur_set = VK_NULL_HANDLE;
            }
            if (!cur_vp || cur_vp->vx != b->vx || cur_vp->vy != b->vy || cur_vp->vw != b->vw || cur_vp->vh != b->vh) {
                VkViewport vpt;
                vpt.x = (float)(dx + b->vx * kx); vpt.y = (float)(dy + b->vy * ky);
                vpt.width = (float)(b->vw * kx); vpt.height = (float)(b->vh * ky);
                vpt.minDepth = 0; vpt.maxDepth = 1;
                vkCmdSetViewport(cmd, 0, 1, &vpt);
                vkCmdSetScissor(cmd, 0, 1, &sc);
                cur_vp = b;
            }
            if (!have_col || cur_col != b->color) {
                float fogc[4] = { ((b->color >> 16) & 255) / 255.f, ((b->color >> 8) & 255) / 255.f, (b->color & 255) / 255.f, 1 };
                vkCmdPushConstants(cmd, g_vkr.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, fogc);
                cur_col = b->color; have_col = 1;
            }
            if (k->set != cur_set) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_vkr.layout, 0, 1, &k->set, 0, NULL);
                cur_set = k->set;
            }
            vkCmdDrawIndexed(cmd, g_vkr.md[i].count, 1, g_vkr.md[i].first, 0, 0);
        }
    }
    if (ov && g_vkr.overlay.set) {
        VkViewport vpt = { (float)dx, (float)dy, (float)dw, (float)dh, 0, 1 };
        VkRect2D sc = { { dx, dy }, { (uint32_t)dw, (uint32_t)dh } };
        float fogc[4] = { 0, 0, 0, 1 };
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_vkr.pipe2d);
        vkCmdSetViewport(cmd, 0, 1, &vpt);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        vkCmdPushConstants(cmd, g_vkr.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, fogc);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_vkr.layout, 0, 1, &g_vkr.overlay.set, 0, NULL);
        vkCmdDraw(cmd, 6, 1, n2d, 0);
    }
    vkCmdEndRenderPass(cmd);
}
