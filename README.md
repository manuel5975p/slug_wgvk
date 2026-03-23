# slug_wgvk

GPU accelerated text rendering using the Slug algorithm, implemented in native C with WebGPU via Vulkan.

![Hello, Slug!](examples/hello_slug.png)

## What is this?

slug_wgvk renders text at high quality on the GPU by evaluating quadratic Bezier curves directly in fragment shaders. Instead of rasterizing glyphs into bitmaps at fixed resolutions, every glyph is stored as a set of curves and rendered analytically per pixel. This means text stays perfectly crisp at any size or zoom level.

The project is a native C port of the TypeScript/browser based [slug_webgpu](https://github.com/user/slug-webgpu) project, targeting desktop Vulkan through the WGVK (WebGPU on Vulkan) backend.

![GPU Accelerated Text](examples/gpu_text.png)

## How it works

1. **Glyph extraction**: Fonts are parsed with `stb_truetype`. Cubic Bezier curves are split into quadratics via de Casteljau subdivision.
2. **Band acceleration**: Each glyph is divided into 8x8 horizontal and vertical bands. Only curves overlapping a given band are tested per pixel, drastically reducing per fragment work.
3. **GPU rendering**: Curve data and band indices are packed into textures. The vertex shader positions dilated glyph quads; the fragment shader solves quadratic polynomials to compute exact coverage, producing smooth antialiased text.

![Quadratic Bezier Curves](examples/bezier.png)

## Building

```bash
mkdir build && cd build
cmake ..
make
```

### Dependencies

| Dependency | Purpose | How it's obtained |
|---|---|---|
| WGVK | WebGPU on Vulkan implementation | Local sibling directory or fetched from GitHub |
| GLFW | Window management (interactive mode) | Fetched via CMake |
| stb_truetype.h | TrueType font parsing | Downloaded at build time |
| stb_image_write.h | PNG image output (headless mode) | Included in repository |

## Usage

### Interactive mode

Opens a window and renders text in real time. Supports window resizing.

```bash
./slug_wgvk [font.ttf] [text]
./slug_wgvk /usr/share/fonts/TTF/DejaVuSans.ttf "Hello World"
```

### Headless mode

Renders text to a PNG (default) or PPM image file. No window is opened. Uses only the WebGPU API with no display server dependency.

```bash
./slug_headless [OPTIONS] [TEXT...]
```

| Option | Description | Default |
|---|---|---|
| `-f`, `--font` | Font file path or family name | sans serif via fontconfig |
| `-s`, `--size` | Base font size in pixels | 200 |
| `-S`, `--scale` | Scale multiplier | 1.0 |
| `-o`, `--output` | Output file (.png or .ppm) | output.png |
| `-W`, `--width` | Image width in pixels | auto fit |
| `-H`, `--height` | Image height in pixels | auto fit |
| `-p`, `--padding` | Padding around text | 20 |

**Examples:**

```bash
./slug_headless Hello World
./slug_headless -f "DejaVu Sans" -s 100 --scale 2 -o big.png "GPU text"
./slug_headless -f /usr/share/fonts/TTF/DejaVuSans.ttf -o greeting.png Greetings
```

Font names are resolved through fontconfig when a direct file path is not provided.

![The quick brown fox jumps over the lazy dog](examples/pangram.png)

## Architecture

```
slug.h / slug.c          Core text preparation: glyph curves, bands, GPU data packing
main.c                   Interactive windowed renderer (GLFW + WebGPU)
headless.c               Headless PNG/PPM renderer (WebGPU only, no window)
SlugVertexShader.wgsl     Vertex dilation and MVP transform
SlugPixelShader.wgsl      Quadratic curve coverage evaluation
stb_image_write.h         Embedded single header PNG writer
```

## Acknowledgments

The Slug algorithm was developed by Eric Lengyel. This implementation follows the general approach described in his work on GPU accelerated vector text rendering.

## License

Public domain / MIT. The stb headers are public domain (authored by Sean Barrett).
