/*
 * slug_wgvk – Slug text rendering via WGVK (WebGPU-on-Vulkan) + simple_wgsl
 *
 * Port of slug-webgpu (TypeScript/browser) to native C.
 * Shaders are unmodified WGSL; simple_wgsl compiles them to SPIR-V at runtime.
 *
 * Usage:  slug_wgvk [font.ttf] [text]
 */

#include "common.h"          /* wgpu_init(), STRVIEW, nanoTime              */
#include "slug.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/*  File I/O                                                          */
/* ------------------------------------------------------------------ */

static char *read_file(const char *path, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Error: cannot open '%s'\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc(sz + 1);
    size_t rd = fread(buf, 1, sz, f);
    buf[rd] = '\0';
    fclose(f);
    if (out_size) *out_size = rd;
    return buf;
}

/* ------------------------------------------------------------------ */
/*  Application context                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    wgpu_base base;
    WGPURenderPipeline pipeline;
    WGPUBindGroup      bindGroup;
    WGPUBuffer         vertexBuffer;
    WGPUBuffer         indexBuffer;
    WGPUBuffer         uniformBuffer;
    int                indexCount;
    /* text metrics (in pixels) for centering */
    float totalWidth;
    float textHeight;
    float descender;
} Context;

static int g_resized = 0;

static void resize_cb(GLFWwindow *w, int width, int height)
{
    (void)w; (void)width; (void)height;
    g_resized = 1;
}

/* ------------------------------------------------------------------ */
/*  Main loop – one frame                                             */
/* ------------------------------------------------------------------ */

static void main_loop(void *userdata)
{
    Context *ctx = (Context *)userdata;
    WGPUDevice device = ctx->base.device;
    WGPUQueue  queue  = ctx->base.queue;

    glfwPollEvents();

    int width, height;
    glfwGetWindowSize(ctx->base.window, &width, &height);
    if (width <= 0 || height <= 0) return;

    WGPUSurfaceTexture st;
    wgpuSurfaceGetCurrentTexture(ctx->base.surface, &st);

    if (st.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal || g_resized) {
        g_resized = 0;
        wgpuSurfaceConfigure(ctx->base.surface, &(const WGPUSurfaceConfiguration){
            .device      = device,
            .format      = WGPUTextureFormat_BGRA8Unorm,
            .usage       = WGPUTextureUsage_RenderAttachment,
            .width       = (uint32_t)width,
            .height      = (uint32_t)height,
            .alphaMode   = WGPUCompositeAlphaMode_Opaque,
            .presentMode = WGPUPresentMode_Fifo,
        });
        return;
    }

    /* Update uniform buffer each frame to handle window resize */
    float fw = (float)width, fh = (float)height;
    float offX = (fw - ctx->totalWidth) / 2.0f;
    float offY = (fh - ctx->textHeight) / 2.0f + (-ctx->descender);

    float ubo[20] = {
        2.f/fw,  0,       0,  offX * 2.f/fw - 1.f,   /* row 0 */
        0,       2.f/fh,  0,  offY * 2.f/fh - 1.f,   /* row 1 */
        0,       0,       0,  0,                       /* row 2 */
        0,       0,       0,  1,                       /* row 3 */
        fw,      fh,      0,  0,                       /* viewport */
    };
    wgpuQueueWriteBuffer(queue, ctx->uniformBuffer, 0, ubo, sizeof ubo);

    /* Surface texture view */
    WGPUTextureView sv = wgpuTextureCreateView(st.texture,
        &(const WGPUTextureViewDescriptor){
            .format         = WGPUTextureFormat_BGRA8Unorm,
            .dimension      = WGPUTextureViewDimension_2D,
            .baseMipLevel   = 0, .mipLevelCount   = 1,
            .baseArrayLayer = 0, .arrayLayerCount = 1,
            .aspect         = WGPUTextureAspect_All,
            .usage          = WGPUTextureUsage_RenderAttachment,
        });

    /* Command encoder + render pass */
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(device, NULL);

    WGPURenderPassColorAttachment ca = {
        .view       = sv,
        .loadOp     = WGPULoadOp_Clear,
        .storeOp    = WGPUStoreOp_Store,
        .clearValue = {0.05, 0.05, 0.1, 1.0},
        .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED,
    };
    WGPURenderPassEncoder rp = wgpuCommandEncoderBeginRenderPass(enc,
        &(const WGPURenderPassDescriptor){
            .colorAttachmentCount = 1,
            .colorAttachments     = &ca,
        });

    wgpuRenderPassEncoderSetPipeline(rp, ctx->pipeline);
    wgpuRenderPassEncoderSetBindGroup(rp, 0, ctx->bindGroup, 0, NULL);
    wgpuRenderPassEncoderSetVertexBuffer(rp, 0, ctx->vertexBuffer, 0, WGPU_WHOLE_SIZE);
    wgpuRenderPassEncoderSetIndexBuffer(rp, ctx->indexBuffer,
                                         WGPUIndexFormat_Uint32, 0, WGPU_WHOLE_SIZE);
    wgpuRenderPassEncoderDrawIndexed(rp, ctx->indexCount, 1, 0, 0, 0);
    wgpuRenderPassEncoderEnd(rp);

    WGPUCommandBuffer cb = wgpuCommandEncoderFinish(enc, NULL);
    wgpuQueueSubmit(queue, 1, &cb);
    wgpuSurfacePresent(ctx->base.surface);

    wgpuRenderPassEncoderRelease(rp);
    wgpuTextureViewRelease(sv);
    wgpuCommandBufferRelease(cb);
    wgpuCommandEncoderRelease(enc);
}

/* ------------------------------------------------------------------ */
/*  main                                                              */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    const char *fontPath = argc > 1 ? argv[1] : "Inter.ttf";
    const char *text     = argc > 2 ? argv[2] : "Hello Slug";
    const float fontSize = 200.0f;

    /* ---- Load font ---- */
    size_t fontSz;
    unsigned char *fontBlob = (unsigned char *)read_file(fontPath, &fontSz);
    if (!fontBlob) return 1;

    stbtt_fontinfo font;
    if (!stbtt_InitFont(&font, fontBlob, stbtt_GetFontOffsetForIndex(fontBlob, 0))) {
        fprintf(stderr, "Error: failed to parse font '%s'\n", fontPath);
        free(fontBlob);
        return 1;
    }

    /* ---- Prepare Slug data ---- */
    SlugTextData sd = slug_prepare_text(&font, text, fontSize);
    if (sd.indexCount == 0) {
        fprintf(stderr, "Error: no renderable glyphs in '%s'\n", text);
        free(fontBlob);
        return 1;
    }

    float scale = stbtt_ScaleForPixelHeight(&font, fontSize);
    int ascent_i, descent_i, lineGap_i;
    stbtt_GetFontVMetrics(&font, &ascent_i, &descent_i, &lineGap_i);
    float ascender  = ascent_i  * scale;
    float descender = descent_i * scale;  /* negative */

    printf("Slug data ready: %d vertices, %d indices, curve tex %dx%d, band tex %dx%d\n",
           sd.vertexCount, sd.indexCount,
           SLUG_TEX_WIDTH, sd.curveTexHeight,
           SLUG_TEX_WIDTH, sd.bandTexHeight);

    /* ---- WGVK init ---- */
    Context *ctx = (Context *)calloc(1, sizeof(Context));
    ctx->base = wgpu_init();
    if (!ctx->base.device) { fprintf(stderr, "WGVK init failed\n"); return 1; }

    glfwSetWindowSizeCallback(ctx->base.window, resize_cb);

    WGPUDevice device = ctx->base.device;
    WGPUQueue  queue  = ctx->base.queue;

    ctx->totalWidth = sd.totalAdvance * scale;
    ctx->textHeight = ascender - descender;
    ctx->descender  = descender;
    ctx->indexCount  = sd.indexCount;

    /* ---- GPU buffers ---- */

    /* Vertex buffer */
    ctx->vertexBuffer = wgpuDeviceCreateBuffer(device,
        &(WGPUBufferDescriptor){
            .size  = sd.vertexCount * 80,
            .usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst,
        });
    wgpuQueueWriteBuffer(queue, ctx->vertexBuffer, 0,
                          sd.vertices, sd.vertexCount * 80);

    /* Index buffer */
    ctx->indexBuffer = wgpuDeviceCreateBuffer(device,
        &(WGPUBufferDescriptor){
            .size  = sd.indexCount * 4,
            .usage = WGPUBufferUsage_Index | WGPUBufferUsage_CopyDst,
        });
    wgpuQueueWriteBuffer(queue, ctx->indexBuffer, 0,
                          sd.indices, sd.indexCount * 4);

    /* Uniform buffer (ParamStruct: 4xvec4f matrix + vec4f viewport = 80 bytes) */
    ctx->uniformBuffer = wgpuDeviceCreateBuffer(device,
        &(WGPUBufferDescriptor){
            .size  = 80,
            .usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst,
        });

    /* ---- GPU textures ---- */

    /* Curve texture (RGBA32Float) */
    WGPUTexture curveTex = wgpuDeviceCreateTexture(device,
        &(WGPUTextureDescriptor){
            .usage       = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst,
            .dimension   = WGPUTextureDimension_2D,
            .size        = {SLUG_TEX_WIDTH, sd.curveTexHeight, 1},
            .format      = WGPUTextureFormat_RGBA32Float,
            .mipLevelCount = 1,
            .sampleCount = 1,
        });
    wgpuQueueWriteTexture(queue,
        &(WGPUTexelCopyTextureInfo){.texture = curveTex, .aspect = WGPUTextureAspect_All},
        sd.curveTexData,
        (size_t)SLUG_TEX_WIDTH * sd.curveTexHeight * 16,
        &(WGPUTexelCopyBufferLayout){
            .bytesPerRow  = SLUG_TEX_WIDTH * 16,
            .rowsPerImage = sd.curveTexHeight},
        &(WGPUExtent3D){SLUG_TEX_WIDTH, sd.curveTexHeight, 1});

    WGPUTextureView curveView = wgpuTextureCreateView(curveTex,
        &(WGPUTextureViewDescriptor){
            .format = WGPUTextureFormat_RGBA32Float,
            .dimension = WGPUTextureViewDimension_2D,
            .mipLevelCount = 1, .arrayLayerCount = 1,
            .aspect = WGPUTextureAspect_All,
            .usage = WGPUTextureUsage_TextureBinding,
        });

    /* Band texture (RGBA32Uint) */
    WGPUTexture bandTex = wgpuDeviceCreateTexture(device,
        &(WGPUTextureDescriptor){
            .usage       = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst,
            .dimension   = WGPUTextureDimension_2D,
            .size        = {SLUG_TEX_WIDTH, sd.bandTexHeight, 1},
            .format      = WGPUTextureFormat_RGBA32Uint,
            .mipLevelCount = 1,
            .sampleCount = 1,
        });
    wgpuQueueWriteTexture(queue,
        &(WGPUTexelCopyTextureInfo){.texture = bandTex, .aspect = WGPUTextureAspect_All},
        sd.bandTexData,
        (size_t)SLUG_TEX_WIDTH * sd.bandTexHeight * 16,
        &(WGPUTexelCopyBufferLayout){
            .bytesPerRow  = SLUG_TEX_WIDTH * 16,
            .rowsPerImage = sd.bandTexHeight},
        &(WGPUExtent3D){SLUG_TEX_WIDTH, sd.bandTexHeight, 1});

    WGPUTextureView bandView = wgpuTextureCreateView(bandTex,
        &(WGPUTextureViewDescriptor){
            .format = WGPUTextureFormat_RGBA32Uint,
            .dimension = WGPUTextureViewDimension_2D,
            .mipLevelCount = 1, .arrayLayerCount = 1,
            .aspect = WGPUTextureAspect_All,
            .usage = WGPUTextureUsage_TextureBinding,
        });

    /* ---- Shader modules ---- */
    char *vsSrc = read_file("SlugVertexShader.wgsl", NULL);
    char *fsSrc = read_file("SlugPixelShader.wgsl",  NULL);
    if (!vsSrc || !fsSrc) {
        fprintf(stderr, "Error: shader files not found in working directory\n");
        return 1;
    }

    WGPUShaderSourceWGSL vsWgsl = {
        .chain = {.sType = WGPUSType_ShaderSourceWGSL},
        .code  = {.data = vsSrc, .length = WGPU_STRLEN},
    };
    WGPUShaderModule vsMod = wgpuDeviceCreateShaderModule(device,
        &(WGPUShaderModuleDescriptor){.nextInChain = &vsWgsl.chain});

    WGPUShaderSourceWGSL fsWgsl = {
        .chain = {.sType = WGPUSType_ShaderSourceWGSL},
        .code  = {.data = fsSrc, .length = WGPU_STRLEN},
    };
    WGPUShaderModule fsMod = wgpuDeviceCreateShaderModule(device,
        &(WGPUShaderModuleDescriptor){.nextInChain = &fsWgsl.chain});

    /* ---- Pipeline layout ---- */

    /* Bind group layout:
       binding 0 = uniform buffer  (vertex)
       binding 1 = curve texture   (fragment, unfilterable-float)
       binding 2 = band  texture   (fragment, uint)                 */
    WGPUBindGroupLayoutEntry bglEntries[3] = {
        {
            .binding    = 0,
            .visibility = WGPUShaderStage_Vertex,
            .buffer     = {.type = WGPUBufferBindingType_Uniform},
        },
        {
            .binding    = 1,
            .visibility = WGPUShaderStage_Fragment,
            .texture    = {.sampleType    = WGPUTextureSampleType_UnfilterableFloat,
                           .viewDimension = WGPUTextureViewDimension_2D},
        },
        {
            .binding    = 2,
            .visibility = WGPUShaderStage_Fragment,
            .texture    = {.sampleType    = WGPUTextureSampleType_Uint,
                           .viewDimension = WGPUTextureViewDimension_2D},
        },
    };
    WGPUBindGroupLayout bgl = wgpuDeviceCreateBindGroupLayout(device,
        &(WGPUBindGroupLayoutDescriptor){.entryCount = 3, .entries = bglEntries});

    WGPUPipelineLayout pll = wgpuDeviceCreatePipelineLayout(device,
        &(WGPUPipelineLayoutDescriptor){
            .bindGroupLayoutCount = 1,
            .bindGroupLayouts     = &bgl,
        });

    /* ---- Render pipeline ---- */

    /* 5 vertex attributes, 80-byte stride */
    WGPUVertexAttribute vAttrs[5] = {
        {.shaderLocation = 0, .offset =  0, .format = WGPUVertexFormat_Float32x4},
        {.shaderLocation = 1, .offset = 16, .format = WGPUVertexFormat_Float32x4},
        {.shaderLocation = 2, .offset = 32, .format = WGPUVertexFormat_Float32x4},
        {.shaderLocation = 3, .offset = 48, .format = WGPUVertexFormat_Float32x4},
        {.shaderLocation = 4, .offset = 64, .format = WGPUVertexFormat_Float32x4},
    };
    WGPUVertexBufferLayout vbLayout = {
        .arrayStride   = 80,
        .attributeCount = 5,
        .attributes     = vAttrs,
        .stepMode       = WGPUVertexStepMode_Vertex,
    };

    WGPUBlendState blend = {
        .color = {.srcFactor = WGPUBlendFactor_SrcAlpha,
                  .dstFactor = WGPUBlendFactor_OneMinusSrcAlpha,
                  .operation = WGPUBlendOperation_Add},
        .alpha = {.srcFactor = WGPUBlendFactor_One,
                  .dstFactor = WGPUBlendFactor_OneMinusSrcAlpha,
                  .operation = WGPUBlendOperation_Add},
    };
    WGPUColorTargetState cts = {
        .format    = WGPUTextureFormat_BGRA8Unorm,
        .blend     = &blend,
        .writeMask = WGPUColorWriteMask_All,
    };
    WGPUFragmentState fs = {
        .module     = fsMod,
        .entryPoint = STRVIEW("main"),
        .targetCount = 1,
        .targets     = &cts,
    };

    ctx->pipeline = wgpuDeviceCreateRenderPipeline(device,
        &(WGPURenderPipelineDescriptor){
            .layout    = pll,
            .vertex    = {
                .module      = vsMod,
                .entryPoint  = STRVIEW("main"),
                .bufferCount = 1,
                .buffers     = &vbLayout,
            },
            .fragment  = &fs,
            .primitive = {
                .topology = WGPUPrimitiveTopology_TriangleList,
                .cullMode = WGPUCullMode_None,
            },
            .multisample = {.count = 1, .mask = 0xFFFFFFFF},
        });

    /* ---- Bind group ---- */
    WGPUBindGroupEntry bgEntries[3] = {
        {.binding = 0, .buffer = ctx->uniformBuffer, .size = 80},
        {.binding = 1, .textureView = curveView},
        {.binding = 2, .textureView = bandView},
    };
    ctx->bindGroup = wgpuDeviceCreateBindGroup(device,
        &(WGPUBindGroupDescriptor){
            .layout     = bgl,
            .entryCount = 3,
            .entries    = bgEntries,
        });

    /* Release setup-only objects */
    wgpuShaderModuleRelease(vsMod);
    wgpuShaderModuleRelease(fsMod);
    wgpuBindGroupLayoutRelease(bgl);
    wgpuPipelineLayoutRelease(pll);
    wgpuTextureRelease(curveTex);
    wgpuTextureRelease(bandTex);
    wgpuTextureViewRelease(curveView);
    wgpuTextureViewRelease(bandView);
    free(vsSrc);
    free(fsSrc);

    slug_free_text_data(&sd);
    free(fontBlob);

    printf("Rendering \"%s\" — press Escape to quit\n", text);

    /* ---- Main loop ---- */
    while (!glfwWindowShouldClose(ctx->base.window))
        main_loop(ctx);

    free(ctx);
    return 0;
}
