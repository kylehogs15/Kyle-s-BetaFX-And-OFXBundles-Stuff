/* Color Exchange - pixel math (no dependencies, shared by plugin and test harness) */
#ifndef CE_CORE_H
#define CE_CORE_H

static inline float ce_clamp(float x) { return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }
static inline float ce_abs(float x) { return x < 0.f ? -x : x; }

static void ce_rgb2hsv(float r, float g, float b, float *h, float *s, float *v)
{
    float mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    float mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    float d = mx - mn, hh = 0.f;
    if (d > 1e-6f) {
        if (mx == r)      { hh = (g - b) / d; if (hh < 0.f) hh += 6.f; }
        else if (mx == g) hh = (b - r) / d + 2.f;
        else              hh = (r - g) / d + 4.f;
        hh /= 6.f;
    }
    *h = hh;
    *s = mx > 1e-6f ? d / mx : 0.f;
    *v = mx;
}

static void ce_hsv2rgb(float h, float s, float v, float *r, float *g, float *b)
{
    h = h - (float)(int)h; if (h < 0.f) h += 1.f;
    float x = h * 6.f;
    int i = (int)x;
    float f = x - (float)i;
    float p = v * (1.f - s), q = v * (1.f - s * f), t = v * (1.f - s * (1.f - f));
    switch (i % 6) {
        case 0: *r = v; *g = t; *b = p; break;
        case 1: *r = q; *g = v; *b = p; break;
        case 2: *r = p; *g = v; *b = t; break;
        case 3: *r = p; *g = q; *b = v; break;
        case 4: *r = t; *g = p; *b = v; break;
        default:*r = v; *g = p; *b = q; break;
    }
}

typedef struct {
    float h1, s1, v1;   /* Color 1 (the colour to find)    */
    float h2, s2, v2;   /* Color 2 (the colour to put in)  */
    float tol, soft;    /* tolerance / softness, 0..1      */
} CEParams;

static void ce_prepare(CEParams *p, const float c1[3], const float c2[3], float tol, float soft)
{
    ce_rgb2hsv(c1[0], c1[1], c1[2], &p->h1, &p->s1, &p->v1);
    ce_rgb2hsv(c2[0], c2[1], c2[2], &p->h2, &p->s2, &p->v2);
    p->tol = tol; p->soft = soft;
}

/* Replace every pixel that looks like Color 1 with Color 2.
   Shading / texture of the original pixel is kept, so it still looks natural. */
static void ce_pixel(const CEParams *p, float *r, float *g, float *b)
{
    float h, s, v;
    ce_rgb2hsv(*r, *g, *b, &h, &s, &v);

    float dh = ce_abs(h - p->h1); if (dh > 0.5f) dh = 1.f - dh; dh *= 2.f;
    float hc = s < p->s1 ? s : p->s1;           /* hue only matters for saturated colours */
    float ds = s - p->s1, dv = (v - p->v1) * 0.5f;
    float d2 = (dh * hc) * (dh * hc) + ds * ds + dv * dv;
    float d  = __builtin_sqrtf(d2);

    float w;
    if (d <= p->tol) w = 1.f;
    else if (p->soft <= 1e-5f || d >= p->tol + p->soft) w = 0.f;
    else { float t = (d - p->tol) / p->soft; w = 1.f - t * t * (3.f - 2.f * t); }
    if (w <= 0.f) return;

    float ns = ce_clamp(s + (p->s2 - p->s1));
    float nv = p->v1 > 0.02f ? ce_clamp(v * (p->v2 / p->v1)) : ce_clamp(v + p->v2);
    float nr, ng, nb;
    ce_hsv2rgb(p->h2, ns, nv, &nr, &ng, &nb);
    *r += (nr - *r) * w;
    *g += (ng - *g) * w;
    *b += (nb - *b) * w;
}

#endif
