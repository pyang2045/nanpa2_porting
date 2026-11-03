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
static id<MTLRenderPipelineState> pipeline;
static id<MTLTexture> index_tex, pal_tex;
static char name[128];
static int crt = 1;

struct uniforms {
	float out_w, out_h; /* viewport size in drawable pixels */
	int32_t crt;
	int32_t pad;
};

static NSString *const shader_src = @
	"#include <metal_stdlib>\n"
	"using namespace metal;\n"
	"struct V { float4 pos [[position]]; float2 uv; };\n"
	"struct U { float2 out_size; int crt; int pad; };\n"
	"vertex V vs(uint vid [[vertex_id]]) {\n"
	"  float2 p = float2((vid << 1) & 2, vid & 2);\n"
	"  V o; o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0); o.uv = float2(p.x, 1.0 - p.y);\n"
	"  return o;\n"
	"}\n"
	/* one source pixel, in (roughly) linear light */
	"static float3 px(texture2d<uint> idx, texture2d<float> pal, int2 c) {\n"
	"  c = clamp(c, int2(0), int2(idx.get_width() - 1, idx.get_height() - 1));\n"
	"  float3 s = pal.read(uint2(idx.read(uint2(c)).r, 0)).rgb;\n"
	"  return s * s;\n"
	"}\n"
	"fragment float4 fs(V in [[stage_in]], texture2d<uint> idx [[texture(0)]],\n"
	"                   texture2d<float> pal [[texture(1)]], constant U &u [[buffer(0)]]) {\n"
	"  float2 src = float2(idx.get_width(), idx.get_height());\n"
	"  if (u.crt == 0) {\n"
	"    uint2 c = min(uint2(in.uv * src), uint2(src) - 1);\n"
	"    return pal.read(uint2(idx.read(c).r, 0));\n"
	"  }\n"
	/* curvature: same formula as present_crt_warp() */
	"  float2 c = in.uv * 2.0 - 1.0;\n"
	"  c *= float2(1.0 + c.y * c.y / 32.0, 1.0 + c.x * c.x / 24.0);\n"
	"  const float r = 0.04;\n"
	"  float d = length(max(abs(c) - (1.0 - r), 0.0)) - r;\n"
	"  float edge = saturate(-d * u.out_size.y * 0.5);\n"
	"  if (edge <= 0.0) return float4(0, 0, 0, 1);\n"
	"  float2 uv = c * 0.5 + 0.5;\n"
	/* soft horizontal beam between two pixels, two scanlines vertically */
	"  float2 p = uv * src - 0.5;\n"
	"  float2 b = floor(p), f = p - b;\n"
	"  int2 i = int2(b);\n"
	"  float fx = smoothstep(0.0, 1.0, f.x);\n"
	"  float3 a0 = mix(px(idx, pal, i), px(idx, pal, i + int2(1, 0)), fx);\n"
	"  float3 a1 = mix(px(idx, pal, i + int2(0, 1)), px(idx, pal, i + int2(1, 1)), fx);\n"
	"  const float3 luma = float3(0.299, 0.587, 0.114);\n"
	"  float s0 = mix(0.30, 0.45, sqrt(dot(a0, luma)));\n"
	"  float s1 = mix(0.30, 0.45, sqrt(dot(a1, luma)));\n"
	"  float w0 = exp(-0.5 * f.y * f.y / (s0 * s0));\n"
	"  float w1 = exp(-0.5 * (1.0 - f.y) * (1.0 - f.y) / (s1 * s1));\n"
	"  float3 col = a0 * w0 + a1 * w1;\n"
	/* aperture grille, faded out when there are too few output pixels */
	"  float strength = 0.18 * saturate(u.out_size.x / src.x - 1.0);\n"
	"  uint m = uint(in.pos.x) % 3u;\n"
	"  float3 mask = float3(m == 0u, m == 1u, m == 2u) * (3.0 * strength) + (1.0 - strength);\n"
	"  col *= mask;\n"
	"  float2 vv = uv * (1.0 - uv);\n"
	"  col *= pow(saturate(vv.x * vv.y * 20.0), 0.18) * 1.3;\n"
	"  return float4(sqrt(col) * edge, 1.0);\n"
	"}\n";

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
	MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
	pd.vertexFunction = [lib newFunctionWithName:@"vs"];
	pd.fragmentFunction = [lib newFunctionWithName:@"fs"];
	pd.colorAttachments[0].pixelFormat = layer.pixelFormat;
	pipeline = [device newRenderPipelineStateWithDescriptor:pd error:&err];
	if (!pipeline) {
		SDL_Log("Metal pipeline: %s", err.localizedDescription.UTF8String);
		return 0;
	}

	MTLTextureDescriptor *td = [MTLTextureDescriptor
		texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Uint
					     width:SCREEN_W
					    height:SCREEN_H
					 mipmapped:NO];
	index_tex = [device newTextureWithDescriptor:td];
	td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
								 width:256
								height:1
							     mipmapped:NO];
	pal_tex = [device newTextureWithDescriptor:td];
	snprintf(name, sizeof name, "Metal | %s", device.name.UTF8String);
	return 1;
}

/* Draw the screen into target (dw x dh), letterboxed. */
static void encode(id<MTLCommandBuffer> cb, id<MTLTexture> target, int dw, int dh)
{
	MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
	rp.colorAttachments[0].texture = target;
	rp.colorAttachments[0].loadAction = MTLLoadActionClear;
	rp.colorAttachments[0].storeAction = MTLStoreActionStore;
	rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);

	int x, y, w, h;
	present_fit(dw, dh, &x, &y, &w, &h);
	struct uniforms u = { (float)w, (float)h, crt, 0 };
	id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
	[enc setViewport:(MTLViewport){ x, y, w, h, 0, 1 }];
	[enc setRenderPipelineState:pipeline];
	[enc setFragmentTexture:index_tex atIndex:0];
	[enc setFragmentTexture:pal_tex atIndex:1];
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
	pipeline = nil;
	index_tex = nil;
	pal_tex = nil;
	queue = nil;
	device = nil;
	layer = nil;
	if (view)
		SDL_Metal_DestroyView(view);
	view = NULL;
}
