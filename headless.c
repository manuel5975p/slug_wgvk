/*
 * Headless Slug renderer — renders to an offscreen texture and saves PPM.
 * No window, no surface, no swapchain.
 */
#include "common.h"
#include "slug.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

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

static volatile int g_mapped = 0;
static void map_cb(WGPUMapAsyncStatus status, WGPUStringView msg, void *u1, void *u2) {
    (void)msg; (void)u1; (void)u2;
    g_mapped = (status == WGPUMapAsyncStatus_Success) ? 1 : -1;
}

int main(int argc, char **argv)
{
    const char *fontPath = argc > 1 ? argv[1] : "Inter.ttf";
    const char *text     = argc > 2 ? argv[2] : "AVA";
    const char *outPath  = argc > 3 ? argv[3] : "output.ppm";
    const float fontSize = 200.0f;
    const int W = 512, H = 512;

    /* Load font */
    size_t fontSz;
    unsigned char *fontBlob = (unsigned char *)read_file(fontPath, &fontSz);
    if (!fontBlob) return 1;
    stbtt_fontinfo font;
    if (!stbtt_InitFont(&font, fontBlob, stbtt_GetFontOffsetForIndex(fontBlob, 0))) {
        fprintf(stderr, "Failed to parse font\n"); return 1;
    }

    /* Prepare slug data */
    SlugTextData sd = slug_prepare_text(&font, text, fontSize);
    if (sd.indexCount == 0) { fprintf(stderr, "No glyphs\n"); return 1; }

    float scale = stbtt_ScaleForPixelHeight(&font, fontSize);
    int ascent_i, descent_i, lineGap_i;
    stbtt_GetFontVMetrics(&font, &ascent_i, &descent_i, &lineGap_i);
    float descender = descent_i * scale;

    /* WGVK headless init — just device+queue, no window */
    wgpu_base base = wgpu_init();
    if (!base.device) { fprintf(stderr, "WGVK init failed\n"); return 1; }
    WGPUDevice device = base.device;
    WGPUQueue queue = base.queue;

    /* Create offscreen render target */
    WGPUTexture rtTex = wgpuDeviceCreateTexture(device,
        &(WGPUTextureDescriptor){
            .usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc,
            .dimension = WGPUTextureDimension_2D,
            .size = {W, H, 1},
            .format = WGPUTextureFormat_RGBA8Unorm,
            .mipLevelCount = 1, .sampleCount = 1,
        });
    WGPUTextureView rtView = wgpuTextureCreateView(rtTex,
        &(WGPUTextureViewDescriptor){
            .format = WGPUTextureFormat_RGBA8Unorm,
            .dimension = WGPUTextureViewDimension_2D,
            .mipLevelCount = 1, .arrayLayerCount = 1,
            .aspect = WGPUTextureAspect_All,
            .usage = WGPUTextureUsage_RenderAttachment,
        });

    /* Readback buffer (RGBA8, 4 bytes per pixel, 256-byte row alignment) */
    uint32_t rowBytes = W * 4;
    uint32_t alignedRow = (rowBytes + 255) & ~255u;
    WGPUBuffer readBuf = wgpuDeviceCreateBuffer(device,
        &(WGPUBufferDescriptor){
            .size = alignedRow * H,
            .usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead,
        });

    /* GPU buffers — same as main.c */
    WGPUBuffer vertexBuffer = wgpuDeviceCreateBuffer(device,
        &(WGPUBufferDescriptor){ .size = sd.vertexCount * 80,
            .usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst });
    wgpuQueueWriteBuffer(queue, vertexBuffer, 0, sd.vertices, sd.vertexCount * 80);

    WGPUBuffer indexBuffer = wgpuDeviceCreateBuffer(device,
        &(WGPUBufferDescriptor){ .size = sd.indexCount * 4,
            .usage = WGPUBufferUsage_Index | WGPUBufferUsage_CopyDst });
    wgpuQueueWriteBuffer(queue, indexBuffer, 0, sd.indices, sd.indexCount * 4);

    WGPUBuffer uniformBuffer = wgpuDeviceCreateBuffer(device,
        &(WGPUBufferDescriptor){ .size = 80,
            .usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst });

    /* Textures */
    WGPUTexture curveTex = wgpuDeviceCreateTexture(device,
        &(WGPUTextureDescriptor){
            .usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst,
            .dimension = WGPUTextureDimension_2D,
            .size = {SLUG_TEX_WIDTH, sd.curveTexHeight, 1},
            .format = WGPUTextureFormat_RGBA32Float,
            .mipLevelCount = 1, .sampleCount = 1,
        });
    wgpuQueueWriteTexture(queue,
        &(WGPUTexelCopyTextureInfo){.texture = curveTex, .aspect = WGPUTextureAspect_All},
        sd.curveTexData, (size_t)SLUG_TEX_WIDTH * sd.curveTexHeight * 16,
        &(WGPUTexelCopyBufferLayout){ .bytesPerRow = SLUG_TEX_WIDTH * 16, .rowsPerImage = sd.curveTexHeight },
        &(WGPUExtent3D){SLUG_TEX_WIDTH, sd.curveTexHeight, 1});
    WGPUTextureView curveView = wgpuTextureCreateView(curveTex,
        &(WGPUTextureViewDescriptor){ .format = WGPUTextureFormat_RGBA32Float,
            .dimension = WGPUTextureViewDimension_2D,
            .mipLevelCount = 1, .arrayLayerCount = 1,
            .aspect = WGPUTextureAspect_All, .usage = WGPUTextureUsage_TextureBinding });

    WGPUTexture bandTex = wgpuDeviceCreateTexture(device,
        &(WGPUTextureDescriptor){
            .usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst,
            .dimension = WGPUTextureDimension_2D,
            .size = {SLUG_TEX_WIDTH, sd.bandTexHeight, 1},
            .format = WGPUTextureFormat_RGBA32Uint,
            .mipLevelCount = 1, .sampleCount = 1,
        });
    wgpuQueueWriteTexture(queue,
        &(WGPUTexelCopyTextureInfo){.texture = bandTex, .aspect = WGPUTextureAspect_All},
        sd.bandTexData, (size_t)SLUG_TEX_WIDTH * sd.bandTexHeight * 16,
        &(WGPUTexelCopyBufferLayout){ .bytesPerRow = SLUG_TEX_WIDTH * 16, .rowsPerImage = sd.bandTexHeight },
        &(WGPUExtent3D){SLUG_TEX_WIDTH, sd.bandTexHeight, 1});
    WGPUTextureView bandView = wgpuTextureCreateView(bandTex,
        &(WGPUTextureViewDescriptor){ .format = WGPUTextureFormat_RGBA32Uint,
            .dimension = WGPUTextureViewDimension_2D,
            .mipLevelCount = 1, .arrayLayerCount = 1,
            .aspect = WGPUTextureAspect_All, .usage = WGPUTextureUsage_TextureBinding });

    /* Shaders */
    char *vsSrc = read_file("SlugVertexShader.wgsl", NULL);
    char *fsSrc = read_file("SlugPixelShader.wgsl", NULL);
    if (!vsSrc || !fsSrc) { fprintf(stderr, "Shader files not found\n"); return 1; }

    WGPUShaderSourceWGSL vsWgsl = { .chain = {.sType = WGPUSType_ShaderSourceWGSL},
        .code = {.data = vsSrc, .length = WGPU_STRLEN} };
    WGPUShaderModule vsMod = wgpuDeviceCreateShaderModule(device,
        &(WGPUShaderModuleDescriptor){.nextInChain = &vsWgsl.chain});
    WGPUShaderSourceWGSL fsWgsl = { .chain = {.sType = WGPUSType_ShaderSourceWGSL},
        .code = {.data = fsSrc, .length = WGPU_STRLEN} };
    WGPUShaderModule fsMod = wgpuDeviceCreateShaderModule(device,
        &(WGPUShaderModuleDescriptor){.nextInChain = &fsWgsl.chain});

    /* Pipeline */
    WGPUBindGroupLayoutEntry bglEntries[3] = {
        { .binding = 0, .visibility = WGPUShaderStage_Vertex,
          .buffer = {.type = WGPUBufferBindingType_Uniform} },
        { .binding = 1, .visibility = WGPUShaderStage_Fragment,
          .texture = {.sampleType = WGPUTextureSampleType_UnfilterableFloat,
                      .viewDimension = WGPUTextureViewDimension_2D} },
        { .binding = 2, .visibility = WGPUShaderStage_Fragment,
          .texture = {.sampleType = WGPUTextureSampleType_Uint,
                      .viewDimension = WGPUTextureViewDimension_2D} },
    };
    WGPUBindGroupLayout bgl = wgpuDeviceCreateBindGroupLayout(device,
        &(WGPUBindGroupLayoutDescriptor){.entryCount = 3, .entries = bglEntries});
    WGPUPipelineLayout pll = wgpuDeviceCreatePipelineLayout(device,
        &(WGPUPipelineLayoutDescriptor){.bindGroupLayoutCount = 1, .bindGroupLayouts = &bgl});

    WGPUVertexAttribute vAttrs[5] = {
        {.shaderLocation = 0, .offset =  0, .format = WGPUVertexFormat_Float32x4},
        {.shaderLocation = 1, .offset = 16, .format = WGPUVertexFormat_Float32x4},
        {.shaderLocation = 2, .offset = 32, .format = WGPUVertexFormat_Float32x4},
        {.shaderLocation = 3, .offset = 48, .format = WGPUVertexFormat_Float32x4},
        {.shaderLocation = 4, .offset = 64, .format = WGPUVertexFormat_Float32x4},
    };
    WGPUVertexBufferLayout vbLayout = { .arrayStride = 80, .attributeCount = 5,
        .attributes = vAttrs, .stepMode = WGPUVertexStepMode_Vertex };

    WGPUBlendState blend = {
        .color = {.srcFactor = WGPUBlendFactor_One,
                  .dstFactor = WGPUBlendFactor_OneMinusSrcAlpha,
                  .operation = WGPUBlendOperation_Add},
        .alpha = {.srcFactor = WGPUBlendFactor_One,
                  .dstFactor = WGPUBlendFactor_OneMinusSrcAlpha,
                  .operation = WGPUBlendOperation_Add},
    };
    WGPUColorTargetState cts = { .format = WGPUTextureFormat_RGBA8Unorm,
        .blend = &blend, .writeMask = WGPUColorWriteMask_All };
    WGPUFragmentState fs = { .module = fsMod, .entryPoint = STRVIEW("main"),
        .targetCount = 1, .targets = &cts };

    WGPURenderPipeline pipeline = wgpuDeviceCreateRenderPipeline(device,
        &(WGPURenderPipelineDescriptor){
            .layout = pll,
            .vertex = { .module = vsMod, .entryPoint = STRVIEW("main"),
                        .bufferCount = 1, .buffers = &vbLayout },
            .fragment = &fs,
            .primitive = { .topology = WGPUPrimitiveTopology_TriangleList, .cullMode = WGPUCullMode_None },
            .multisample = {.count = 1, .mask = 0xFFFFFFFF},
        });

    WGPUBindGroupEntry bgEntries[3] = {
        {.binding = 0, .buffer = uniformBuffer, .size = 80},
        {.binding = 1, .textureView = curveView},
        {.binding = 2, .textureView = bandView},
    };
    WGPUBindGroup bindGroup = wgpuDeviceCreateBindGroup(device,
        &(WGPUBindGroupDescriptor){ .layout = bgl, .entryCount = 3, .entries = bgEntries });

    /* Set up uniforms */
    float fw = (float)W, fh = (float)H;
    float totalWidth = sd.totalAdvance * scale;
    float textHeight = (ascent_i - descent_i) * scale;
    float offX = (fw - totalWidth) / 2.0f;
    float offY = (fh - textHeight) / 2.0f + (-descender);

    float ubo[20] = {
        2.f/fw, 0, 0, offX * 2.f/fw - 1.f,
        0, 2.f/fh, 0, offY * 2.f/fh - 1.f,
        0, 0, 0, 0,
        0, 0, 0, 1,
        fw, fh, 0, 0,
    };
    wgpuQueueWriteBuffer(queue, uniformBuffer, 0, ubo, sizeof ubo);

    /* Render */
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(device, NULL);
    WGPURenderPassColorAttachment ca = {
        .view = rtView, .loadOp = WGPULoadOp_Clear, .storeOp = WGPUStoreOp_Store,
        .clearValue = {0.05, 0.05, 0.1, 1.0}, .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED,
    };
    WGPURenderPassEncoder rp = wgpuCommandEncoderBeginRenderPass(enc,
        &(const WGPURenderPassDescriptor){ .colorAttachmentCount = 1, .colorAttachments = &ca });
    wgpuRenderPassEncoderSetPipeline(rp, pipeline);
    wgpuRenderPassEncoderSetBindGroup(rp, 0, bindGroup, 0, NULL);
    wgpuRenderPassEncoderSetVertexBuffer(rp, 0, vertexBuffer, 0, WGPU_WHOLE_SIZE);
    wgpuRenderPassEncoderSetIndexBuffer(rp, indexBuffer, WGPUIndexFormat_Uint32, 0, WGPU_WHOLE_SIZE);
    wgpuRenderPassEncoderDrawIndexed(rp, sd.indexCount, 1, 0, 0, 0);
    wgpuRenderPassEncoderEnd(rp);

    /* Copy render target to readback buffer */
    wgpuCommandEncoderCopyTextureToBuffer(enc,
        &(WGPUTexelCopyTextureInfo){.texture = rtTex, .aspect = WGPUTextureAspect_All},
        &(WGPUTexelCopyBufferInfo){.buffer = readBuf,
            .layout = {.bytesPerRow = alignedRow, .rowsPerImage = H}},
        &(WGPUExtent3D){W, H, 1});

    WGPUCommandBuffer cb = wgpuCommandEncoderFinish(enc, NULL);
    wgpuQueueSubmit(queue, 1, &cb);

    /* Map and read back */
    WGPUBufferMapCallbackInfo mapInfo = { .callback = map_cb, .mode = WGPUCallbackMode_WaitAnyOnly };
    WGPUFuture mapFuture = wgpuBufferMapAsync(readBuf, WGPUMapMode_Read, 0, alignedRow * H, mapInfo);
    WGPUFutureWaitInfo waitInfo = { .future = mapFuture };
    wgpuInstanceWaitAny(base.instance, 1, &waitInfo, UINT64_MAX);

    if (g_mapped != 1) { fprintf(stderr, "Map failed\n"); return 1; }

    const uint8_t *mapped = (const uint8_t *)wgpuBufferGetMappedRange(readBuf, 0, alignedRow * H);

    /* Write PPM */
    FILE *out = fopen(outPath, "wb");
    fprintf(out, "P6\n%d %d\n255\n", W, H);
    for (int y = 0; y < H; y++) {
        const uint8_t *row = mapped + y * alignedRow;
        for (int x = 0; x < W; x++) {
            fputc(row[x*4+0], out); /* R */
            fputc(row[x*4+1], out); /* G */
            fputc(row[x*4+2], out); /* B */
        }
    }
    fclose(out);
    printf("Wrote %s (%dx%d)\n", outPath, W, H);

    /* Cleanup */
    slug_free_text_data(&sd);
    free(fontBlob);
    free(vsSrc);
    free(fsSrc);
    return 0;
}
