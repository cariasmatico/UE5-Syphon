// Copyright 2026 Yoshitsugu Kosaka. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "SceneViewExtension.h"
#include "RenderGraphResources.h"
#include "SyphonServerComponent.h"
#include <atomic>

class FTextureRenderTargetResource;

/**
 * Hooks the end of the game view family render for one world and hands a frame to Syphon.
 *
 *  - Main View:     copies the final image of the game viewport (after post-processing and
 *                   upscaling, before UI/HUD/debug text) - i.e. exactly what the view target camera renders.
 *  - Scene Capture: copies the component's SceneCapture render target. Scene captures render before
 *                   the main view family, so this is the same frame, not one late.
 *
 * Frames go through a small ring of textures and Syphon publishes the copy made on the previous
 * frame, so the GPU copy is always submitted before Syphon reads it (one frame of latency).
 */
class FSyphonViewExtension : public FWorldSceneViewExtension
{
public:
   FSyphonViewExtension(const FAutoRegister &AutoReg, UWorld *InWorld);

   // Game thread: configuration is pushed to the render thread.
   void SetSource_GameThread(ESyphonCaptureMode Mode, FTextureRenderTargetResource *CaptureResource);
   void SetEnabled_GameThread(bool bEnabled);

   /** Transparent widget texture blended over each published frame (premultiplied alpha), or null for none.
    *  Main View: same size as the game viewport. Scene Capture: same size as the capture. */
   void SetOverlay_GameThread(FTextureRenderTargetResource *OverlayResource);

   /** Size of the last frame handed to Syphon (for debug display). */
   FIntPoint GetLastOutputSize() const { return FIntPoint(LastW.load(), LastH.load()); }

   virtual void PostRenderViewFamily_RenderThread(FRDGBuilder &GraphBuilder, FSceneViewFamily &InViewFamily) override;

private:
   void Publish_RenderThread(FRDGBuilder &GraphBuilder, ERHIFeatureLevel::Type FeatureLevel,
                             FRDGTextureRef Source, const FIntRect &Rect, const FIntRect &OverlayRect);
   void DrawOverlay_RenderThread(FRDGBuilder &GraphBuilder, ERHIFeatureLevel::Type FeatureLevel,
                                 FRDGTextureRef Dest, const FIntRect &OverlayRect);
   void ResetRing_RenderThread();

   static constexpr int32 RingSize = 3;

   // Render thread state
   ESyphonCaptureMode Mode_RT = ESyphonCaptureMode::MainView;
   FTextureRenderTargetResource *CaptureResource_RT = nullptr;
   FTextureRenderTargetResource *OverlayResource_RT = nullptr;
   bool bEnabled_RT = false;
   TRefCountPtr<IPooledRenderTarget> Ring[RingSize];
   bool RingValid[RingSize] = {};
   int32 RingIndex = 0;
   FIntPoint AllocSize = FIntPoint::ZeroValue;
   EPixelFormat AllocFormat = PF_Unknown;
   ETextureCreateFlags AllocSRGB = ETextureCreateFlags::None;
   uint32 LastFrameNumber = MAX_uint32;

   std::atomic<int32> LastW{0};
   std::atomic<int32> LastH{0};
};
