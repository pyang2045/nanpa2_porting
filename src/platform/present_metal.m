/* macOS presenter: the window's CAMetalLayer (SDL_Metal_CreateView).
 * The 8-bit screen and its palette are uploaded as two textures and the
 * palette lookup happens in the fragment shader, which also simulates a CRT
 * monitor: barrel curvature with rounded corners, Gaussian scanlines whose
 * beam widens with brightness, an aperture-grille mask and a vignette. */
#include "present_common.h"

#include <SDL_metal.h>
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

static SDL_Window *window;
static SDL_MetalView view;
static CAMetalLayer *layer;
static id<MTLDevice> device;
static id<MTLCommandQueue> queue;
static id<MTLRenderPipelineState> pipe_lin, pipe_blur, pipe_crt;
static id<MTLTexture> index_tex, pal_tex, rgb_tex, lin_tex, blur_tex, glow_tex;
static int rgb_mode; /* the last frame came from present_frame_rgb */
static id<MTLSamplerState> linear_sampler;
static char name[128];
static int crt = 1;

struct uniforms {
	float out_w, out_h; /* viewport size in drawable pixels */
	int32_t crt;
	int32_t rgb; /* read rgb_tex instead of index + palette */
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
	"struct U { float2 out_size; int crt; int rgb; };\n"
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
		rgb_mode = 0;
		show();
	}
}

void present_frame_rgb(const uint32_t *xrgb)
{
	@autoreleasepool {
		/* alpha is ignored by the shaders, so 0x00RRGGBB uploads as BGRA */
		[rgb_tex replaceRegion:MTLRegionMake2D(0, 0, SCREEN_W, SCREEN_H)
			   mipmapLevel:0
			     withBytes:xrgb
			   bytesPerRow:SCREEN_W * 4];
		rgb_mode = 1;
		show();
	}
}

static void show(void)
{
	@autoreleasepool {
		int dw, dh;
		SDL_Metal_GetDrawableSize(window, &dw, &dh);
		if (dw <= 0 || dh <= 0)
			return;
		/* nextDrawable blocks up to its 1 s timeout while the window is
		 * covered, which would stall the game; skip until it is visible.
		 * The textures hold the frame, so the next call shows it. */
		NSWindow *nsw = ((__bridge NSView *)view).window;
		if (nsw && !(nsw.occlusionState & NSWindowOcclusionStateVisible))
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
	queue = nil;
	device = nil;
	layer = nil;
	if (view)
		SDL_Metal_DestroyView(view);
	view = NULL;
}
