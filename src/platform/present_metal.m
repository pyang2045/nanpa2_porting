/* macOS presenter: the window's CAMetalLayer (SDL_Metal_CreateView).
 * The 8-bit screen and its palette are uploaded as two textures and the
 * palette lookup happens in the fragment shader. */
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

static NSString *const shader_src = @
	"#include <metal_stdlib>\n"
	"using namespace metal;\n"
	"struct V { float4 pos [[position]]; float2 uv; };\n"
	"vertex V vs(uint vid [[vertex_id]]) {\n"
	"  float2 p = float2((vid << 1) & 2, vid & 2);\n"
	"  V o; o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0); o.uv = float2(p.x, 1.0 - p.y);\n"
	"  return o;\n"
	"}\n"
	"fragment float4 fs(V in [[stage_in]], texture2d<uint> idx [[texture(0)]],\n"
	"                   texture2d<float> pal [[texture(1)]]) {\n"
	"  uint2 c = uint2(in.uv * float2(idx.get_width(), idx.get_height()));\n"
	"  c = min(c, uint2(idx.get_width() - 1, idx.get_height() - 1));\n"
	"  uint i = idx.read(c).r;\n"
	"  return pal.read(uint2(i, 0));\n"
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

		MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
		rp.colorAttachments[0].texture = drawable.texture;
		rp.colorAttachments[0].loadAction = MTLLoadActionClear;
		rp.colorAttachments[0].storeAction = MTLStoreActionStore;
		rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);

		int x, y, w, h;
		present_fit(dw, dh, &x, &y, &w, &h);
		id<MTLCommandBuffer> cb = [queue commandBuffer];
		id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
		[enc setViewport:(MTLViewport){ x, y, w, h, 0, 1 }];
		[enc setRenderPipelineState:pipeline];
		[enc setFragmentTexture:index_tex atIndex:0];
		[enc setFragmentTexture:pal_tex atIndex:1];
		[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
		[enc endEncoding];
		[cb presentDrawable:drawable];
		[cb commit];
	}
}

const char *present_name(void)
{
	return name;
}

int present_window_to_screen(int wx, int wy, int *sx, int *sy)
{
	return present_map_point(window, wx, wy, sx, sy);
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
