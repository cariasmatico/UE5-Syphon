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

void FSyphonServerBridge::Start(const FString &ServerName, void *NativeDevice, void *NativeQueue)
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
