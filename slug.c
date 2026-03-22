#define STB_TRUETYPE_IMPLEMENTATION
#include "slug.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/*  Utilities                                                         */
/* ------------------------------------------------------------------ */

static float pack_u32_as_f32(uint32_t v)
{
    float f;
    memcpy(&f, &v, sizeof f);
    return f;
}

/* ------------------------------------------------------------------ */
/*  Curve extraction (port of extractCurves in slug.ts)               */
/*  Uses stb_truetype glyph shapes instead of text-shaper paths.      */
/* ------------------------------------------------------------------ */

static int extract_curves(const stbtt_fontinfo *font, int glyphIndex,
                           SlugQuadCurve **out_curves, SlugBounds *out_bounds)
{
    stbtt_vertex *verts;
    int nv = stbtt_GetGlyphShape(font, glyphIndex, &verts);
    if (nv <= 0) return 0;

    int ix0, iy0, ix1, iy1;
    if (!stbtt_GetGlyphBox(font, glyphIndex, &ix0, &iy0, &ix1, &iy1)) {
        stbtt_FreeShape(font, verts);
        return 0;
    }
    out_bounds->xMin = (float)ix0;
    out_bounds->yMin = (float)iy0;
    out_bounds->xMax = (float)ix1;
    out_bounds->yMax = (float)iy1;

    int cap = nv * 2;
    SlugQuadCurve *curves = (SlugQuadCurve *)malloc(cap * sizeof *curves);
    int n = 0;
    float cx = 0, cy = 0;

    for (int i = 0; i < nv; i++) {
        float x = (float)verts[i].x;
        float y = (float)verts[i].y;

        switch (verts[i].type) {
        case STBTT_vmove:
            cx = x; cy = y;
            break;

        case STBTT_vline: {
            float dx = x - cx, dy = y - cy;
            if (fabsf(dx) < 0.1f && fabsf(dy) < 0.1f) { cx = x; cy = y; break; }
            /* Offset control point perpendicular to the line by a tiny amount
               so the curve is a true (non-degenerate) quadratic. This avoids
               the linear fallback in SolveHorizPoly/SolveVertPoly which can
               produce artifacts on some GPU drivers. */
            float len = sqrtf(dx*dx + dy*dy);
            float nx = -dy / len * 0.05f, ny = dx / len * 0.05f;
            curves[n++] = (SlugQuadCurve){cx, cy,
                                          (cx + x) * .5f + nx, (cy + y) * .5f + ny,
                                          x, y};
            cx = x; cy = y;
            break;
        }
        case STBTT_vcurve:
            curves[n++] = (SlugQuadCurve){cx, cy,
                                          (float)verts[i].cx, (float)verts[i].cy,
                                          x, y};
            cx = x; cy = y;
            break;

        case STBTT_vcubic: {
            /* de Casteljau split at t=0.5 → two quadratics */
            float cx1 = (float)verts[i].cx,  cy1 = (float)verts[i].cy;
            float cx2 = (float)verts[i].cx1, cy2 = (float)verts[i].cy1;

            float m01x = (cx + cx1) * .5f,   m01y = (cy + cy1) * .5f;
            float m12x = (cx1 + cx2) * .5f,  m12y = (cy1 + cy2) * .5f;
            float m23x = (cx2 + x) * .5f,    m23y = (cy2 + y) * .5f;
            float m012x = (m01x + m12x) * .5f, m012y = (m01y + m12y) * .5f;
            float m123x = (m12x + m23x) * .5f, m123y = (m12y + m23y) * .5f;
            float midx  = (m012x + m123x) * .5f, midy = (m012y + m123y) * .5f;

            curves[n++] = (SlugQuadCurve){cx, cy, m01x, m01y, midx, midy};
            curves[n++] = (SlugQuadCurve){midx, midy, m123x, m123y, x, y};
            cx = x; cy = y;
            break;
        }
        }
    }

    stbtt_FreeShape(font, verts);

    /* (normal curves from stb_truetype) */

    *out_curves = curves;
    return n;
}

/* ------------------------------------------------------------------ */
/*  Band building (port of buildBands)                                */
/* ------------------------------------------------------------------ */

static void build_bands(SlugQuadCurve *curves, int curveCount,
                         const SlugBounds *bounds, int bandCount,
                         SlugGlyphBands *out)
{
    float w = bounds->xMax - bounds->xMin;
    float h = bounds->yMax - bounds->yMin;

    out->hBandCount = bandCount;
    out->vBandCount = bandCount;
    out->hBands = (SlugBandEntry *)calloc(bandCount, sizeof(SlugBandEntry));
    out->vBands = (SlugBandEntry *)calloc(bandCount, sizeof(SlugBandEntry));

    for (int i = 0; i < bandCount; i++) {
        out->hBands[i].curveIndices = (int *)malloc(curveCount * sizeof(int));
        out->vBands[i].curveIndices = (int *)malloc(curveCount * sizeof(int));
    }

    /* Precompute the same scale+offset values that the shader uses for
       pixel-to-band mapping (bandTransform).  Using the identical formula
       here avoids floating-point rounding mismatches between the CPU band
       assignment and the GPU pixel lookup, which otherwise cause curves
       to be absent from bands the shader expects them in – producing
       horizontal/vertical line artifacts at band boundaries. */
    float bsX = w > 0 ? (float)bandCount / w : 0;
    float boX = -bounds->xMin * bsX;
    float bsY = h > 0 ? (float)bandCount / h : 0;
    float boY = -bounds->yMin * bsY;

    for (int ci = 0; ci < curveCount; ci++) {
        SlugQuadCurve *c = &curves[ci];
        float cyMin = fminf(fminf(c->p0y, c->p1y), c->p2y);
        float cyMax = fmaxf(fmaxf(c->p0y, c->p1y), c->p2y);
        float cxMin = fminf(fminf(c->p0x, c->p1x), c->p2x);
        float cxMax = fmaxf(fmaxf(c->p0x, c->p1x), c->p2x);

        if (h > 0) {
            int b0 = (int)fminf(bandCount - 1, fmaxf(0, floorf(cyMin * bsY + boY)));
            int b1 = (int)fminf(bandCount - 1, fmaxf(0, floorf(cyMax * bsY + boY)));
            for (int b = b0; b <= b1; b++)
                out->hBands[b].curveIndices[out->hBands[b].count++] = ci;
        }
        if (w > 0) {
            int b0 = (int)fminf(bandCount - 1, fmaxf(0, floorf(cxMin * bsX + boX)));
            int b1 = (int)fminf(bandCount - 1, fmaxf(0, floorf(cxMax * bsX + boX)));
            for (int b = b0; b <= b1; b++)
                out->vBands[b].curveIndices[out->vBands[b].count++] = ci;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Sorting helper – sort band curve indices by descending max coord  */
/* ------------------------------------------------------------------ */

typedef struct { int idx; float key; } SortEntry;

static int cmp_desc(const void *a, const void *b)
{
    float ka = ((const SortEntry *)a)->key;
    float kb = ((const SortEntry *)b)->key;
    return (ka < kb) - (ka > kb);
}

/* axis 0 = sort by max-x,  axis 1 = sort by max-y */
static void sort_band(SlugBandEntry *band, SlugQuadCurve *curves, int axis)
{
    int n = band->count;
    if (n <= 1) return;
    SortEntry *e = (SortEntry *)malloc(n * sizeof *e);
    for (int i = 0; i < n; i++) {
        SlugQuadCurve *c = &curves[band->curveIndices[i]];
        float mx = axis == 0
            ? fmaxf(fmaxf(c->p0x, c->p1x), c->p2x)
            : fmaxf(fmaxf(c->p0y, c->p1y), c->p2y);
        e[i] = (SortEntry){band->curveIndices[i], mx};
    }
    qsort(e, n, sizeof *e, cmp_desc);
    for (int i = 0; i < n; i++) band->curveIndices[i] = e[i].idx;
    free(e);
}

/* ------------------------------------------------------------------ */
/*  Glyph data packing (port of packGlyphData)                       */
/* ------------------------------------------------------------------ */

typedef struct {
    float    *curveTexData;
    uint32_t *bandTexData;
    int       curveTexHeight;
    int       bandTexHeight;
    int      *glyphLocX;
    int      *glyphLocY;
    int      *glyphCurveStarts;
} PackedData;

static PackedData pack_glyph_data(SlugGlyph *glyphs, int gc)
{
    PackedData r = {0};
    const int W = SLUG_TEX_WIDTH;

    /* --- curve texture (RGBA32Float, width W) --- */
    int totalCurveTexels = 0;
    for (int i = 0; i < gc; i++) totalCurveTexels += glyphs[i].curveCount * 2;

    int cH = (totalCurveTexels + W - 1) / W;
    if (cH < 1) cH = 1;
    float *cTex = (float *)calloc((size_t)W * cH * 4, sizeof(float));
    int *curveStarts = (int *)malloc(gc * sizeof(int));
    int cIdx = 0;

    for (int gi = 0; gi < gc; gi++) {
        curveStarts[gi] = cIdx;
        for (int ci = 0; ci < glyphs[gi].curveCount; ci++) {
            SlugQuadCurve *c = &glyphs[gi].curves[ci];
            /* texel 0 */
            int tx0 = cIdx % W, ty0 = cIdx / W;
            int off0 = (ty0 * W + tx0) * 4;
            cTex[off0] = c->p0x; cTex[off0+1] = c->p0y;
            cTex[off0+2] = c->p1x; cTex[off0+3] = c->p1y;
            /* texel 1 */
            int tx1 = (cIdx+1) % W, ty1 = (cIdx+1) / W;
            int off1 = (ty1 * W + tx1) * 4;
            cTex[off1] = c->p2x; cTex[off1+1] = c->p2y;
            cIdx += 2;
        }
    }

    /* --- band texture (RGBA32Uint, width W) --- */
    int totalBand = 0;
    for (int gi = 0; gi < gc; gi++) {
        int hdr = glyphs[gi].bands.hBandCount + glyphs[gi].bands.vBandCount;
        int pad = W - (totalBand % W);
        if (pad < hdr && pad < W) totalBand += pad;
        totalBand += hdr;
        for (int b = 0; b < glyphs[gi].bands.hBandCount; b++)
            totalBand += glyphs[gi].bands.hBands[b].count;
        for (int b = 0; b < glyphs[gi].bands.vBandCount; b++)
            totalBand += glyphs[gi].bands.vBands[b].count;
    }

    int bH = (totalBand + W - 1) / W;
    if (bH < 1) bH = 1;
    uint32_t *bTex = (uint32_t *)calloc((size_t)W * bH * 4, sizeof(uint32_t));
    int *locX = (int *)malloc(gc * sizeof(int));
    int *locY = (int *)malloc(gc * sizeof(int));
    int bIdx = 0;

    for (int gi = 0; gi < gc; gi++) {
        SlugGlyph *g = &glyphs[gi];
        int hbc = g->bands.hBandCount, vbc = g->bands.vBandCount;
        int hdr = hbc + vbc;

        /* avoid header row-wrap */
        int cx = bIdx % W;
        if (cx + hdr > W) bIdx = (bIdx / W + 1) * W;

        locX[gi] = bIdx % W;
        locY[gi] = bIdx / W;
        int glyphStart = bIdx;
        int gcs = curveStarts[gi];

        /* sort */
        for (int b = 0; b < hbc; b++) sort_band(&g->bands.hBands[b], g->curves, 0);
        for (int b = 0; b < vbc; b++) sort_band(&g->bands.vBands[b], g->curves, 1);

        /* offsets */
        int clOff = hdr;
        int *offs = (int *)malloc(hdr * sizeof(int));
        for (int i = 0; i < hbc; i++) { offs[i] = clOff; clOff += g->bands.hBands[i].count; }
        for (int i = 0; i < vbc; i++) { offs[hbc+i] = clOff; clOff += g->bands.vBands[i].count; }

        /* write headers */
        for (int i = 0; i < hdr; i++) {
            int tl = glyphStart + i;
            int di = (tl / W * W + tl % W) * 4;
            int cnt = i < hbc ? g->bands.hBands[i].count
                              : g->bands.vBands[i - hbc].count;
            bTex[di]   = (uint32_t)cnt;
            bTex[di+1] = (uint32_t)offs[i];
        }

        /* write curve lists */
        for (int i = 0; i < hdr; i++) {
            SlugBandEntry *band = i < hbc ? &g->bands.hBands[i]
                                          : &g->bands.vBands[i - hbc];
            int ls = glyphStart + offs[i];
            for (int j = 0; j < band->count; j++) {
                int ct = gcs + band->curveIndices[j] * 2;
                int tl = ls + j;
                int di = (tl / W * W + tl % W) * 4;
                bTex[di]   = (uint32_t)(ct % W);
                bTex[di+1] = (uint32_t)(ct / W);
            }
        }

        free(offs);
        bIdx = glyphStart + clOff;
    }

    r.curveTexData     = cTex;
    r.bandTexData      = bTex;
    r.curveTexHeight   = cH;
    r.bandTexHeight    = bH;
    r.glyphLocX        = locX;
    r.glyphLocY        = locY;
    r.glyphCurveStarts = curveStarts;
    return r;
}

/* ------------------------------------------------------------------ */
/*  Free helpers                                                      */
/* ------------------------------------------------------------------ */

static void free_glyph_bands(SlugGlyphBands *b)
{
    for (int i = 0; i < b->hBandCount; i++) free(b->hBands[i].curveIndices);
    for (int i = 0; i < b->vBandCount; i++) free(b->vBands[i].curveIndices);
    free(b->hBands);
    free(b->vBands);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

SlugTextData slug_prepare_text(const stbtt_fontinfo *font,
                               const char *text, float fontSize)
{
    SlugTextData res = {0};
    int tl = (int)strlen(text);
    float scale = stbtt_ScaleForPixelHeight(font, fontSize);

    /* map codepoints → glyph indices */
    int *gi = (int *)malloc(tl * sizeof(int));
    for (int i = 0; i < tl; i++)
        gi[i] = stbtt_FindGlyphIndex(font, (unsigned char)text[i]);

    /* collect unique glyphs */
    SlugGlyph *sg = (SlugGlyph *)calloc(tl, sizeof(SlugGlyph));
    int sgc = 0;

    for (int i = 0; i < tl; i++) {
        int found = 0;
        for (int j = 0; j < sgc; j++)
            if (sg[j].glyphId == gi[i]) { found = 1; break; }
        if (found) continue;

        SlugQuadCurve *curves;
        SlugBounds bounds;
        int nc = extract_curves(font, gi[i], &curves, &bounds);
        if (nc <= 0) continue;

        SlugGlyphBands bands;
        build_bands(curves, nc, &bounds, SLUG_BAND_COUNT, &bands);
        sg[sgc++] = (SlugGlyph){gi[i], curves, nc, bands, bounds};
    }

    PackedData pk = pack_glyph_data(sg, sgc);

    /* vertex / index buffers  (20 floats = 80 bytes per vertex) */
    float    *verts = (float *)   malloc(tl * 4 * 20 * sizeof(float));
    uint32_t *idxs  = (uint32_t *)malloc(tl * 6 * sizeof(uint32_t));
    int vc = 0, ic = 0, qi = 0;
    float cursorX = 0;

    for (int i = 0; i < tl; i++) {
        int adv, lsb;
        stbtt_GetGlyphHMetrics(font, gi[i], &adv, &lsb);

        /* find slug glyph */
        int si = -1;
        for (int j = 0; j < sgc; j++)
            if (sg[j].glyphId == gi[i]) { si = j; break; }

        if (si < 0) {
            cursorX += adv;
            if (i + 1 < tl)
                cursorX += stbtt_GetGlyphKernAdvance(font, gi[i], gi[i+1]);
            continue;
        }

        SlugGlyph *g = &sg[si];
        float xMin = g->bounds.xMin, yMin = g->bounds.yMin;
        float xMax = g->bounds.xMax, yMax = g->bounds.yMax;
        float w = xMax - xMin, h = yMax - yMin;

        float ox = cursorX * scale;
        float x0 = ox + xMin * scale, y0 = yMin * scale;
        float x1 = ox + xMax * scale, y1 = yMax * scale;

        float bsX = w > 0 ? (float)g->bands.vBandCount / w : 0;
        float bsY = h > 0 ? (float)g->bands.hBandCount / h : 0;
        float boX = -xMin * bsX, boY = -yMin * bsY;

        float glp = pack_u32_as_f32(((uint32_t)pk.glyphLocY[si] << 16)
                                   | (uint32_t)pk.glyphLocX[si]);
        float bmp = pack_u32_as_f32(((uint32_t)(g->bands.hBandCount - 1) << 16)
                                   | (uint32_t)(g->bands.vBandCount - 1));
        float is = 1.0f / scale;

        float corners[4][6] = {
            {x0,y0,-1,-1,xMin,yMin},
            {x1,y0, 1,-1,xMax,yMin},
            {x1,y1, 1, 1,xMax,yMax},
            {x0,y1,-1, 1,xMin,yMax},
        };

        for (int c = 0; c < 4; c++) {
            float *v = &verts[vc * 20];
            v[ 0]=corners[c][0]; v[ 1]=corners[c][1]; v[ 2]=corners[c][2]; v[ 3]=corners[c][3];
            v[ 4]=corners[c][4]; v[ 5]=corners[c][5]; v[ 6]=glp;           v[ 7]=bmp;
            v[ 8]=is;            v[ 9]=0;              v[10]=0;             v[11]=is;
            v[12]=bsX;           v[13]=bsY;            v[14]=boX;           v[15]=boY;
            v[16]=1;             v[17]=1;              v[18]=1;             v[19]=1;
            vc++;
        }

        uint32_t base = qi * 4;
        idxs[ic++]=base; idxs[ic++]=base+1; idxs[ic++]=base+2;
        idxs[ic++]=base; idxs[ic++]=base+2; idxs[ic++]=base+3;

        cursorX += adv;
        if (i + 1 < tl)
            cursorX += stbtt_GetGlyphKernAdvance(font, gi[i], gi[i+1]);
        qi++;
    }

    res.vertices      = verts;
    res.indices        = idxs;
    res.vertexCount    = vc;
    res.indexCount     = ic;
    res.curveTexData   = pk.curveTexData;
    res.bandTexData    = pk.bandTexData;
    res.curveTexHeight = pk.curveTexHeight;
    res.bandTexHeight  = pk.bandTexHeight;
    res.totalAdvance   = cursorX;

    free(pk.glyphLocX);
    free(pk.glyphLocY);
    free(pk.glyphCurveStarts);
    for (int i = 0; i < sgc; i++) { free(sg[i].curves); free_glyph_bands(&sg[i].bands); }
    free(sg);
    free(gi);
    return res;
}

void slug_free_text_data(SlugTextData *d)
{
    free(d->vertices);
    free(d->indices);
    free(d->curveTexData);
    free(d->bandTexData);
    memset(d, 0, sizeof *d);
}
