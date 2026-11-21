// Copyright 2026 Yoshitsugu Kosaka. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"

#if PLATFORM_MAC
/**
 * Thin wrapper around SyphonMetalServer.
 * Call these on the RHI thread (RHICmdList.EnqueueLambda) so server creation, publishing
 * and teardown are serialised with Unreal's own Metal command submission.
 */
class FSyphonServerBridge
{
public:
   /** NativeDevice / NativeQueue: Unreal's MTLDevice and MTLCommandQueue (null -> create our own). */
   static void Start(const FString &ServerName, void *NativeDevice, void *NativeQueue);
   static void PublishTexture(void *MetalTexturePtr, int Width, int Height);
   static void Stop();
};
#endif
