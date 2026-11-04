/* Color Exchange - OFX plugin for VEGAS Pro (and other OFX hosts)
   Freestanding C: no C runtime, so the .ofx runs on any Windows x64 PC without extra installs. */
#include <stddef.h>
#include "ofxCore.h"
#include "ofxImageEffect.h"
#include "ofxParam.h"
#include "ofxProperty.h"
#include "ofxInteract.h"
#include "ofxPixels.h"
#include "core.h"

/* ---- tiny libc replacements (compiler may emit calls to these) ---- */
__attribute__((optnone)) void *memset(void *d, int c, size_t n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
__attribute__((optnone)) void *memcpy(void *d, const void *s, size_t n) { unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; return d; }
static int streq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

#define PLUGIN_ID "com.colorexchange.vegas.ColorExchange"

static OfxHost *gHost;
static OfxPropertySuiteV1 *gProp;
static OfxImageEffectSuiteV1 *gEff;
static OfxParameterSuiteV1 *gParam;
static OfxInteractSuiteV1 *gInteract;

static OfxStatus mainEntry(const char *action, const void *handle, OfxPropertySetHandle in, OfxPropertySetHandle out);
static OfxStatus overlayEntry(const char *action, const void *handle, OfxPropertySetHandle in, OfxPropertySetHandle out);

/* ================================================================= */
/*  pixel access                                                      */
/* ================================================================= */
typedef struct {
    char *data; int rowBytes; int x1, y1, x2, y2; int depth; /* 0 byte,1 short,2 float */ int premult;
} Img;

static int fetchImgInfo(OfxPropertySetHandle h, Img *im)
{
    char *depthStr = 0, *pm = 0; void *data = 0; int rb = 0, b[4] = {0,0,0,0};
    gProp->propGetPointer(h, kOfxImagePropData, 0, &data);
    gProp->propGetInt(h, kOfxImagePropRowBytes, 0, &rb);
    gProp->propGetIntN(h, kOfxImagePropBounds, 4, b);
    gProp->propGetString(h, kOfxImageEffectPropPixelDepth, 0, &depthStr);
    gProp->propGetString(h, kOfxImageEffectPropPreMultiplication, 0, &pm);
    im->data = (char *)data; im->rowBytes = rb;
    im->x1 = b[0]; im->y1 = b[1]; im->x2 = b[2]; im->y2 = b[3];
    im->depth = 2;
    if (depthStr) { if (streq(depthStr, kOfxBitDepthByte)) im->depth = 0; else if (streq(depthStr, kOfxBitDepthShort)) im->depth = 1; }
    im->premult = pm && streq(pm, kOfxImagePreMultiplied);
    return data != 0;
}

static inline char *pixAddr(const Img *im, int x, int y)
{
    int bpp = im->depth == 0 ? 4 : (im->depth == 1 ? 8 : 16);
    return im->data + (long)(y - im->y1) * im->rowBytes + (long)(x - im->x1) * bpp;
}

static inline void readPix(const Img *im, const char *p, float *c)
{
    if (im->depth == 0)      { const unsigned char *q = (const unsigned char *)p;  for (int i = 0; i < 4; i++) c[i] = q[i] * (1.f / 255.f); }
    else if (im->depth == 1) { const unsigned short *q = (const unsigned short *)p; for (int i = 0; i < 4; i++) c[i] = q[i] * (1.f / 65535.f); }
    else                     { const float *q = (const float *)p;                    for (int i = 0; i < 4; i++) c[i] = q[i]; }
}

static inline void writePix(const Img *im, char *p, const float *c)
{
    if (im->depth == 0)      { unsigned char *q = (unsigned char *)p;  for (int i = 0; i < 4; i++) q[i] = (unsigned char)(ce_clamp(c[i]) * 255.f + .5f); }
    else if (im->depth == 1) { unsigned short *q = (unsigned short *)p; for (int i = 0; i < 4; i++) q[i] = (unsigned short)(ce_clamp(c[i]) * 65535.f + .5f); }
    else                     { float *q = (float *)p;                   for (int i = 0; i < 4; i++) q[i] = c[i]; }
}

/* ================================================================= */
/*  render                                                            */
/* ================================================================= */
static OfxStatus doRender(OfxImageEffectHandle eff, OfxPropertySetHandle in)
{
    OfxTime time = 0; int win[4] = {0,0,0,0};
    gProp->propGetDouble(in, kOfxPropTime, 0, &time);
    gProp->propGetIntN(in, kOfxImageEffectPropRenderWindow, 4, win);

    OfxParamSetHandle ps; gEff->getParamSet(eff, &ps);
    OfxParamHandle p1, p2, pt, pf;
    gParam->paramGetHandle(ps, "color1", &p1, 0);
    gParam->paramGetHandle(ps, "color2", &p2, 0);
    gParam->paramGetHandle(ps, "tolerance", &pt, 0);
    gParam->paramGetHandle(ps, "softness", &pf, 0);
    double a[3], b[3], tol, soft;
    gParam->paramGetValueAtTime(p1, time, &a[0], &a[1], &a[2]);
    gParam->paramGetValueAtTime(p2, time, &b[0], &b[1], &b[2]);
    gParam->paramGetValueAtTime(pt, time, &tol);
    gParam->paramGetValueAtTime(pf, time, &soft);

    float c1[3] = {(float)a[0], (float)a[1], (float)a[2]};
    float c2[3] = {(float)b[0], (float)b[1], (float)b[2]};
    CEParams cp; ce_prepare(&cp, c1, c2, (float)tol, (float)soft);

    OfxImageClipHandle srcClip, dstClip;
    gEff->clipGetHandle(eff, kOfxImageEffectSimpleSourceClipName, &srcClip, 0);
    gEff->clipGetHandle(eff, kOfxImageEffectOutputClipName, &dstClip, 0);
    OfxPropertySetHandle srcH = 0, dstH = 0;
    if (gEff->clipGetImage(srcClip, time, 0, &srcH) != kOfxStatOK) return kOfxStatFailed;
    if (gEff->clipGetImage(dstClip, time, 0, &dstH) != kOfxStatOK) { gEff->clipReleaseImage(srcH); return kOfxStatFailed; }

    Img src, dst;
    if (!fetchImgInfo(srcH, &src) || !fetchImgInfo(dstH, &dst)) {
        gEff->clipReleaseImage(srcH); gEff->clipReleaseImage(dstH); return kOfxStatFailed;
    }

    for (int y = win[1]; y < win[3]; y++) {
        for (int x = win[0]; x < win[2]; x++) {
            float c[4] = {0, 0, 0, 0};
            if (x >= src.x1 && x < src.x2 && y >= src.y1 && y < src.y2) readPix(&src, pixAddr(&src, x, y), c);
            float al = c[3];
            if (src.premult && al > 1e-6f) { c[0] /= al; c[1] /= al; c[2] /= al; }
            ce_pixel(&cp, &c[0], &c[1], &c[2]);
            if (src.premult) { c[0] *= al; c[1] *= al; c[2] *= al; }
            if (x >= dst.x1 && x < dst.x2 && y >= dst.y1 && y < dst.y2) writePix(&dst, pixAddr(&dst, x, y), c);
        }
    }
    gEff->clipReleaseImage(srcH);
    gEff->clipReleaseImage(dstH);
    return kOfxStatOK;
}

/* ================================================================= */
/*  describe                                                          */
/* ================================================================= */
static OfxStatus describe(OfxImageEffectHandle eff)
{
    OfxPropertySetHandle p; gEff->getPropertySet(eff, &p);
    gProp->propSetString(p, kOfxPropLabel, 0, "Color Exchange");
    gProp->propSetString(p, kOfxPropShortLabel, 0, "Color Exchange");
    gProp->propSetString(p, kOfxImageEffectPluginPropGrouping, 0, "Color");
    gProp->propSetString(p, kOfxPropPluginDescription, 0, "Swap Color 1 for Color 2. Pick Color 1 from the video, pick the colour to replace it with in Color 2.");
    gProp->propSetString(p, kOfxImageEffectPropSupportedContexts, 0, kOfxImageEffectContextFilter);
    gProp->propSetString(p, kOfxImageEffectPropSupportedContexts, 1, kOfxImageEffectContextGeneral);
    gProp->propSetString(p, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthByte);
    gProp->propSetString(p, kOfxImageEffectPropSupportedPixelDepths, 1, kOfxBitDepthShort);
    gProp->propSetString(p, kOfxImageEffectPropSupportedPixelDepths, 2, kOfxBitDepthFloat);
    gProp->propSetInt(p, kOfxImageEffectPluginPropSingleInstance, 0, 0);
    gProp->propSetString(p, kOfxImageEffectPluginRenderThreadSafety, 0, kOfxImageEffectRenderFullySafe);
    gProp->propSetInt(p, kOfxImageEffectPluginPropHostFrameThreading, 0, 1);
    gProp->propSetInt(p, kOfxImageEffectPropSupportsMultipleClipDepths, 0, 0);
    gProp->propSetInt(p, kOfxImageEffectPropSupportsTiles, 0, 1);
    gProp->propSetPointer(p, kOfxImageEffectPluginPropOverlayInteractV1, 0, (void *)overlayEntry);
    return kOfxStatOK;
}

static void rgbParam(OfxParamSetHandle ps, const char *name, const char *label, double r, double g, double b, const char *hint)
{
    OfxPropertySetHandle pp;
    gParam->paramDefine(ps, kOfxParamTypeRGB, name, &pp);
    gProp->propSetString(pp, kOfxPropLabel, 0, label);
    gProp->propSetString(pp, kOfxParamPropHint, 0, hint);
    gProp->propSetDouble(pp, kOfxParamPropDefault, 0, r);
    gProp->propSetDouble(pp, kOfxParamPropDefault, 1, g);
    gProp->propSetDouble(pp, kOfxParamPropDefault, 2, b);
    gProp->propSetInt(pp, kOfxParamPropAnimates, 0, 1);
}

static void dblParam(OfxParamSetHandle ps, const char *name, const char *label, double def, const char *hint)
{
    OfxPropertySetHandle pp;
    gParam->paramDefine(ps, kOfxParamTypeDouble, name, &pp);
    gProp->propSetString(pp, kOfxPropLabel, 0, label);
    gProp->propSetString(pp, kOfxParamPropHint, 0, hint);
    gProp->propSetDouble(pp, kOfxParamPropDefault, 0, def);
    gProp->propSetDouble(pp, kOfxParamPropMin, 0, 0.0);
    gProp->propSetDouble(pp, kOfxParamPropMax, 0, 1.0);
    gProp->propSetDouble(pp, kOfxParamPropDisplayMin, 0, 0.0);
    gProp->propSetDouble(pp, kOfxParamPropDisplayMax, 0, 1.0);
    gProp->propSetDouble(pp, kOfxParamPropIncrement, 0, 0.01);
    gProp->propSetInt(pp, kOfxParamPropAnimates, 0, 1);
}

static OfxStatus describeInContext(OfxImageEffectHandle eff)
{
    OfxPropertySetHandle cp;
    gEff->clipDefine(eff, kOfxImageEffectOutputClipName, &cp);
    gProp->propSetString(cp, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
    gEff->clipDefine(eff, kOfxImageEffectSimpleSourceClipName, &cp);
    gProp->propSetString(cp, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);

    OfxParamSetHandle ps; gEff->getParamSet(eff, &ps);
    rgbParam(ps, "color1", "Color 1", 1.0, 0.9, 0.0, "The colour in the video to change (yellow by default). Click the colour box to pick one.");
    rgbParam(ps, "color2", "Color 2", 1.0, 0.0, 0.0, "The colour to change it into (red by default). Click the colour box to pick one.");
    {
        OfxPropertySetHandle pp;
        gParam->paramDefine(ps, kOfxParamTypeDouble2D, "pickPoint", &pp);
        gProp->propSetString(pp, kOfxPropLabel, 0, "Eyedropper Point");
        gProp->propSetString(pp, kOfxParamPropHint, 0, "Aim this at the spot in the video you want to pick (0,0 = bottom-left, 1,1 = top-right).");
        gProp->propSetString(pp, kOfxParamPropDoubleType, 0, kOfxParamDoubleTypeXYAbsolute);
        gProp->propSetString(pp, kOfxParamPropDefaultCoordinateSystem, 0, kOfxParamCoordinatesNormalised);
        gProp->propSetDouble(pp, kOfxParamPropDefault, 0, 0.5);
        gProp->propSetDouble(pp, kOfxParamPropDefault, 1, 0.5);
        gProp->propSetDouble(pp, kOfxParamPropMin, 0, 0.0); gProp->propSetDouble(pp, kOfxParamPropMin, 1, 0.0);
        gProp->propSetDouble(pp, kOfxParamPropMax, 0, 1.0); gProp->propSetDouble(pp, kOfxParamPropMax, 1, 1.0);
        gProp->propSetDouble(pp, kOfxParamPropDisplayMin, 0, 0.0); gProp->propSetDouble(pp, kOfxParamPropDisplayMin, 1, 0.0);
        gProp->propSetDouble(pp, kOfxParamPropDisplayMax, 0, 1.0); gProp->propSetDouble(pp, kOfxParamPropDisplayMax, 1, 1.0);
        gProp->propSetInt(pp, kOfxParamPropAnimates, 0, 0);
        gParam->paramDefine(ps, kOfxParamTypePushButton, "pickBtn", &pp);
        gProp->propSetString(pp, kOfxPropLabel, 0, "Pick Color 1 (Eyedropper)");
        gProp->propSetString(pp, kOfxParamPropHint, 0, "Copies the colour under the Eyedropper Point into Color 1.");
    }
    dblParam(ps, "tolerance", "Tolerance", 0.25, "How different from Color 1 a pixel can be and still be changed.");
    dblParam(ps, "softness", "Softness", 0.15, "Feather the edge of the change.");
    return kOfxStatOK;
}

/* ================================================================= */
/*  overlay: click in the viewer to pick Color 1 (used if host supports overlays) */
/* ================================================================= */
static OfxStatus overlayPenDown(OfxImageEffectHandle eff, OfxPropertySetHandle in)
{
    double t = 0, pos[2] = {0, 0}, rs[2] = {1, 1};
    gProp->propGetDouble(in, kOfxPropTime, 0, &t);
    gProp->propGetDoubleN(in, kOfxInteractPropPenPosition, 2, pos);
    gProp->propGetDoubleN(in, kOfxImageEffectPropRenderScale, 2, rs);

    OfxImageClipHandle clip; OfxPropertySetHandle h = 0;
    if (gEff->clipGetHandle(eff, kOfxImageEffectSimpleSourceClipName, &clip, 0) != kOfxStatOK) return kOfxStatFailed;
    if (gEff->clipGetImage(clip, t, 0, &h) != kOfxStatOK) return kOfxStatFailed;
    Img im;
    if (!fetchImgInfo(h, &im)) { gEff->clipReleaseImage(h); return kOfxStatFailed; }
    int x = (int)(pos[0] * rs[0]), y = (int)(pos[1] * rs[1]);
    if (x < im.x1 || x >= im.x2 || y < im.y1 || y >= im.y2) { gEff->clipReleaseImage(h); return kOfxStatReplyDefault; }
    float c[4]; readPix(&im, pixAddr(&im, x, y), c);
    if (im.premult && c[3] > 1e-6f) { c[0] /= c[3]; c[1] /= c[3]; c[2] /= c[3]; }
    gEff->clipReleaseImage(h);

    OfxParamSetHandle ps; gEff->getParamSet(eff, &ps);
    OfxParamHandle p1;
    gParam->paramGetHandle(ps, "color1", &p1, 0);
    gParam->paramSetValue(p1, (double)ce_clamp(c[0]), (double)ce_clamp(c[1]), (double)ce_clamp(c[2]));
    return kOfxStatOK;
}

/* average a 5x5 block around (x,y) so video noise doesn't give a wrong pick */
static int sampleAvg(const Img *im, int x, int y, float *rgb)
{
    float sum[3] = {0,0,0}; int n = 0;
    for (int j = -2; j <= 2; j++) for (int i = -2; i <= 2; i++) {
        int xx = x + i, yy = y + j;
        if (xx < im->x1 || xx >= im->x2 || yy < im->y1 || yy >= im->y2) continue;
        float c[4]; readPix(im, pixAddr(im, xx, yy), c);
        if (im->premult && c[3] > 1e-6f) { c[0] /= c[3]; c[1] /= c[3]; c[2] /= c[3]; }
        sum[0] += c[0]; sum[1] += c[1]; sum[2] += c[2]; n++;
    }
    if (!n) return 0;
    for (int k = 0; k < 3; k++) rgb[k] = ce_clamp(sum[k] / n);
    return 1;
}

static OfxStatus pickFromPoint(OfxImageEffectHandle eff, OfxTime t)
{
    OfxParamSetHandle ps; gEff->getParamSet(eff, &ps);
    OfxParamHandle pp, p1;
    gParam->paramGetHandle(ps, "pickPoint", &pp, 0);
    gParam->paramGetHandle(ps, "color1", &p1, 0);
    double nx = 0.5, ny = 0.5;
    gParam->paramGetValueAtTime(pp, t, &nx, &ny);

    OfxImageClipHandle clip; OfxPropertySetHandle h = 0;
    if (gEff->clipGetHandle(eff, kOfxImageEffectSimpleSourceClipName, &clip, 0) != kOfxStatOK) return kOfxStatFailed;
    if (gEff->clipGetImage(clip, t, 0, &h) != kOfxStatOK) return kOfxStatFailed;
    Img im; float rgb[3];
    int ok = fetchImgInfo(h, &im);
    if (ok) {
        int x = im.x1 + (int)(nx * (im.x2 - im.x1)), y = im.y1 + (int)(ny * (im.y2 - im.y1));
        if (x >= im.x2) x = im.x2 - 1;
        if (y >= im.y2) y = im.y2 - 1;
        ok = sampleAvg(&im, x, y, rgb);
    }
    gEff->clipReleaseImage(h);
    if (!ok) return kOfxStatFailed;
    gParam->paramSetValue(p1, (double)rgb[0], (double)rgb[1], (double)rgb[2]);
    return kOfxStatOK;
}

static OfxStatus instanceChanged(OfxImageEffectHandle eff, OfxPropertySetHandle in)
{
    char *type = 0, *name = 0; double t = 0;
    gProp->propGetString(in, kOfxPropType, 0, &type);
    gProp->propGetString(in, kOfxPropName, 0, &name);
    gProp->propGetDouble(in, kOfxPropTime, 0, &t);
    if (type && name && streq(type, kOfxTypeParameter) && streq(name, "pickBtn")) return pickFromPoint(eff, t);
    return kOfxStatReplyDefault;
}

static OfxStatus overlayEntry(const char *action, const void *handle, OfxPropertySetHandle in, OfxPropertySetHandle out)
{
    (void)out;
    if (streq(action, kOfxInteractActionPenDown)) {
        OfxInteractHandle ih = (OfxInteractHandle)handle;
        OfxPropertySetHandle ip; void *effp = 0;
        if (!gInteract || gInteract->interactGetPropertySet(ih, &ip) != kOfxStatOK) return kOfxStatReplyDefault;
        gProp->propGetPointer(ip, kOfxPropEffectInstance, 0, &effp);
        if (!effp) return kOfxStatReplyDefault;
        return overlayPenDown((OfxImageEffectHandle)effp, in) == kOfxStatOK ? kOfxStatOK : kOfxStatReplyDefault;
    }
    return kOfxStatReplyDefault;
}

/* ================================================================= */
/*  main entry                                                        */
/* ================================================================= */
static OfxStatus mainEntry(const char *action, const void *handle, OfxPropertySetHandle in, OfxPropertySetHandle out)
{
    (void)out;
    OfxImageEffectHandle eff = (OfxImageEffectHandle)handle;
    if (streq(action, kOfxActionLoad)) {
        if (!gHost) return kOfxStatErrMissingHostFeature;
        gProp   = (OfxPropertySuiteV1 *)gHost->fetchSuite(gHost->host, kOfxPropertySuite, 1);
        gEff    = (OfxImageEffectSuiteV1 *)gHost->fetchSuite(gHost->host, kOfxImageEffectSuite, 1);
        gParam  = (OfxParameterSuiteV1 *)gHost->fetchSuite(gHost->host, kOfxParameterSuite, 1);
        gInteract = (OfxInteractSuiteV1 *)gHost->fetchSuite(gHost->host, kOfxInteractSuite, 1);
        return (gProp && gEff && gParam) ? kOfxStatOK : kOfxStatErrMissingHostFeature;
    }
    if (streq(action, kOfxActionUnload))          return kOfxStatOK;
    if (streq(action, kOfxActionDescribe))        return describe(eff);
    if (streq(action, kOfxImageEffectActionDescribeInContext)) return describeInContext(eff);
    if (streq(action, kOfxActionCreateInstance) || streq(action, kOfxActionDestroyInstance) ||
        streq(action, kOfxImageEffectActionBeginSequenceRender) || streq(action, kOfxImageEffectActionEndSequenceRender))
        return kOfxStatOK;
    if (streq(action, kOfxActionInstanceChanged)) return instanceChanged(eff, in);
    if (streq(action, kOfxImageEffectActionRender)) return doRender(eff, in);
    return kOfxStatReplyDefault;
}

int _fltused = 0;
static void setHost(OfxHost *h) { gHost = h; }

static OfxPlugin gPlugin = {
    kOfxImageEffectPluginApi, 1, PLUGIN_ID, 1, 0, setHost, mainEntry
};

OfxExport int OfxGetNumberOfPlugins(void) { return 1; }
OfxExport OfxPlugin *OfxGetPlugin(int nth) { return nth == 0 ? &gPlugin : 0; }
