/* macOS presenter: the window's CAMetalLayer (SDL_Metal_CreateView).
 * The 8-bit screen and its palette are uploaded as two textures and the
 * palette lookup happens in the fragment shader, which also simulates a CRT
 * monitor: barrel curvature with rounded corners, Gaussian scanlines whose
 * beam widens with brightness, an aperture-grille mask and a vignette. */
#include "present_common.h"

#include <SDL_metal.h>
#import <TargetConditionals.h>
#if TARGET_OS_OSX
#import <AppKit/AppKit.h>
#else
#import <UIKit/UIKit.h>
#endif
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

/* CPU read-back textures (probe, present_capture): macOS needs a managed texture and a blit
 * synchronize; on iOS (unified memory) a shared texture is already visible to the CPU. */
#if TARGET_OS_OSX
#define READBACK_STORAGE MTLStorageModeManaged
#define SYNC_FOR_CPU(blit, tex) [(blit) synchronizeResource:(tex)]
#else
#define READBACK_STORAGE MTLStorageModeShared
#define SYNC_FOR_CPU(blit, tex) ((void)0)
#endif

static SDL_Window *window;
static SDL_MetalView view;
static CAMetalLayer *layer;
static id<MTLDevice> device;
static id<MTLCommandQueue> queue;
static id<MTLRenderPipelineState> pipe_lin, pipe_blur, pipe_crt, pipe_up, pipe_hd, pipe_flat;
static id<MTLTexture> index_tex, pal_tex, rgb_tex, lin_tex, blur_tex, glow_tex, hd_tex;
static int rgb_mode; /* the last frame came from present_frame_rgb */
static id<MTLSamplerState> linear_sampler;
static char name[128];

struct uniforms {
	float out_w, out_h; /* viewport size in drawable pixels */
	int32_t crt;
	int32_t rgb; /* read rgb_tex instead of index + palette */
	int32_t smooth; /* Catmull-Rom instead of area averaging */
	int32_t vw, vh; /* the virtual raster of CRT HD */
	int32_t pad[2];
};

/* The CRT is a late-90s flat-face Sony FD Trinitron PC monitor showing
 * 640x480. Per frame:
 *   fs_lin   index + palette -> linear-light RGB (640x480, half float)
 *   fs_blur  Gaussian blur of that, horizontally then vertically (the glow)
 *   fs_crt   beam and scanlines, aperture grille, damper wires, glow,
 *            vignette, dark glass
 * fs_crt adapts to the output scale (drawable pixels per screen pixel): at
 * 1x it is a plain resample, the scanlines fade in towards 3x and the grille
 * (RGB stripes three output pixels per triad, the finest that does not beat
 * against the output pixels) is faint at 2x and full from about 3.5x. */
static NSString *const shader_src = @
	"#include <metal_stdlib>\n"
	"using namespace metal;\n"
	"struct V { float4 pos [[position]]; float2 uv; };\n"
	"struct U { float2 out_size; int crt; int rgb; int smooth; int vw; int vh; int pad0; int pad1; };\n"
	"constant float GAMMA = 2.2;\n"
	"constant float SCAN = 1.0;\n"   /* scanline amount at >= 3.5x */
	"constant float BEAM_X = 0.25;\n" /* beam spot sigma, screen pixels */
	"constant float MASK = 0.22;\n"  /* aperture grille strength from 3.5x */
	"constant float GLOW = 0.10;\n"  /* halation: share of the blurred picture */
	"constant float WIRE = 0.22;\n"  /* damper wire shadow */
	"constant float WIRE_W = 0.3;\n" /* ... and width, in screen lines */
	"constant float3 GLASS = float3(0.0030, 0.0034, 0.0040);\n" /* dark glass, linear */
	"constant float3 LUMA = float3(0.2126, 0.7152, 0.0722);\n"
	"vertex V vs(uint vid [[vertex_id]]) {\n"
	"  float2 p = float2((vid << 1) & 2, vid & 2);\n"
	"  V o; o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0); o.uv = float2(p.x, 1.0 - p.y);\n"
	"  return o;\n"
	"}\n"
	/* one screen pixel: palette lookup, or the true-colour frame */
	"static float4 screen_px(texture2d<uint> idx, texture2d<float> pal, texture2d<float> rgb,\n"
	"                        int is_rgb, uint2 c) {\n"
	"  return float4((is_rgb ? rgb.read(c) : pal.read(uint2(idx.read(c).r, 0))).rgb, 1.0);\n"
	"}\n"
	"fragment float4 fs_lin(V in [[stage_in]], texture2d<uint> idx [[texture(0)]],\n"
	"                       texture2d<float> pal [[texture(1)]],\n"
	"                       texture2d<float> rgb [[texture(4)]], constant U &u [[buffer(0)]]) {\n"
	"  float3 s = screen_px(idx, pal, rgb, u.rgb, uint2(in.pos.xy)).rgb;\n"
	"  return float4(pow(s, GAMMA), 1.0);\n"
	"}\n"
	/* 9-tap Gaussian, sigma 2 screen pixels, along dir */
	"fragment float4 fs_blur(V in [[stage_in]], texture2d<float> t [[texture(0)]],\n"
	"                        constant int2 &dir [[buffer(0)]]) {\n"
	"  int2 c = int2(in.pos.xy), hi = int2(t.get_width() - 1, t.get_height() - 1);\n"
	"  float3 sum = 0.0; float wsum = 0.0;\n"
	"  for (int k = -4; k <= 4; k++) {\n"
	"    float w = exp(-0.125 * float(k * k));\n"
	"    sum += w * t.read(uint2(clamp(c + dir * k, int2(0), hi))).rgb; wsum += w;\n"
	"  }\n"
	"  return float4(sum / wsum, 1.0);\n"
	"}\n"
	"static float3 lin(texture2d<float> t, int2 c) {\n"
	"  return t.read(uint2(clamp(c, int2(0), int2(t.get_width() - 1, t.get_height() - 1)))).rgb;\n"
	"}\n"
	"static float erf_(float x) {\n" /* Winitzki's approximation */
	"  float x2 = x * x;\n"
	"  return sign(x) * sqrt(1.0 - exp(-x2 * (1.2732395 + 0.147 * x2) / (1.0 + 0.147 * x2)));\n"
	"}\n"
	/* a scanline beam of unit area and width sigma, averaged over the output
	 * pixel's footprint h (both in screen lines) at distance d: summed over
	 * all lines a flat field keeps its brightness, and the average over the
	 * footprint stops fractional scales from aliasing */
	"static float beam(float d, float sigma, float h) {\n"
	"  float k = 0.70710678 / sigma;\n"
	"  return (erf_((d + 0.5 * h) * k) - erf_((d - 0.5 * h) * k)) / (2.0 * h);\n"
	"}\n"
	"fragment float4 fs_crt(V in [[stage_in]], texture2d<uint> idx [[texture(0)]],\n"
	"                       texture2d<float> pal [[texture(1)]],\n"
	"                       texture2d<float> lt [[texture(2)]],\n"
	"                       texture2d<float> glow [[texture(3)]],\n"
	"                       texture2d<float> rgb [[texture(4)]],\n"
	"                       sampler smp [[sampler(0)]], constant U &u [[buffer(0)]]) {\n"
	"  float2 src = float2(idx.get_width(), idx.get_height());\n"
	"  if (u.crt == 0) {\n"
	"    uint2 c = min(uint2(in.uv * src), uint2(src) - 1);\n"
	"    return screen_px(idx, pal, rgb, u.rgb, c);\n"
	"  }\n"
	"  float2 scale = u.out_size / src;\n"
	/* flat face: the raster's corners are only slightly rounded */
	"  float2 c = in.uv * 2.0 - 1.0;\n"
	"  const float r = 0.012;\n"
	"  float d = length(max(abs(c) - (1.0 - r), 0.0)) - r;\n"
	"  float edge = saturate(-d * u.out_size.y * 0.5);\n"
	"  float2 uv = in.uv;\n"
	"  float2 p = uv * src - 0.5;\n"
	/* at 2x the two output rows of a line would sit symmetrically about the
	 * beam and get the same light (no visible scanlines): shift by half an
	 * output row so one is on the beam and the other in the gap */
	"  p.y += 0.5 / scale.y * saturate(1.0 - 2.0 * abs(scale.y - 2.0));\n"
	"  float2 b = floor(p), f = p - b;\n"
	"  int2 i = int2(b);\n"
	/* horizontally each screen pixel is a box blurred by the beam spot
	 * (BEAM_X screen pixels, less below 2x) and averaged over the output
	 * pixel: the weight of a pixel at distance dx */
	"  float h = 1.0 / scale.x;\n"
	"  float sx = sqrt(pow(BEAM_X * smoothstep(1.0, 2.0, scale.x), 2.0) + h * h / 12.0 + 1e-4);\n"
	"  float4 dx = f.x - float4(-1.0, 0.0, 1.0, 2.0);\n"
	"  float kx = 0.70710678 / sx;\n"
	"  float4 wx;\n"
	"  for (int j = 0; j < 4; j++) wx[j] = erf_((dx[j] + 0.5) * kx) - erf_((dx[j] - 0.5) * kx);\n"
	"  wx /= dot(wx, 1.0);\n"
	"  float3 row[4];\n"
	"  for (int k = 0; k < 4; k++) {\n"
	"    row[k] = 0.0;\n"
	"    for (int j = 0; j < 4; j++) row[k] += wx[j] * lin(lt, i + int2(j - 1, k - 1));\n"
	"  }\n"
	/* without scanlines: the same box filter vertically */
	"  float hy = 1.0 / scale.y;\n"
	"  float sy = sqrt(pow(BEAM_X * smoothstep(1.0, 2.0, scale.y), 2.0) + hy * hy / 12.0 + 1e-4);\n"
	"  float3 flat = 0.0; float wsum = 0.0;\n"
	"  for (int k = 0; k < 4; k++) {\n"
	"    float dy = f.y - float(k - 1);\n"
	"    float w = erf_((dy + 0.5) * 0.70710678 / sy) - erf_((dy - 0.5) * 0.70710678 / sy);\n"
	"    flat += w * row[k]; wsum += w;\n"
	"  }\n"
	"  flat /= wsum;\n"
	/* scanlines: Gaussian beams, wider (brighter lines bloom into the gaps) */
	"  float3 scan = 0.0;\n"
	"  for (int k = 0; k < 4; k++) {\n"
	"    float sigma = mix(0.28, 0.38, sqrt(dot(row[k], LUMA)));\n"
	"    scan += row[k] * beam(f.y - float(k - 1), sigma, hy);\n"
	"  }\n"
	"  float3 col = mix(flat, scan, SCAN * smoothstep(1.0, 2.0, scale.y));\n"
	/* aperture grille: continuous vertical R, G, B stripes in output pixels,
	 * the loss made up (bright pixels clip on their own stripe) */
	"  float ms = MASK * saturate((scale.x - 1.25) / 2.25);\n"
	"  uint m = uint(in.pos.x) - 3u * (uint(in.pos.x) / 3u);\n"
	"  col *= (1.0 - 1.5 * ms * (1.0 - float3(m == 0u, m == 1u, m == 2u))) / (1.0 - ms);\n"
	/* glow: phosphor halation */
	"  col = mix(col, glow.sample(smp, uv).rgb, GLOW);\n"
	/* damper wires at 1/3 and 2/3 of the height: their thin shadow,
	 * averaged over the output pixel */
	"  float y = uv.y * src.y;\n"
	"  float wire = 0.0;\n"
	"  for (int k = 1; k <= 2; k++) {\n"
	"    float wy = src.y * float(k) / 3.0;\n"
	"    float lo = max(y - 0.5 * hy, wy - 0.5 * WIRE_W), hi = min(y + 0.5 * hy, wy + 0.5 * WIRE_W);\n"
	"    wire += max(hi - lo, 0.0) / hy;\n"
	"  }\n"
	"  col *= 1.0 - WIRE * saturate(wire);\n"
	/* mild vignette, then the dark grey-black of the glass under it all */
	"  float2 vv = uv * (1.0 - uv);\n"
	"  col *= pow(saturate(vv.x * vv.y * 16.0), 0.06);\n"
	"  col = GLASS + col * (1.0 - GLASS);\n"
	"  return float4(pow(saturate(col), 1.0 / GAMMA) * edge, 1.0);\n"
	"}\n"
	/* ---- display modes CRT HD, Sharp, Smooth (CRT 480 is fs_crt above) ----
	 * Area-averaged ("sharp") or Catmull-Rom weights of the source pixels
	 * for the interval a..b (source coordinates) of one output pixel. */
	"struct Taps { int i0; int n; float w[8]; };\n"
	"static Taps taps(float a, float b, int smooth_) {\n"
	"  Taps t; t.i0 = 0; t.n = 0;\n"
	"  for (int k = 0; k < 8; k++) t.w[k] = 0.0;\n"
	"  if (smooth_ != 0 && b - a <= 1.0001) {\n"
	"    float p = 0.5 * (a + b) - 0.5, fl = floor(p), x = p - fl, x2 = x * x, x3 = x2 * x;\n"
	"    t.i0 = int(fl) - 1; t.n = 4;\n"
	"    t.w[0] = -0.5 * x3 + x2 - 0.5 * x; t.w[1] = 1.5 * x3 - 2.5 * x2 + 1.0;\n"
	"    t.w[2] = -1.5 * x3 + 2.0 * x2 + 0.5 * x; t.w[3] = 0.5 * x3 - 0.5 * x2;\n"
	"    return t;\n"
	"  }\n"
	"  t.i0 = int(floor(a + 1e-5));\n"
	"  t.n = min(int(ceil(b - 1e-5)) - t.i0, 8);\n"
	"  float sum = 0.0;\n"
	"  for (int k = 0; k < t.n; k++) {\n"
	"    float j = float(t.i0 + k);\n"
	"    t.w[k] = max(min(b, j + 1.0) - max(a, j), 0.0); sum += t.w[k];\n"
	"  }\n"
	"  for (int k = 0; k < t.n; k++) t.w[k] /= sum;\n"
	"  return t;\n"
	"}\n"
	"static float3 resample(texture2d<float> t, float2 a, float2 b, int smooth_) {\n"
	"  Taps tx = taps(a.x, b.x, smooth_), ty = taps(a.y, b.y, smooth_);\n"
	"  float3 sum = 0.0;\n"
	"  for (int j = 0; j < ty.n; j++) {\n"
	"    float3 r = 0.0;\n"
	"    for (int i = 0; i < tx.n; i++) r += tx.w[i] * lin(t, int2(tx.i0 + i, ty.i0 + j));\n"
	"    sum += ty.w[j] * r;\n"
	"  }\n"
	"  return max(sum, 0.0);\n"
	"}\n"
	/* CRT HD, first pass: the 640x480 linear picture onto the virtual raster */
	"fragment float4 fs_up(V in [[stage_in]], texture2d<float> t [[texture(0)]], constant U &u [[buffer(0)]]) {\n"
	"  float2 r = float2(t.get_width(), t.get_height()) / float2(u.vw, u.vh);\n"
	"  float2 o = in.pos.xy;\n"
	"  return float4(resample(t, o * r, (o + 1.0) * r, u.smooth), 1.0);\n"
	"}\n"
	/* Sharp and Smooth: the same straight onto the output; a pixel that lies
	 * wholly within one source pixel is that pixel's exact colour */
	"fragment float4 fs_flat(V in [[stage_in]], texture2d<uint> idx [[texture(0)]],\n"
	"                        texture2d<float> pal [[texture(1)]], texture2d<float> lt [[texture(2)]],\n"
	"                        texture2d<float> rgb [[texture(4)]], constant U &u [[buffer(0)]]) {\n"
	"  float2 src = float2(idx.get_width(), idx.get_height());\n"
	"  float2 r = src / u.out_size, o = in.pos.xy;\n"
	"  float2 a = o * r, b = (o + 1.0) * r;\n"
	"  int2 c = int2(floor(a + 1e-4));\n"
	"  if (u.smooth == 0 && all(floor(b - 1e-4) == floor(a + 1e-4)))\n"
	"    return screen_px(idx, pal, rgb, u.rgb, uint2(clamp(c, int2(0), int2(src) - 1)));\n"
	"  float3 l = resample(lt, a, b, u.smooth);\n"
	"  return float4(pow(saturate(l), 1.0 / GAMMA), 1.0);\n"
	"}\n"
	/* CRT HD, second pass: fs_crt's tube on the virtual raster lt (any size
	 * against the output: more taps when an output pixel spans several
	 * virtual pixels). Beam, scanlines and grille work at the raster's line
	 * pitch; the damper wires and the glow are the tube's. */
	"constant float BEAM_HD = 0.5;\n" /* beam spot sigma, virtual pixels */
	"fragment float4 fs_hd(V in [[stage_in]], texture2d<float> lt [[texture(2)]],\n"
	"                      texture2d<float> glow [[texture(3)]],\n"
	"                      sampler smp [[sampler(0)]], constant U &u [[buffer(0)]]) {\n"
	"  float2 src = float2(lt.get_width(), lt.get_height());\n"
	"  float2 scale = u.out_size / src;\n"
	"  float2 c = in.uv * 2.0 - 1.0;\n"
	"  const float r = 0.012;\n"
	"  float d = length(max(abs(c) - (1.0 - r), 0.0)) - r;\n"
	"  float edge = saturate(-d * u.out_size.y * 0.5);\n"
	"  float2 uv = in.uv;\n"
	"  float2 p = uv * src - 0.5;\n"
	"  p.y += 0.5 / scale.y * saturate(1.0 - 2.0 * abs(scale.y - 2.0));\n"
	"  float2 b = floor(p), f = p - b;\n"
	"  int2 i = int2(b);\n"
	"  int N = min(scale.x, scale.y) < 0.8 ? 8 : 4;\n"
	"  int o0 = 1 - N / 2;\n"
	"  float h = 1.0 / scale.x;\n"
	"  float sx = sqrt(pow(BEAM_HD * smoothstep(1.0, 2.0, scale.x), 2.0) + h * h / 12.0 + 1e-4);\n"
	"  float kx = 0.70710678 / sx;\n"
	"  float wx[8]; float wsx = 0.0;\n"
	"  for (int j = 0; j < N; j++) {\n"
	"    float dx = f.x - float(o0 + j);\n"
	"    wx[j] = erf_((dx + 0.5) * kx) - erf_((dx - 0.5) * kx); wsx += wx[j];\n"
	"  }\n"
	"  float3 row[8];\n"
	"  for (int k = 0; k < N; k++) {\n"
	"    float3 acc = 0.0;\n"
	"    for (int j = 0; j < N; j++) acc += wx[j] * lin(lt, i + int2(o0 + j, o0 + k));\n"
	"    row[k] = acc / wsx;\n"
	"  }\n"
	"  float hy = 1.0 / scale.y;\n"
	"  float sy = sqrt(pow(BEAM_HD * smoothstep(1.0, 2.0, scale.y), 2.0) + hy * hy / 12.0 + 1e-4);\n"
	"  float3 flat = 0.0; float wsum = 0.0;\n"
	"  for (int k = 0; k < N; k++) {\n"
	"    float dy = f.y - float(o0 + k);\n"
	"    float w = erf_((dy + 0.5) * 0.70710678 / sy) - erf_((dy - 0.5) * 0.70710678 / sy);\n"
	"    flat += w * row[k]; wsum += w;\n"
	"  }\n"
	"  flat /= wsum;\n"
	"  float3 scan = 0.0;\n"
	"  for (int k = 0; k < N; k++) {\n"
	"    float sigma = mix(0.28, 0.38, sqrt(dot(row[k], LUMA)));\n"
	"    scan += row[k] * beam(f.y - float(o0 + k), sigma, hy);\n"
	"  }\n"
	"  float3 col = mix(flat, scan, SCAN * smoothstep(1.0, 2.0, scale.y));\n"
	"  float ms = MASK * saturate((scale.x - 1.25) / 2.25);\n"
	"  uint m = uint(in.pos.x) - 3u * (uint(in.pos.x) / 3u);\n"
	"  col *= (1.0 - 1.5 * ms * (1.0 - float3(m == 0u, m == 1u, m == 2u))) / (1.0 - ms);\n"
	"  col = mix(col, glow.sample(smp, uv).rgb, GLOW);\n"
	/* the wires keep their place and thickness on the tube (WIRE_W game lines) */
	"  float y = uv.y * src.y;\n"
	"  float ww = WIRE_W * src.y / 480.0;\n"
	"  float wire = 0.0;\n"
	"  for (int k = 1; k <= 2; k++) {\n"
	"    float wy = src.y * float(k) / 3.0;\n"
	"    float lo = max(y - 0.5 * hy, wy - 0.5 * ww), hi = min(y + 0.5 * hy, wy + 0.5 * ww);\n"
	"    wire += max(hi - lo, 0.0) / hy;\n"
	"  }\n"
	"  col *= 1.0 - WIRE * saturate(wire);\n"
	"  float2 vv = uv * (1.0 - uv);\n"
	"  col *= pow(saturate(vv.x * vv.y * 16.0), 0.06);\n"
	"  col = GLASS + col * (1.0 - GLASS);\n"
	"  return float4(pow(saturate(col), 1.0 / GAMMA) * edge, 1.0);\n"
	"}\n";

static id<MTLRenderPipelineState> make_pipeline(id<MTLLibrary> lib, id<MTLFunction> vs, NSString *fs,
					       MTLPixelFormat fmt)
{
	NSError *err = nil;
	MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
	pd.vertexFunction = vs;
	pd.fragmentFunction = [lib newFunctionWithName:fs];
	pd.colorAttachments[0].pixelFormat = fmt;
	id<MTLRenderPipelineState> p = [device newRenderPipelineStateWithDescriptor:pd error:&err];
	if (!p)
		SDL_Log("Metal pipeline %s: %s", fs.UTF8String, err.localizedDescription.UTF8String);
	return p;
}

static id<MTLTexture> make_texture(MTLPixelFormat fmt, int w, int h, MTLTextureUsage usage)
{
	MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt
										      width:(NSUInteger)w
										     height:(NSUInteger)h
										  mipmapped:NO];
	td.usage = usage;
	td.storageMode = usage & MTLTextureUsageRenderTarget ? MTLStorageModePrivate : READBACK_STORAGE;
	return [device newTextureWithDescriptor:td];
}

Uint32 present_window_flags(void)
{
#if TARGET_OS_OSX
	return present_headless() ? 0 : SDL_WINDOW_METAL;
#else
	/* iOS: a full-screen Metal window; the view's layer is sized from its bounds x the display scale */
	return present_headless() ? 0 : SDL_WINDOW_METAL | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_BORDERLESS |
						 SDL_WINDOW_ALLOW_HIGHDPI;
#endif
}

int present_init(SDL_Window *win)
{
	window = win;
	if (present_headless()) {
		/* no Metal device or layer (present_common.h) */
		soft = 1;
		snprintf(name, sizeof name, "headless (software, no GPU)");
		return 1;
	}
	view = SDL_Metal_CreateView(win);
	if (!view)
		return 0;
	layer = (__bridge CAMetalLayer *)SDL_Metal_GetLayer(view);
	device = MTLCreateSystemDefaultDevice();
	if (!device)
		return 0;
	layer.device = device;
	layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
	layer.framebufferOnly = YES;
	queue = [device newCommandQueue];

	NSError *err = nil;
	id<MTLLibrary> lib = [device newLibraryWithSource:shader_src options:nil error:&err];
	if (!lib) {
		SDL_Log("Metal shader: %s", err.localizedDescription.UTF8String);
		return 0;
	}
	id<MTLFunction> vs = [lib newFunctionWithName:@"vs"];
	pipe_lin = make_pipeline(lib, vs, @"fs_lin", MTLPixelFormatRGBA16Float);
	pipe_blur = make_pipeline(lib, vs, @"fs_blur", MTLPixelFormatRGBA16Float);
	pipe_crt = make_pipeline(lib, vs, @"fs_crt", layer.pixelFormat);
	if (!pipe_lin || !pipe_blur || !pipe_crt)
		return 0;

	index_tex = make_texture(MTLPixelFormatR8Uint, SCREEN_W, SCREEN_H, MTLTextureUsageShaderRead);
	pal_tex = make_texture(MTLPixelFormatBGRA8Unorm, 256, 1, MTLTextureUsageShaderRead);
	rgb_tex = make_texture(MTLPixelFormatBGRA8Unorm, SCREEN_W, SCREEN_H, MTLTextureUsageShaderRead);
	MTLTextureUsage rt = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
	lin_tex = make_texture(MTLPixelFormatRGBA16Float, SCREEN_W, SCREEN_H, rt);
	blur_tex = make_texture(MTLPixelFormatRGBA16Float, SCREEN_W, SCREEN_H, rt);
	glow_tex = make_texture(MTLPixelFormatRGBA16Float, SCREEN_W, SCREEN_H, rt);
	MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
	sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
	sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
	linear_sampler = [device newSamplerStateWithDescriptor:sd];
	if (!index_tex || !pal_tex || !rgb_tex || !lin_tex || !blur_tex || !glow_tex || !linear_sampler)
		return 0;
	snprintf(name, sizeof name, "Metal | %s", device.name.UTF8String);
	return 1;
}

/* One full-screen triangle with fragment shader pipe into target. */
static id<MTLRenderCommandEncoder> begin_pass(id<MTLCommandBuffer> cb, id<MTLTexture> target,
					       id<MTLRenderPipelineState> pipe)
{
	MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
	rp.colorAttachments[0].texture = target;
	rp.colorAttachments[0].loadAction = MTLLoadActionClear;
	rp.colorAttachments[0].storeAction = MTLStoreActionStore;
	rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
	id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
	[enc setRenderPipelineState:pipe];
	return enc;
}

static void blur_pass(id<MTLCommandBuffer> cb, id<MTLTexture> from, id<MTLTexture> to, int dx, int dy)
{
	int32_t dir[2] = { dx, dy };
	id<MTLRenderCommandEncoder> enc = begin_pass(cb, to, pipe_blur);
	[enc setFragmentTexture:from atIndex:0];
	[enc setFragmentBytes:dir length:sizeof dir atIndex:0];
	[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
	[enc endEncoding];
}

/* Draw the screen into target (dw x dh), letterboxed. */
static void encode(id<MTLCommandBuffer> cb, id<MTLTexture> target, int dw, int dh)
{
	int x, y, w, h;
	present_fit(dw, dh, &x, &y, &w, &h);
	struct uniforms u = { (float)w, (float)h, crt, rgb_mode };
	if (crt) {
		id<MTLRenderCommandEncoder> enc = begin_pass(cb, lin_tex, pipe_lin);
		[enc setFragmentTexture:index_tex atIndex:0];
		[enc setFragmentTexture:pal_tex atIndex:1];
		[enc setFragmentTexture:rgb_tex atIndex:4];
		[enc setFragmentBytes:&u length:sizeof u atIndex:0];
		[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
		[enc endEncoding];
		blur_pass(cb, lin_tex, blur_tex, 1, 0);
		blur_pass(cb, blur_tex, glow_tex, 0, 1);
	}
	id<MTLRenderCommandEncoder> enc = begin_pass(cb, target, pipe_crt);
	[enc setViewport:(MTLViewport){ x, y, w, h, 0, 1 }];
	[enc setFragmentTexture:index_tex atIndex:0];
	[enc setFragmentTexture:pal_tex atIndex:1];
	[enc setFragmentTexture:lin_tex atIndex:2];
	[enc setFragmentTexture:glow_tex atIndex:3];
	[enc setFragmentTexture:rgb_tex atIndex:4];
	[enc setFragmentSamplerState:linear_sampler atIndex:0];
	[enc setFragmentBytes:&u length:sizeof u atIndex:0];
	[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
	[enc endEncoding];
}

static void show(void);

/* Offscreen probe (struct present_stats): render the current frame at
 * 640x480 without the CRT simulation into a texture kept for the purpose,
 * read it back and note its mean luminance. Called from show() while
 * presents are skipped, at most once a second: one small render and a
 * 1.2 MB readback. */
static id<MTLTexture> probe_tex;
static uint32_t probe_buf[SCREEN_W * SCREEN_H];

static void probe(void)
{
	uint32_t now = SDL_GetTicks();
	if (pstats.last_probe && now - pstats.last_probe < 1000)
		return;
	if (!probe_tex) {
		MTLTextureDescriptor *td =
			[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
									   width:SCREEN_W
									  height:SCREEN_H
								       mipmapped:NO];
		td.usage = MTLTextureUsageRenderTarget;
		td.storageMode = READBACK_STORAGE;
		probe_tex = [device newTextureWithDescriptor:td];
		if (!probe_tex)
			return;
	}
	int saved_crt = crt;
	crt = 0;
	id<MTLCommandBuffer> cb = [queue commandBuffer];
	encode(cb, probe_tex, SCREEN_W, SCREEN_H);
	crt = saved_crt;
	id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
	SYNC_FOR_CPU(bl, probe_tex);
	[bl endEncoding];
	[cb commit];
	[cb waitUntilCompleted];
	if (cb.status != MTLCommandBufferStatusCompleted)
		return;
	[probe_tex getBytes:probe_buf
		bytesPerRow:SCREEN_W * 4
		 fromRegion:MTLRegionMake2D(0, 0, SCREEN_W, SCREEN_H)
		mipmapLevel:0];
	/* BGRA8 in memory is 0xAARRGGBB as a little-endian word */
	present_note_probe(present_luma_rgb(probe_buf));
}

void present_frame(const uint8_t *pixels, const uint32_t pal[256])
{
	if (soft) {
		soft_frame(pixels, pal);
		return;
	}
	@autoreleasepool {
		[index_tex replaceRegion:MTLRegionMake2D(0, 0, SCREEN_W, SCREEN_H)
			     mipmapLevel:0
			       withBytes:pixels
			     bytesPerRow:SCREEN_W];
		uint32_t bgra[256];
		for (int i = 0; i < 256; i++)
			bgra[i] = 0xff000000u | pal[i]; /* 0x00RRGGBB is BGRA in memory */
		[pal_tex replaceRegion:MTLRegionMake2D(0, 0, 256, 1)
			   mipmapLevel:0
			     withBytes:bgra
			   bytesPerRow:sizeof bgra];
		rgb_mode = 0;
		present_note_in(present_luma_indexed(pixels, pal));
		show();
	}
}

void present_frame_rgb(const uint32_t *xrgb)
{
	if (soft) {
		soft_frame_rgb(xrgb);
		return;
	}
	@autoreleasepool {
		/* alpha is ignored by the shaders, so 0x00RRGGBB uploads as BGRA */
		[rgb_tex replaceRegion:MTLRegionMake2D(0, 0, SCREEN_W, SCREEN_H)
			   mipmapLevel:0
			     withBytes:xrgb
			   bytesPerRow:SCREEN_W * 4];
		rgb_mode = 1;
		present_note_in(present_luma_rgb(xrgb));
		show();
	}
}

static void show(void)
{
	@autoreleasepool {
		int dw, dh;
		SDL_Metal_GetDrawableSize(window, &dw, &dh);
		pstats.drawable_w = dw;
		pstats.drawable_h = dh;
		if (dw <= 0 || dh <= 0) {
			pstats.skip_size++;
			return;
		}
		/* nextDrawable blocks up to its 1 s timeout while the window is
		 * covered, which would stall the game; skip until it is visible.
		 * The textures hold the frame, so the next call shows it. */
#if TARGET_OS_OSX
		NSWindow *nsw = ((__bridge NSView *)view).window;
		pstats.occluded = nsw && !(nsw.occlusionState & NSWindowOcclusionStateVisible);
#else
		/* iOS: no window occlusion; the background is handled in window.c (no pump work, no
		 * presenting) and the app state is checked here */
		pstats.occluded = UIApplication.sharedApplication.applicationState == UIApplicationStateBackground;
#endif
		if (pstats.occluded) {
			pstats.skip_occluded++;
			probe();
			return;
		}
		layer.drawableSize = CGSizeMake(dw, dh);
		Uint64 t = SDL_GetPerformanceCounter();
		id<CAMetalDrawable> drawable = [layer nextDrawable];
		pstats.last_wait_ms = (double)(SDL_GetPerformanceCounter() - t) * 1000.0 /
				      (double)SDL_GetPerformanceFrequency();
		if (pstats.last_wait_ms > pstats.worst_wait_ms)
			pstats.worst_wait_ms = pstats.last_wait_ms;
		if (!drawable) {
			pstats.skip_drawable++;
			return;
		}
		id<MTLCommandBuffer> cb = [queue commandBuffer];
		encode(cb, drawable.texture, dw, dh);
		[cb presentDrawable:drawable];
		[cb commit];
		present_note_shown();
	}
}

void present_stats(struct present_stats *out)
{
	@autoreleasepool {
#if TARGET_OS_OSX
		NSWindow *nsw = ((__bridge NSView *)view).window;
		if (nsw)
			pstats.occluded = !(nsw.occlusionState & NSWindowOcclusionStateVisible);
		int n = 0;
		for (NSWindow *w in NSApp.windows)
			n += w.visible ? 1 : 0;
		pstats.os_windows = n;
		pstats.app_hidden = NSApp.hidden ? 1 : 0;
#else
		pstats.os_windows = 1;
		pstats.app_hidden = UIApplication.sharedApplication.applicationState == UIApplicationStateBackground;
#endif
		*out = pstats;
	}
}

int present_capture(const char *bmp_path, int w, int h)
{
	if (soft)
		return soft_capture(bmp_path, w, h);
	@autoreleasepool {
		if (w <= 0 || h <= 0) /* the window's drawable size */
			SDL_Metal_GetDrawableSize(window, &w, &h);
		MTLTextureDescriptor *td =
			[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
									   width:(NSUInteger)w
									  height:(NSUInteger)h
								       mipmapped:NO];
		td.usage = MTLTextureUsageRenderTarget;
		td.storageMode = READBACK_STORAGE;
		id<MTLTexture> t = [device newTextureWithDescriptor:td];
		if (!t)
			return 0;
		id<MTLCommandBuffer> cb = [queue commandBuffer];
		encode(cb, t, w, h);
		id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
		SYNC_FOR_CPU(bl, t);
		[bl endEncoding];
		[cb commit];
		[cb waitUntilCompleted];

		uint32_t *buf = malloc((size_t)w * (size_t)h * 4);
		if (!buf)
			return 0;
		[t getBytes:buf
			bytesPerRow:(NSUInteger)w * 4
			 fromRegion:MTLRegionMake2D(0, 0, (NSUInteger)w, (NSUInteger)h)
			mipmapLevel:0];
		SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(buf, w, h, 32, w * 4,
								    SDL_PIXELFORMAT_ARGB8888);
		int ok = s && SDL_SaveBMP(s, bmp_path) == 0;
		SDL_FreeSurface(s);
		free(buf);
		return ok;
	}
}

void present_set_crt(int on)
{
	crt = on;
}

int present_get_crt(void)
{
	return crt;
}

const char *present_name(void)
{
	return name;
}

int present_window_to_screen(int wx, int wy, int *sx, int *sy)
{
	return present_map_point(window, wx, wy, sx, sy);
}

void present_screen_to_window(int sx, int sy, int *wx, int *wy)
{
	present_unmap_point(window, sx, sy, wx, wy);
}

static void toggle_crt(void)
{
	crt = !crt;
}

int present_hotkey(const SDL_KeyboardEvent *key)
{
	return present_common_hotkey(window, key, toggle_crt);
}

void present_set_fullscreen(int on)
{
	present_common_set_fullscreen(window, on);
}

int present_get_fullscreen(void)
{
	return present_common_get_fullscreen(window);
}

void present_shutdown(void)
{
	pipe_lin = pipe_blur = pipe_crt = nil;
	index_tex = pal_tex = rgb_tex = lin_tex = blur_tex = glow_tex = nil;
	linear_sampler = nil;
	probe_tex = nil;
	queue = nil;
	device = nil;
	layer = nil;
	if (view)
		SDL_Metal_DestroyView(view);
	view = NULL;
}
