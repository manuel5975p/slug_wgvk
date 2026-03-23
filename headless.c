/*
 * Headless Slug renderer — renders to an offscreen texture and saves PNG/PPM.
 * No window, no surface, no swapchain.  Uses only webgpu.h — no GLFW.
 */
#include <webgpu/webgpu.h>
#include "slug.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <getopt.h>
#include <unistd.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#ifndef STRVIEW
#define STRVIEW(X) (WGPUStringView){X, sizeof(X) - 1}
#endif

/* Headless WebGPU init — instance + adapter + device + queue, no window. */
typedef struct {
    WGPUInstance  instance;
    WGPUAdapter   adapter;
    WGPUDevice    device;
    WGPUQueue     queue;
} HeadlessGPU;

static void on_adapter(WGPURequestAdapterStatus s, WGPUAdapter a,
                        WGPUStringView msg, void *u1, void *u2) {
    (void)s; (void)msg; (void)u2;
    *(WGPUAdapter *)u1 = a;
}
static void on_device(WGPURequestDeviceStatus s, WGPUDevice d,
                       WGPUStringView msg, void *u1, void *u2) {
    (void)s; (void)msg; (void)u2;
    *(WGPUDevice *)u1 = d;
}

static HeadlessGPU headless_gpu_init(void)
{
    HeadlessGPU g = {0};

    WGPUInstanceFeatureName features[] = {
        WGPUInstanceFeatureName_TimedWaitAny,
        WGPUInstanceFeatureName_ShaderSourceSPIRV,
    };
    WGPUInstanceDescriptor idesc = {
        .requiredFeatures     = features,
        .requiredFeatureCount = 2,
    };
    g.instance = wgpuCreateInstance(&idesc);
    if (!g.instance) return g;

    /* Adapter */
    WGPURequestAdapterOptions aopts = {
        .featureLevel = WGPUFeatureLevel_Compatibility,
        .backendType  = WGPUBackendType_WebGPU,
    };
    WGPURequestAdapterCallbackInfo acb = {
        .callback = on_adapter,
        .mode     = WGPUCallbackMode_WaitAnyOnly,
        .userdata1 = &g.adapter,
    };
    WGPUFuture af = wgpuInstanceRequestAdapter(g.instance, &aopts, acb);
    WGPUFutureWaitInfo aw = { .future = af };
    wgpuInstanceWaitAny(g.instance, 1, &aw, 1000000000);
    if (!g.adapter) return g;

    /* Device */
    WGPUDeviceDescriptor ddesc = {0};
    WGPURequestDeviceCallbackInfo dcb = {
        .callback  = on_device,
        .mode      = WGPUCallbackMode_WaitAnyOnly,
        .userdata1 = &g.device,
    };
    WGPUFuture df = wgpuAdapterRequestDevice(g.adapter, &ddesc, dcb);
    WGPUFutureWaitInfo dw = { .future = df };
    wgpuInstanceWaitAny(g.instance, 1, &dw, 1000000000);
    if (!g.device) return g;

    g.queue = wgpuDeviceGetQueue(g.device);
    return g;
}

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

/* Resolve a font by file path or family name via fontconfig (fc-match).
 * Returns a malloc'd path on success, NULL on failure. */
static char *resolve_font(const char *name_or_path)
{
    if (access(name_or_path, R_OK) == 0)
        return strdup(name_or_path);

    /* Sanitize for shell: only allow alphanumeric, space, hyphen, underscore */
    for (const char *p = name_or_path; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != ' ' && *p != '-' && *p != '_') {
            fprintf(stderr, "Error: invalid font name '%s'\n", name_or_path);
            return NULL;
        }
    }

    char cmd[512];
    snprintf(cmd, sizeof cmd, "fc-match --format=%%{file} '%s'", name_or_path);
    FILE *fp = popen(cmd, "r");
    if (!fp) return NULL;
    char buf[1024];
    if (!fgets(buf, sizeof buf, fp)) { pclose(fp); return NULL; }
    pclose(fp);
    size_t len = strlen(buf);
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r'))
        buf[--len] = '\0';
    if (len == 0 || access(buf, R_OK) != 0) return NULL;
    return strdup(buf);
}

static void print_help(const char *prog)
{
    printf(
        "Usage: %s [OPTIONS] [TEXT...]\n\n"
        "Headless Slug GPU text renderer -- renders text to a PNG or PPM image.\n\n"
        "Options:\n"
        "  -h, --help            Show this help and exit\n"
        "  -f, --font PATH|NAME  Font file or family name (default: sans-serif via fontconfig)\n"
        "  -s, --size PIXELS     Base font size in pixels (default: 200)\n"
        "  -S, --scale FACTOR    Scale multiplier for output (default: 1.0)\n"
        "  -o, --output PATH     Output file (default: output.png)\n"
        "  -W, --width PIXELS    Image width  (default: auto-fit to text)\n"
        "  -H, --height PIXELS   Image height (default: auto-fit to text)\n"
        "  -p, --padding PIXELS  Padding around text (default: 20)\n"
        "\n"
        "Output format is chosen by file extension: .png (default) or .ppm.\n"
        "Remaining arguments are joined as the text to render.\n"
        "If no text is given, renders \"Hello, Slug!\".\n"
        "\n"
        "Font resolution:\n"
        "  If --font is a readable file path, it is used directly.\n"
        "  Otherwise it is resolved as a family name via fontconfig (fc-match).\n"
        "  If --font is omitted, the default sans-serif font is used.\n"
        "\n"
        "Examples:\n"
        "  %s Hello World\n"
        "  %s -f 'DejaVu Sans' -s 100 --scale 2 -o big.png \"GPU text\"\n"
        "  %s -f /usr/share/fonts/TTF/DejaVuSans.ttf Greetings\n",
        prog, prog, prog, prog);
}

static volatile int g_mapped = 0;
static void map_cb(WGPUMapAsyncStatus status, WGPUStringView msg, void *u1, void *u2) {
    (void)msg; (void)u1; (void)u2;
    g_mapped = (status == WGPUMapAsyncStatus_Success) ? 1 : -1;
}

int main(int argc, char **argv)
{
    const char *fontSpec = NULL;
    const char *outPath  = "output.png";
    float fontSize       = 200.0f;
    float scaleFactor    = 1.0f;
    int userW = 0, userH = 0;   /* 0 = auto-fit */
    int padding = 20;

    static struct option longopts[] = {
        {"help",    no_argument,       NULL, 'h'},
        {"font",    required_argument, NULL, 'f'},
        {"size",    required_argument, NULL, 's'},
        {"scale",   required_argument, NULL, 'S'},
        {"output",  required_argument, NULL, 'o'},
        {"width",   required_argument, NULL, 'W'},
        {"height",  required_argument, NULL, 'H'},
        {"padding", required_argument, NULL, 'p'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "hf:s:S:o:W:H:p:", longopts, NULL)) != -1) {
        switch (opt) {
        case 'h': print_help(argv[0]); return 0;
        case 'f': fontSpec = optarg; break;
        case 's': fontSize = (float)atof(optarg); break;
        case 'S': scaleFactor = (float)atof(optarg); break;
        case 'o': outPath = optarg; break;
        case 'W': userW = atoi(optarg); break;
        case 'H': userH = atoi(optarg); break;
        case 'p': padding = atoi(optarg); break;
        default:  print_help(argv[0]); return 1;
        }
    }

    /* Remaining non-option args become the text (joined with spaces) */
    char *textBuf = NULL;
    const char *text;
    if (optind < argc) {
        size_t total = 0;
        for (int i = optind; i < argc; i++)
            total += strlen(argv[i]) + 1;
        textBuf = (char *)malloc(total);
        textBuf[0] = '\0';
        for (int i = optind; i < argc; i++) {
            if (i > optind) strcat(textBuf, " ");
            strcat(textBuf, argv[i]);
        }
        text = textBuf;
    } else {
        text = "Hello, Slug!";
    }

    /* Resolve font */
    char *fontPath;
    if (fontSpec) {
        fontPath = resolve_font(fontSpec);
        if (!fontPath) {
            fprintf(stderr, "Error: could not find font '%s'\n", fontSpec);
            free(textBuf);
            return 1;
        }
    } else {
        fontPath = resolve_font("sans-serif");
        if (!fontPath) {
            fprintf(stderr, "Error: no --font specified and fontconfig could not find a default font.\n"
                            "Install fontconfig or specify a font with --font.\n");
            free(textBuf);
            return 1;
        }
    }

    float effectiveSize = fontSize * scaleFactor;
    fprintf(stderr, "Font:  %s\n", fontPath);
    fprintf(stderr, "Size:  %.0f px (scale %.2fx -> %.0f px)\n",
            fontSize, scaleFactor, effectiveSize);
    fprintf(stderr, "Text:  \"%s\"\n", text);

    /* Load font */
    size_t fontSz;
    unsigned char *fontBlob = (unsigned char *)read_file(fontPath, &fontSz);
    if (!fontBlob) { free(fontPath); free(textBuf); return 1; }
    stbtt_fontinfo font;
    if (!stbtt_InitFont(&font, fontBlob, stbtt_GetFontOffsetForIndex(fontBlob, 0))) {
        fprintf(stderr, "Failed to parse font '%s'\n", fontPath);
        free(fontBlob); free(fontPath); free(textBuf);
        return 1;
    }

    /* Prepare slug data */
    SlugTextData sd = slug_prepare_text(&font, text, effectiveSize);
    if (sd.indexCount == 0) {
        fprintf(stderr, "No renderable glyphs in \"%s\"\n", text);
        free(fontBlob); free(fontPath); free(textBuf);
        return 1;
    }

    float scale = stbtt_ScaleForPixelHeight(&font, effectiveSize);
    int ascent_i, descent_i, lineGap_i;
    stbtt_GetFontVMetrics(&font, &ascent_i, &descent_i, &lineGap_i);
    float descender = descent_i * scale;

    /* Compute image dimensions (auto-fit or user override) */
    float totalWidth = sd.totalAdvance * scale;
    float textHeight = (ascent_i - descent_i) * scale;

    const int W = userW > 0 ? userW : (int)ceilf(totalWidth + 2.0f * padding);
    const int H = userH > 0 ? userH : (int)ceilf(textHeight + 2.0f * padding);
    fprintf(stderr, "Image: %dx%d\n", W, H);

    /* Headless GPU init — no window, no surface */
    HeadlessGPU gpu = headless_gpu_init();
    if (!gpu.device) { fprintf(stderr, "Headless GPU init failed\n"); return 1; }
    WGPUDevice device = gpu.device;
    WGPUQueue queue = gpu.queue;

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
    wgpuInstanceWaitAny(gpu.instance, 1, &waitInfo, UINT64_MAX);

    if (g_mapped != 1) { fprintf(stderr, "Map failed\n"); return 1; }

    const uint8_t *mapped = (const uint8_t *)wgpuBufferGetMappedRange(readBuf, 0, alignedRow * H);

    /* Detect output format by extension */
    const char *ext = strrchr(outPath, '.');
    int use_png = (!ext || strcasecmp(ext, ".ppm") != 0);

    if (use_png) {
        /* Build tightly packed RGBA buffer (stb_image_write needs contiguous rows) */
        uint8_t *rgba = (uint8_t *)malloc(W * H * 4);
        for (int y = 0; y < H; y++)
            memcpy(rgba + y * W * 4, mapped + y * alignedRow, W * 4);
        stbi_write_png(outPath, W, H, 4, rgba, W * 4);
        free(rgba);
    } else {
        FILE *out = fopen(outPath, "wb");
        fprintf(out, "P6\n%d %d\n255\n", W, H);
        for (int y = 0; y < H; y++) {
            const uint8_t *row = mapped + y * alignedRow;
            for (int x = 0; x < W; x++) {
                fputc(row[x*4+0], out);
                fputc(row[x*4+1], out);
                fputc(row[x*4+2], out);
            }
        }
        fclose(out);
    }
    printf("Wrote %s (%dx%d)\n", outPath, W, H);

    /* Cleanup */
    slug_free_text_data(&sd);
    free(fontBlob);
    free(fontPath);
    free(textBuf);
    free(vsSrc);
    free(fsSrc);
    return 0;
}
