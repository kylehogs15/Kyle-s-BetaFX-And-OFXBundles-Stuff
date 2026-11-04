/*
 * Color Correction - OFX image effect plug-in for VEGAS Pro
 *
 * Six parameters, default 0 (Hue -180..180 degrees, the other five -100..100):
 *   Brightness, Temperature, Contrast, Saturation, Gamma, Hue
 *
 * Temperature is fitted exactly to the reference pictures: the two
 * reference pictures are Temperature +100 (warm purple) and -100 (blue); they differ by
 * R60 G10 B40, so each end is half of that: -100 = R-30 G-5 B+20 (blue),
 * +100 = R+30 G+5 B-20 (warm).  Linear in the slider, 0 = no change.
 *
 * Freestanding C (no C runtime) so it cross-compiles with clang + lld-link.
 */
#include <stddef.h>
#include "ofxCore.h"
#include "ofxProperty.h"
#include "ofxParam.h"
#include "ofxImageEffect.h"

#if defined(_WIN32)
#define OFX_EXPORT __declspec(dllexport)
#else
#define OFX_EXPORT __attribute__((visibility("default")))
#endif

#define PLUGIN_ID    "com.kylehogs.ColorCorrection"
#define PLUGIN_LABEL "Color Correction"

/* ------------------------------------------------------------------ */
/* tiny math library (accurate to ~1e-6, plenty for 8-bit / float video) */
/* ------------------------------------------------------------------ */
static float cc_log2f(float x)            /* x > 0 */
{
    union { float f; unsigned u; } v;
    v.f = x;
    int e = (int)((v.u >> 23) & 255u) - 127;
    v.u = (v.u & 0x007fffffu) | 0x3f800000u;      /* mantissa in [1,2) */
    float m = v.f;
    float t = (m - 1.0f) / (m + 1.0f);
    float t2 = t * t;
    float s = t * (1.0f + t2 * (1.0f / 3 + t2 * (1.0f / 5 + t2 * (1.0f / 7 + t2 * (1.0f / 9)))));
    return (float)e + s * 2.8853900817779268f;    /* 2/ln2 */
}

static float cc_exp2f(float x)
{
    if (x < -120.0f) return 0.0f;
    if (x > 120.0f) x = 120.0f;
    float r = x + (x >= 0 ? 0.5f : -0.5f);
    int i = (int)r;                               /* nearest integer */
    float f = (x - (float)i) * 0.6931471805599453f;
    float p = 1.0f + f * (1.0f + f * (0.5f + f * (1.0f / 6 + f * (1.0f / 24 + f * (1.0f / 120 + f * (1.0f / 720))))));
    union { float f; unsigned u; } v;
    v.u = (unsigned)(i + 127) << 23;
    return p * v.f;
}

static float cc_powf(float x, float e)
{
    if (x <= 0.0f) return 0.0f;
    return cc_exp2f(e * cc_log2f(x));
}

static float clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

static float fabs1(float x) { return x < 0.0f ? -x : x; }

static void rgb2hsl(float r, float g, float b, float *h, float *s, float *l)
{
    float mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    float mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    float d = mx - mn, L = 0.5f * (mx + mn), den = 1.0f - fabs1(2.0f * L - 1.0f);
    *l = L;
    if (d <= 1e-6f || den <= 1e-6f) { *h = 0.0f; *s = 0.0f; return; }
    *s = clamp01(d / den);
    if (mx == r)      { float t = (g - b) / d; *h = t < 0.0f ? t + 6.0f : t; }
    else if (mx == g) *h = (b - r) / d + 2.0f;
    else              *h = (r - g) / d + 4.0f;
}

static void hsl2rgb(float h, float s, float l, float *r, float *g, float *b)
{
    float c = (1.0f - fabs1(2.0f * l - 1.0f)) * s, m = l - 0.5f * c;
    int i = (int)h; if (i > 5) i = 5;
    float f = h - (float)i;                          /* position inside the sextant */
    float x = (i & 1) ? c * (1.0f - f) : c * f;
    float R, G, B;
    switch (i) {
    case 0:  R = c; G = x; B = 0; break;
    case 1:  R = x; G = c; B = 0; break;
    case 2:  R = 0; G = c; B = x; break;
    case 3:  R = 0; G = x; B = c; break;
    case 4:  R = x; G = 0; B = c; break;
    default: R = c; G = 0; B = x; break;
    }
    *r = clamp01(R + m); *g = clamp01(G + m); *b = clamp01(B + m);
}

/* ------------------------------------------------------------------ */
/* the colour math                                                      */
/* ------------------------------------------------------------------ */
typedef struct {
    float bright;                 /* additive, 0..1 scale        */
    float toff[3];                /* temperature offsets (R,G,B) */
    float cgain;                  /* contrast gain around 0.5    */
    float sat;                    /* HSL saturation multiplier   */
    float ginv;                   /* gamma exponent              */
    float hueShift;               /* hue rotation in sextants (6 = 360 deg) */
    int   identity;
} CCParams;

void cc_setup(CCParams *p, double brightness, double temperature, double contrast,
              double saturation, double gamma, double hue)
{
    float b = (float)brightness / 100.0f, t = (float)temperature / 100.0f;
    float c = (float)contrast / 100.0f, s = (float)saturation / 100.0f;
    float g = (float)gamma / 100.0f;

    p->bright  = b * (102.0f / 255.0f);            /* +100 -> +102/255 (from the recording) */
    p->toff[0] =  t * (30.0f / 255.0f);            /* -100 -> R-30, +100 -> R+30 */
    p->toff[1] =  t * (5.0f / 255.0f);             /*         G-5 / G+5 */
    p->toff[2] = -t * (20.0f / 255.0f);            /*         B+20 / B-20 */
    p->cgain   = cc_exp2f(c >= 0.0f ? 4.0f * c : c);   /* -100 -> 0.5x, +100 -> 16x (hard threshold at mid grey) */
    p->sat     = 1.0f + s;                         /* -100 -> grey, +100 -> 2x */
    p->ginv    = cc_exp2f(g >= 0.0f ? -4.0f * g : -g); /* +100 -> x^(1/16) (very bright), -100 -> x^2 (darker) */
    p->hueShift = (float)hue / 60.0f;            /* Hue is in degrees: -180..180 (60 degrees = 1 sextant) */
    p->identity = (brightness == 0 && temperature == 0 && contrast == 0 &&
                   saturation == 0 && gamma == 0 && hue == 0);
}

/* clip=1 (8-bit): clamp after every step.  clip=0 (float): keep out-of-range values so
   the result matches the reference exactly, even on pixels beyond 0..1 in the source. */
void cc_pixel(const CCParams *p, float *r, float *g, float *b, int clip)
{
    float R = *r, G = *g, B = *b;

    /* brightness + temperature (plain additive shifts) */
    R += p->bright + p->toff[0];
    G += p->bright + p->toff[1];
    B += p->bright + p->toff[2];
    if (clip) { R = clamp01(R); G = clamp01(G); B = clamp01(B); }

    /* contrast around mid grey */
    R = (R - 0.5f) * p->cgain + 0.5f;
    G = (G - 0.5f) * p->cgain + 0.5f;
    B = (B - 0.5f) * p->cgain + 0.5f;
    if (clip || p->cgain != 1.0f) { R = clamp01(R); G = clamp01(G); B = clamp01(B); }

    /* saturation + hue are done in HSL (fitted to the reference recording:
       Saturation -100 = HSL lightness grey, Hue is in degrees, -180..180) */
    if (p->sat != 1.0f || p->hueShift != 0.0f || p->ginv != 1.0f) { R = clamp01(R); G = clamp01(G); B = clamp01(B); }
    if (p->sat != 1.0f || p->hueShift != 0.0f) {
        float h, s, l;
        rgb2hsl(R, G, B, &h, &s, &l);
        s = clamp01(s * p->sat);
        h += p->hueShift;
        h -= 6.0f * (float)(int)(h / 6.0f);
        if (h < 0.0f) h += 6.0f;
        hsl2rgb(h, s, l, &R, &G, &B);
    }

    /* gamma */
    if (p->ginv != 1.0f) {
        R = cc_powf(R, p->ginv);
        G = cc_powf(G, p->ginv);
        B = cc_powf(B, p->ginv);
    }
    *r = R; *g = G; *b = B;
}

/* ------------------------------------------------------------------ */
/* OFX plumbing                                                         */
/* ------------------------------------------------------------------ */
static OfxHost *gHost;
static OfxPropertySuiteV1 *gProp;
static OfxParameterSuiteV1 *gParam;
static OfxImageEffectSuiteV1 *gEffect;

static const char *kParamNames[6]  = {"brightness", "temperature", "contrast", "saturation", "gamma", "hue"};
static const char *kParamLabels[6] = {"Brightness", "Temperature", "Contrast", "Saturation", "Gamma", "Hue"};
static const char *kParamHints[6]  = {
    "Brightness, -100 to 100",
    "Colour temperature, -100 (cool/blue) to 100 (warm/orange)",
    "Contrast, -100 to 100",
    "Saturation, -100 (greyscale) to 100",
    "Gamma, -100 (darker) to 100 (lighter)",
    "Hue rotation in degrees, -180 to 180"};

static int cc_streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static OfxStatus onLoad(void)
{
    if (!gHost) return kOfxStatErrMissingHostFeature;
    gProp   = (OfxPropertySuiteV1 *)gHost->fetchSuite(gHost->host, kOfxPropertySuite, 1);
    gParam  = (OfxParameterSuiteV1 *)gHost->fetchSuite(gHost->host, kOfxParameterSuite, 1);
    gEffect = (OfxImageEffectSuiteV1 *)gHost->fetchSuite(gHost->host, kOfxImageEffectSuite, 1);
    if (!gProp || !gParam || !gEffect) return kOfxStatErrMissingHostFeature;
    return kOfxStatOK;
}

static OfxStatus onDescribe(OfxImageEffectHandle effect)
{
    OfxPropertySetHandle props;
    gEffect->getPropertySet(effect, &props);
    gProp->propSetString(props, kOfxPropLabel, 0, PLUGIN_LABEL);
    gProp->propSetString(props, kOfxImageEffectPluginPropGrouping, 0, "Color Correction");
    gProp->propSetString(props, kOfxImageEffectPropSupportedContexts, 0, kOfxImageEffectContextFilter);
    gProp->propSetString(props, kOfxImageEffectPropSupportedContexts, 1, kOfxImageEffectContextGeneral);
    gProp->propSetString(props, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthFloat);
    gProp->propSetString(props, kOfxImageEffectPropSupportedPixelDepths, 1, kOfxBitDepthByte);
    gProp->propSetInt(props, kOfxImageEffectPluginPropSingleInstance, 0, 0);
    gProp->propSetString(props, kOfxImageEffectPluginRenderThreadSafety, 0, kOfxImageEffectRenderFullySafe);
    gProp->propSetInt(props, kOfxImageEffectPropSupportsTiles, 0, 0);
    gProp->propSetInt(props, kOfxImageEffectPropSupportsMultiResolution, 0, 0);
    gProp->propSetInt(props, kOfxImageEffectPropSupportsMultipleClipDepths, 0, 0);
    return kOfxStatOK;
}

static OfxStatus onDescribeInContext(OfxImageEffectHandle effect)
{
    OfxPropertySetHandle cp;
    OfxParamSetHandle ps;
    int i;

    gEffect->clipDefine(effect, kOfxImageEffectOutputClipName, &cp);
    gProp->propSetString(cp, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
    gEffect->clipDefine(effect, kOfxImageEffectSimpleSourceClipName, &cp);
    gProp->propSetString(cp, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);

    gEffect->getParamSet(effect, &ps);
    for (i = 0; i < 6; i++) {
        OfxPropertySetHandle pp;
        if (gParam->paramDefine(ps, kOfxParamTypeDouble, kParamNames[i], &pp) != kOfxStatOK) continue;
        gProp->propSetString(pp, kOfxPropLabel, 0, kParamLabels[i]);
        gProp->propSetString(pp, kOfxParamPropHint, 0, kParamHints[i]);
        gProp->propSetString(pp, kOfxParamPropDoubleType, 0, kOfxParamDoubleTypePlain);
        gProp->propSetDouble(pp, kOfxParamPropDefault, 0, 0.0);
        {
            double lim = (i == 5) ? 180.0 : 100.0;      /* Hue: -180..180, everything else -100..100 */
            gProp->propSetDouble(pp, kOfxParamPropMin, 0, -lim);
            gProp->propSetDouble(pp, kOfxParamPropMax, 0, lim);
            gProp->propSetDouble(pp, kOfxParamPropDisplayMin, 0, -lim);
            gProp->propSetDouble(pp, kOfxParamPropDisplayMax, 0, lim);
        }
        gProp->propSetDouble(pp, kOfxParamPropIncrement, 0, 1.0);
        gProp->propSetInt(pp, kOfxParamPropDigits, 0, 0);
        gProp->propSetInt(pp, kOfxParamPropAnimates, 0, 1);
    }
    return kOfxStatOK;
}

static OfxStatus onRender(OfxImageEffectHandle effect, OfxPropertySetHandle inArgs)
{
    double time = 0, v[6];
    int win[4] = {0, 0, 0, 0}, i, x, y;
    OfxParamSetHandle ps;
    OfxImageClipHandle srcClip = 0, dstClip = 0;
    OfxPropertySetHandle src = 0, dst = 0;
    OfxStatus st = kOfxStatFailed;
    CCParams cp;

    gProp->propGetDouble(inArgs, kOfxPropTime, 0, &time);
    gProp->propGetIntN(inArgs, kOfxImageEffectPropRenderWindow, 4, win);

    gEffect->getParamSet(effect, &ps);
    for (i = 0; i < 6; i++) {
        OfxParamHandle h;
        v[i] = 0.0;
        if (gParam->paramGetHandle(ps, kParamNames[i], &h, 0) == kOfxStatOK)
            gParam->paramGetValueAtTime(h, time, &v[i]);
        {
            double lim = (i == 5) ? 180.0 : 100.0;
            if (v[i] < -lim) v[i] = -lim;
            if (v[i] > lim) v[i] = lim;
        }
    }
    cc_setup(&cp, v[0], v[1], v[2], v[3], v[4], v[5]);

    gEffect->clipGetHandle(effect, kOfxImageEffectSimpleSourceClipName, &srcClip, 0);
    gEffect->clipGetHandle(effect, kOfxImageEffectOutputClipName, &dstClip, 0);
    if (gEffect->clipGetImage(srcClip, time, 0, &src) != kOfxStatOK || !src) goto done;
    if (gEffect->clipGetImage(dstClip, time, 0, &dst) != kOfxStatOK || !dst) goto done;

    {
        char *sDepth = 0, *dDepth = 0, *sComp = 0, *dComp = 0;
        void *sData = 0, *dData = 0;
        int sRow = 0, dRow = 0, sB[4] = {0, 0, 0, 0}, dB[4] = {0, 0, 0, 0};
        int isFloat;

        gProp->propGetString(src, kOfxImageEffectPropPixelDepth, 0, &sDepth);
        gProp->propGetString(dst, kOfxImageEffectPropPixelDepth, 0, &dDepth);
        gProp->propGetString(src, kOfxImageEffectPropComponents, 0, &sComp);
        gProp->propGetString(dst, kOfxImageEffectPropComponents, 0, &dComp);
        gProp->propGetPointer(src, kOfxImagePropData, 0, &sData);
        gProp->propGetPointer(dst, kOfxImagePropData, 0, &dData);
        gProp->propGetInt(src, kOfxImagePropRowBytes, 0, &sRow);
        gProp->propGetInt(dst, kOfxImagePropRowBytes, 0, &dRow);
        gProp->propGetIntN(src, kOfxImagePropBounds, 4, sB);
        gProp->propGetIntN(dst, kOfxImagePropBounds, 4, dB);

        if (!sDepth || !dDepth || !sComp || !dComp || !sData || !dData) goto done;
        if (!cc_streq(sDepth, dDepth) || !cc_streq(sComp, kOfxImageComponentRGBA) ||
            !cc_streq(dComp, kOfxImageComponentRGBA)) { st = kOfxStatErrImageFormat; goto done; }
        if (cc_streq(sDepth, kOfxBitDepthFloat)) isFloat = 1;
        else if (cc_streq(sDepth, kOfxBitDepthByte)) isFloat = 0;
        else { st = kOfxStatErrImageFormat; goto done; }

        for (y = win[1]; y < win[3]; y++) {
            if (y < sB[1] || y >= sB[3] || y < dB[1] || y >= dB[3]) continue;
            for (x = win[0]; x < win[2]; x++) {
                if (x < sB[0] || x >= sB[2] || x < dB[0] || x >= dB[2]) continue;
                if (isFloat) {
                    const float *sp = (const float *)((const char *)sData + (long)(y - sB[1]) * sRow) + (long)(x - sB[0]) * 4;
                    float *dp = (float *)((char *)dData + (long)(y - dB[1]) * dRow) + (long)(x - dB[0]) * 4;
                    float r = sp[0], g = sp[1], b = sp[2];
                    if (!cp.identity) cc_pixel(&cp, &r, &g, &b, 0);
                    dp[0] = r; dp[1] = g; dp[2] = b; dp[3] = sp[3];
                } else {
                    const unsigned char *sp = (const unsigned char *)sData + (long)(y - sB[1]) * sRow + (long)(x - sB[0]) * 4;
                    unsigned char *dp = (unsigned char *)dData + (long)(y - dB[1]) * dRow + (long)(x - dB[0]) * 4;
                    if (cp.identity) {
                        dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2];
                    } else {
                        float r = sp[0] * (1.0f / 255.0f), g = sp[1] * (1.0f / 255.0f), b = sp[2] * (1.0f / 255.0f);
                        cc_pixel(&cp, &r, &g, &b, 1);
                        dp[0] = (unsigned char)(r * 255.0f + 0.5f);
                        dp[1] = (unsigned char)(g * 255.0f + 0.5f);
                        dp[2] = (unsigned char)(b * 255.0f + 0.5f);
                    }
                    dp[3] = sp[3];
                }
            }
        }
        st = kOfxStatOK;
    }
done:
    if (src) gEffect->clipReleaseImage(src);
    if (dst) gEffect->clipReleaseImage(dst);
    return st;
}

static OfxStatus mainEntry(const char *action, const void *handle, OfxPropertySetHandle inArgs, OfxPropertySetHandle outArgs)
{
    (void)outArgs;
    if (cc_streq(action, kOfxActionLoad)) return onLoad();
    if (!gEffect) return kOfxStatErrMissingHostFeature;
    if (cc_streq(action, kOfxActionDescribe)) return onDescribe((OfxImageEffectHandle)handle);
    if (cc_streq(action, kOfxImageEffectActionDescribeInContext)) return onDescribeInContext((OfxImageEffectHandle)handle);
    if (cc_streq(action, kOfxImageEffectActionRender)) return onRender((OfxImageEffectHandle)handle, inArgs);
    return kOfxStatReplyDefault;
}

static void setHost(OfxHost *host) { gHost = host; }

static OfxPlugin gPlugin = {
    kOfxImageEffectPluginApi, 1, PLUGIN_ID, 1, 0, setHost, mainEntry
};

OFX_EXPORT int OfxGetNumberOfPlugins(void) { return 1; }

OFX_EXPORT OfxPlugin *OfxGetPlugin(int nth) { return nth == 0 ? &gPlugin : 0; }
