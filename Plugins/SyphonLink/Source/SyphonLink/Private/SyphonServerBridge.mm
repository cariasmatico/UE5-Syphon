// Copyright 2026 Yoshitsugu Kosaka. All Rights Reserved.
#if PLATFORM_MAC
#include "SyphonServerBridge.h"
#include "Apple/PreAppleSystemHeaders.h"
#import <Syphon/Syphon.h>
#import <Metal/Metal.h>
#include "Apple/PostAppleSystemHeaders.h"

#if __has_feature(objc_arc)
#define SYPHON_RELEASE(x)
#else
#define SYPHON_RELEASE(x) [x release]
#endif

static SyphonMetalServer *GServer = nil;
static id<MTLDevice> GMetalDevice = nil;
static id<MTLCommandQueue> GQueue = nil;
static bool GOwnsDeviceAndQueue = false;

// Opaque-alpha pass: fullscreen triangle that samples the frame and writes (rgb, 1).
static id<MTLRenderPipelineState> GOpaquePipeline = nil;
static id<MTLTexture> GOpaqueTexture = nil;

static NSString *const GOpaqueShaderSource =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "struct VOut { float4 Pos [[position]]; float2 UV; };\n"
     "vertex VOut SyphonOpaqueVS(uint Id [[vertex_id]]) {\n"
     "  float2 UV = float2((Id << 1) & 2, Id & 2);\n"
     "  VOut O; O.Pos = float4(UV.x * 2.0 - 1.0, 1.0 - UV.y * 2.0, 0.0, 1.0); O.UV = UV; return O;\n"
     "}\n"
     "fragment float4 SyphonOpaquePS(VOut In [[stage_in]], texture2d<float> Tex [[texture(0)]]) {\n"
     "  constexpr sampler S(filter::nearest, address::clamp_to_edge);\n"
     "  return float4(Tex.sample(S, In.UV).rgb, 1.0);\n"
     "}\n";

static void CreateOpaquePipeline()
{
   NSError *Error = nil;
   id<MTLLibrary> Library = [GMetalDevice newLibraryWithSource:GOpaqueShaderSource options:nil error:&Error];
   if (!Library)
   {
      NSLog(@"SyphonLink: opaque-alpha shader failed to compile (%@); publishing without it.", Error);
      return;
   }
   id<MTLFunction> VS = [Library newFunctionWithName:@"SyphonOpaqueVS"];
   id<MTLFunction> PS = [Library newFunctionWithName:@"SyphonOpaquePS"];
   MTLRenderPipelineDescriptor *Desc = [[MTLRenderPipelineDescriptor alloc] init];
   Desc.vertexFunction = VS;
   Desc.fragmentFunction = PS;
   Desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
   GOpaquePipeline = [GMetalDevice newRenderPipelineStateWithDescriptor:Desc error:&Error];
   if (!GOpaquePipeline)
   {
      NSLog(@"SyphonLink: opaque-alpha pipeline failed (%@); publishing without it.", Error);
   }
   SYPHON_RELEASE(Desc);
   SYPHON_RELEASE(VS);
   SYPHON_RELEASE(PS);
   SYPHON_RELEASE(Library);
}

void FSyphonServerBridge::Start(const FString &ServerName, void *NativeDevice, void *NativeQueue, bool bForceOpaque)
{
   @autoreleasepool
   {
      if (GServer)
      {
         return;
      }

      if (NativeDevice && NativeQueue)
      {
         // Share Unreal's device and queue: command buffers on one queue execute in commit
         // order, so Syphon's blit runs after the copy Unreal has already submitted.
         GMetalDevice = (__bridge id<MTLDevice>)NativeDevice;
         GQueue = (__bridge id<MTLCommandQueue>)NativeQueue;
         GOwnsDeviceAndQueue = false;
      }
      else
      {
         GMetalDevice = MTLCreateSystemDefaultDevice();
         GQueue = [GMetalDevice newCommandQueue];
         GOwnsDeviceAndQueue = true;
      }

      if (bForceOpaque)
      {
         CreateOpaquePipeline();
      }

      NSString *Name = [NSString stringWithUTF8String:TCHAR_TO_UTF8(*ServerName)];
      GServer = [[SyphonMetalServer alloc] initWithName:Name
                                                 device:GMetalDevice
                                                options:nil];
   }
}

void FSyphonServerBridge::PublishTexture(void *MetalTexturePtr, int Width, int Height)
{
   if (!GServer || !GQueue || !MetalTexturePtr || Width <= 0 || Height <= 0)
   {
      return;
   }
   @autoreleasepool
   {
      id<MTLTexture> Texture = (__bridge id<MTLTexture>)MetalTexturePtr;
      id<MTLCommandBuffer> CB = [GQueue commandBuffer];

      if (GOpaquePipeline)
      {
         if (!GOpaqueTexture || GOpaqueTexture.width != (NSUInteger)Width || GOpaqueTexture.height != (NSUInteger)Height)
         {
            SYPHON_RELEASE(GOpaqueTexture);
            MTLTextureDescriptor *Desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                            width:Width
                                                                                           height:Height
                                                                                        mipmapped:NO];
            Desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            Desc.storageMode = MTLStorageModePrivate;
            GOpaqueTexture = [GMetalDevice newTextureWithDescriptor:Desc];
         }

         MTLRenderPassDescriptor *Pass = [MTLRenderPassDescriptor renderPassDescriptor];
         Pass.colorAttachments[0].texture = GOpaqueTexture;
         Pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
         Pass.colorAttachments[0].storeAction = MTLStoreActionStore;
         id<MTLRenderCommandEncoder> Encoder = [CB renderCommandEncoderWithDescriptor:Pass];
         [Encoder setRenderPipelineState:GOpaquePipeline];
         [Encoder setFragmentTexture:Texture atIndex:0];
         [Encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
         [Encoder endEncoding];
         Texture = GOpaqueTexture;
      }

      [GServer publishFrameTexture:Texture
                   onCommandBuffer:CB
                       imageRegion:NSMakeRect(0, 0, Width, Height)
                           flipped:YES];
      [CB commit];
   }
}

void FSyphonServerBridge::Stop()
{
   @autoreleasepool
   {
      if (GServer)
      {
         [GServer stop];
         SYPHON_RELEASE(GServer);
         GServer = nil;
      }
      SYPHON_RELEASE(GOpaqueTexture);
      GOpaqueTexture = nil;
      SYPHON_RELEASE(GOpaquePipeline);
      GOpaquePipeline = nil;
      if (GOwnsDeviceAndQueue)
      {
         SYPHON_RELEASE(GQueue);
         SYPHON_RELEASE(GMetalDevice);
      }
      GQueue = nil;
      GMetalDevice = nil;
      GOwnsDeviceAndQueue = false;
   }
}
#endif
