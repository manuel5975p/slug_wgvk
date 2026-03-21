#ifndef SLUG_H
#define SLUG_H

#include <stdint.h>
#include <stb_truetype.h>

#define SLUG_TEX_WIDTH 4096
#define SLUG_BAND_COUNT 8

typedef struct {
    float p0x, p0y, p1x, p1y, p2x, p2y;
} SlugQuadCurve;

typedef struct {
    float xMin, yMin, xMax, yMax;
} SlugBounds;

typedef struct {
    int *curveIndices;
    int count;
} SlugBandEntry;

typedef struct {
    SlugBandEntry *hBands;
    SlugBandEntry *vBands;
    int hBandCount;
    int vBandCount;
} SlugGlyphBands;

typedef struct {
    int glyphId;
    SlugQuadCurve *curves;
    int curveCount;
    SlugGlyphBands bands;
    SlugBounds bounds;
} SlugGlyph;

typedef struct {
    float *vertices;        /* 20 floats per vertex (5 x vec4f, 80 bytes) */
    uint32_t *indices;      /* 6 per glyph quad (2 triangles) */
    int vertexCount;
    int indexCount;
    float *curveTexData;    /* RGBA32Float, width = SLUG_TEX_WIDTH */
    uint32_t *bandTexData;  /* RGBA32Uint,  width = SLUG_TEX_WIDTH */
    int curveTexHeight;
    int bandTexHeight;
    float totalAdvance;     /* in font units (multiply by scale for pixels) */
} SlugTextData;

/* Prepare GPU-ready text data.  Caller must free with slug_free_text_data(). */
SlugTextData slug_prepare_text(const stbtt_fontinfo *font,
                               const char *text, float fontSize);

void slug_free_text_data(SlugTextData *data);

#endif
