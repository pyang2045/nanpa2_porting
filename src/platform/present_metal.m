/* macOS presenter: the window's CAMetalLayer (SDL_Metal_CreateView).
 * The 8-bit screen and its palette are uploaded as two textures and the
 * palette lookup happens in the fragment shader, which also simulates a CRT
 * monitor: barrel curvature with rounded corners, Gaussian scanlines whose
 * beam widens with brightness, an aperture-grille mask and a vignette. */
#include "present_common.h"

#include <SDL_metal.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

static SDL_Window *window;
static SDL_MetalView view;
static CAMetalLayer *layer;
static id<MTLDevice> device;
static id<MTLCommandQueue> queue;
static id<MTLRenderPipelineState> pipe_lin, pipe_blur, pipe_crt;
static id<MTLTexture> index_tex, pal_tex, lin_tex, blur_tex, glow_tex;
static id<MTLSamplerState> linear_sampler;
static char name[128];
static int crt = 1;

struct uniforms {
	float out_w, out_h; /* viewport size in drawable pixels */
	int32_t crt;
	int32_t pad;
};

/* The CRT pipeline, per frame:
 *   fs_lin   index + palette -> linear-light RGB (640x480, half float)
 *   fs_blur  Gaussian blur of that, horizontally then vertically (the glow)
 *   fs_crt   curvature, beam/scanlines, aperture grille, glow, vignette
 * fs_crt adapts to the output scale (drawable pixels per screen pixel):
 * at 1x it is a plain antialiased resample of the curved picture, scanlines
 * fade in from about 2x and the chromatic grille only from 3x, where it
 * cannot beat against the screen pixels into colour fringes.
 * %1$f/%2$f are CRT_WARP_X/Y, shared with present_crt_warp(). */
static NSString *const shader_fmt = @
	"#include <metal_stdlib>\n"
	"using namespace metal;\n"
	"struct V { float4 pos [[position]]; float2 uv; };\n"
	"struct U { float2 out_size; int crt; int pad; };\n"
	"constant float WARP_X = %1$.9g, WARP_Y = %2$.9g;\n"
	"constant float GAMMA = 2.2;\n"
	"constant float SCAN = 1.0;\n"   /* scanline amount at >= 3.5x */
	"constant float BEAM_X = 0.32;\n" /* beam spot sigma, screen pixels */
	"constant float MASK = 0.14;\n"  /* aperture grille strength at >= 4x */
	"constant float GLOW = 0.15;\n"  /* halation: share of the blurred picture */
	"constant float3 LUMA = float3(0.2126, 0.7152, 0.0722);\n"
	"vertex V vs(uint vid [[vertex_id]]) {\n"
	"  float2 p = float2((vid << 1) & 2, vid & 2);\n"
	"  V o; o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0); o.uv = float2(p.x, 1.0 - p.y);\n"
	"  return o;\n"
	"}\n"
	"fragment float4 fs_lin(V in [[stage_in]], texture2d<uint> idx [[texture(0)]],\n"
	"                       texture2d<float> pal [[texture(1)]]) {\n"
	"  float3 s = pal.read(uint2(idx.read(uint2(in.pos.xy)).r, 0)).rgb;\n"
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
	"                       sampler smp [[sampler(0)]], constant U &u [[buffer(0)]]) {\n"
	"  float2 src = float2(idx.get_width(), idx.get_height());\n"
	"  if (u.crt == 0) {\n"
	"    uint2 c = min(uint2(in.uv * src), uint2(src) - 1);\n"
	"    return pal.read(uint2(idx.read(c).r, 0));\n"
	"  }\n"
	"  float2 scale = u.out_size / src;\n"
	/* curvature: same formula as present_crt_warp(); rounded, antialiased corners */
	"  float2 c = in.uv * 2.0 - 1.0;\n"
	"  c *= float2(1.0 + c.y * c.y * WARP_X, 1.0 + c.x * c.x * WARP_Y);\n"
	"  const float r = 0.035;\n"
	"  float d = length(max(abs(c) - (1.0 - r), 0.0)) - r;\n"
	"  float edge = saturate(-d * u.out_size.y * 0.5);\n"
	"  if (edge <= 0.0) return float4(0, 0, 0, 1);\n"
	"  float2 uv = c * 0.5 + 0.5;\n"
	"  float2 p = uv * src - 0.5;\n"
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
	"  float3 col = mix(flat, scan, SCAN * smoothstep(1.0, 3.0, scale.y));\n"
	/* aperture grille in output pixels, only where there are enough of them */
	"  float ms = MASK * smoothstep(2.75, 4.0, scale.x);\n"
	"  uint m = uint(in.pos.x) - 3u * (uint(in.pos.x) / 3u);\n"
	/* darkening only (no clipped channels), half the loss made up */
	"  col *= (1.0 - 1.5 * ms * (1.0 - float3(m == 0u, m == 1u, m == 2u))) / (1.0 - 0.5 * ms);\n"
	/* glow: phosphor halation, mostly from bright areas */
	"  float3 g = glow.sample(smp, uv).rgb;\n"
	"  col = mix(col, g, GLOW);\n"
	/* vignette */
	"  float2 vv = uv * (1.0 - uv);\n"
	"  col *= pow(saturate(vv.x * vv.y * 16.0), 0.15);\n"
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
	td.storageMode = usage & MTLTextureUsageRenderTarget ? MTLStorageModePrivate : MTLStorageModeManaged;
	return [device newTextureWithDescriptor:td];
}

Uint32 present_window_flags(void)
{
	return SDL_WINDOW_METAL;
}

int present_init(SDL_Window *win)
{
	window = win;
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
	NSString *src = [NSString stringWithFormat:shader_fmt, (double)CRT_WARP_X, (double)CRT_WARP_Y];
	id<MTLLibrary> lib = [device newLibraryWithSource:src options:nil error:&err];
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
	MTLTextureUsage rt = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
	lin_tex = make_texture(MTLPixelFormatRGBA16Float, SCREEN_W, SCREEN_H, rt);
	blur_tex = make_texture(MTLPixelFormatRGBA16Float, SCREEN_W, SCREEN_H, rt);
	glow_tex = make_texture(MTLPixelFormatRGBA16Float, SCREEN_W, SCREEN_H, rt);
	MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
	sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
	sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
	linear_sampler = [device newSamplerStateWithDescriptor:sd];
	if (!index_tex || !pal_tex || !lin_tex || !blur_tex || !glow_tex || !linear_sampler)
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
	if (crt) {
		id<MTLRenderCommandEncoder> enc = begin_pass(cb, lin_tex, pipe_lin);
		[enc setFragmentTexture:index_tex atIndex:0];
		[enc setFragmentTexture:pal_tex atIndex:1];
		[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
		[enc endEncoding];
		blur_pass(cb, lin_tex, blur_tex, 1, 0);
		blur_pass(cb, blur_tex, glow_tex, 0, 1);
	}
	int x, y, w, h;
	present_fit(dw, dh, &x, &y, &w, &h);
	struct uniforms u = { (float)w, (float)h, crt, 0 };
	id<MTLRenderCommandEncoder> enc = begin_pass(cb, target, pipe_crt);
	[enc setViewport:(MTLViewport){ x, y, w, h, 0, 1 }];
	[enc setFragmentTexture:index_tex atIndex:0];
	[enc setFragmentTexture:pal_tex atIndex:1];
	[enc setFragmentTexture:lin_tex atIndex:2];
	[enc setFragmentTexture:glow_tex atIndex:3];
	[enc setFragmentSamplerState:linear_sampler atIndex:0];
	[enc setFragmentBytes:&u length:sizeof u atIndex:0];
	[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
	[enc endEncoding];
}

void present_frame(const uint8_t *pixels, const uint32_t pal[256])
{
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

		int dw, dh;
		SDL_Metal_GetDrawableSize(window, &dw, &dh);
		if (dw <= 0 || dh <= 0)
			return;
		layer.drawableSize = CGSizeMake(dw, dh);
		id<CAMetalDrawable> drawable = [layer nextDrawable];
		if (!drawable)
			return;
		id<MTLCommandBuffer> cb = [queue commandBuffer];
		encode(cb, drawable.texture, dw, dh);
		[cb presentDrawable:drawable];
		[cb commit];
	}
}

int present_capture(const char *bmp_path, int w, int h)
{
	@autoreleasepool {
		if (w <= 0 || h <= 0) /* the window's drawable size */
			SDL_Metal_GetDrawableSize(window, &w, &h);
		MTLTextureDescriptor *td =
			[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
									   width:(NSUInteger)w
									  height:(NSUInteger)h
								       mipmapped:NO];
		td.usage = MTLTextureUsageRenderTarget;
		td.storageMode = MTLStorageModeManaged;
		id<MTLTexture> t = [device newTextureWithDescriptor:td];
		if (!t)
			return 0;
		id<MTLCommandBuffer> cb = [queue commandBuffer];
		encode(cb, t, w, h);
		id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
		[bl synchronizeResource:t];
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
	return present_map_point(window, wx, wy, crt, sx, sy);
}

void present_shutdown(void)
{
	pipe_lin = pipe_blur = pipe_crt = nil;
	index_tex = pal_tex = lin_tex = blur_tex = glow_tex = nil;
	linear_sampler = nil;
	queue = nil;
	device = nil;
	layer = nil;
	if (view)
		SDL_Metal_DestroyView(view);
	view = NULL;
}
