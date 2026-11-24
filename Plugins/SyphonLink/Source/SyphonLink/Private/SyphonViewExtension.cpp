// Copyright 2026 Yoshitsugu Kosaka. All Rights Reserved.
#include "SyphonViewExtension.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RendererInterface.h"
#include "SceneView.h"
#include "TextureResource.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "RHIStaticStates.h"
#include "ScreenPass.h"
#include "CommonRenderResources.h"
#include "GlobalShader.h"
#if PLATFORM_MAC
#include "SyphonServerBridge.h"
#endif

FSyphonViewExtension::FSyphonViewExtension(const FAutoRegister &AutoReg, UWorld *InWorld)
    : FWorldSceneViewExtension(AutoReg, InWorld)
{
}

void FSyphonViewExtension::SetSource_GameThread(ESyphonCaptureMode Mode, FTextureRenderTargetResource *CaptureResource)
{
   TSharedRef<FSyphonViewExtension, ESPMode::ThreadSafe> This = StaticCastSharedRef<FSyphonViewExtension>(AsShared());
   ENQUEUE_RENDER_COMMAND(SyphonSetSource)(
       [This, Mode, CaptureResource](FRHICommandListImmediate &)
       {
          if (This->Mode_RT != Mode || This->CaptureResource_RT != CaptureResource)
          {
             This->ResetRing_RenderThread();
          }
          This->Mode_RT = Mode;
          This->CaptureResource_RT = CaptureResource;
       });
}

void FSyphonViewExtension::SetEnabled_GameThread(bool bEnabled)
{
   TSharedRef<FSyphonViewExtension, ESPMode::ThreadSafe> This = StaticCastSharedRef<FSyphonViewExtension>(AsShared());
   ENQUEUE_RENDER_COMMAND(SyphonSetEnabled)(
       [This, bEnabled](FRHICommandListImmediate &)
       {
          This->bEnabled_RT = bEnabled;
          if (!bEnabled)
          {
             This->ResetRing_RenderThread();
             This->CaptureResource_RT = nullptr;
          }
       });
}

void FSyphonViewExtension::SetOverlay_GameThread(FTextureRenderTargetResource *OverlayResource)
{
   TSharedRef<FSyphonViewExtension, ESPMode::ThreadSafe> This = StaticCastSharedRef<FSyphonViewExtension>(AsShared());
   ENQUEUE_RENDER_COMMAND(SyphonSetOverlay)(
       [This, OverlayResource](FRHICommandListImmediate &)
       {
          This->OverlayResource_RT = OverlayResource;
       });
}

void FSyphonViewExtension::ResetRing_RenderThread()
{
   for (int32 i = 0; i < RingSize; ++i)
   {
      Ring[i].SafeRelease();
      RingValid[i] = false;
   }
   RingIndex = 0;
   AllocSize = FIntPoint::ZeroValue;
   AllocFormat = PF_Unknown;
   AllocSRGB = ETextureCreateFlags::None;
   LastW = 0;
   LastH = 0;
}

static FRDGTextureRef FindOrRegister(FRDGBuilder &GraphBuilder, FRHITexture *Texture, const TCHAR *Name)
{
   if (FRDGTextureRef Existing = GraphBuilder.FindExternalTexture(Texture))
   {
      return Existing;
   }
   return GraphBuilder.RegisterExternalTexture(CreateRenderTarget(Texture, Name));
}

void FSyphonViewExtension::PostRenderViewFamily_RenderThread(FRDGBuilder &GraphBuilder, FSceneViewFamily &InViewFamily)
{
#if PLATFORM_MAC
   if (!bEnabled_RT || InViewFamily.Views.Num() == 0)
   {
      return;
   }

   // Only the game viewport of our world (FWorldSceneViewExtension already filters the world).
   const FSceneView *View0 = InViewFamily.Views[0];
   if (!View0 || !View0->bIsGameView || View0->bIsSceneCapture || View0->bIsReflectionCapture || View0->bIsPlanarReflection)
   {
      return;
   }

   // One publish per frame.
   if (LastFrameNumber == InViewFamily.FrameNumber)
   {
      return;
   }
   LastFrameNumber = InViewFamily.FrameNumber;

   if (Mode_RT == ESyphonCaptureMode::MainView)
   {
      if (!InViewFamily.RenderTarget)
      {
         return;
      }
      FRHITexture *Target = InViewFamily.RenderTarget->GetRenderTargetTexture();
      if (!Target)
      {
         return;
      }

      // The viewport texture can be larger than what is drawn (editor viewports, constrained aspect ratio):
      // take only the rendered rect of the view(s).
      FIntRect Rect = View0->UnscaledViewRect;
      for (const FSceneView *View : InViewFamily.Views)
      {
         if (View)
         {
            Rect.Union(View->UnscaledViewRect);
         }
      }

      FRDGTextureRef Source = FindOrRegister(GraphBuilder, Target, TEXT("SyphonViewFamilyTarget"));
      Rect.Clip(FIntRect(FIntPoint::ZeroValue, Source->Desc.Extent));
      // The widget overlay covers the whole viewport (like UMG on screen): take the same rect from it.
      Publish_RenderThread(GraphBuilder, View0->FeatureLevel, Source, Rect, Rect);
   }
   else
   {
      if (!CaptureResource_RT)
      {
         return;
      }
      FRHITexture *Capture = CaptureResource_RT->GetRenderTargetTexture();
      if (!Capture)
      {
         return;
      }
      FRDGTextureRef Source = FindOrRegister(GraphBuilder, Capture, TEXT("SyphonSceneCapture"));
      Publish_RenderThread(GraphBuilder, View0->FeatureLevel, Source, FIntRect(FIntPoint::ZeroValue, Source->Desc.Extent),
                           FIntRect() /* whole overlay */);
   }
#endif
}

void FSyphonViewExtension::DrawOverlay_RenderThread(FRDGBuilder &GraphBuilder, ERHIFeatureLevel::Type FeatureLevel,
                                                    FRDGTextureRef Dest, const FIntRect &InOverlayRect)
{
   if (!OverlayResource_RT)
   {
      return;
   }
   FRHITexture *OverlayRHI = OverlayResource_RT->GetRenderTargetTexture();
   if (!OverlayRHI)
   {
      return;
   }
   FRDGTextureRef Overlay = FindOrRegister(GraphBuilder, OverlayRHI, TEXT("SyphonWidgetOverlay"));

   const FIntRect Full(FIntPoint::ZeroValue, Overlay->Desc.Extent);
   FIntRect OverlayRect = InOverlayRect.IsEmpty() ? Full : InOverlayRect;
   OverlayRect.Clip(Full);
   if (OverlayRect.IsEmpty())
   {
      return;
   }

   FGlobalShaderMap *ShaderMap = GetGlobalShaderMap(FeatureLevel);
   TShaderMapRef<FScreenPassVS> VertexShader(ShaderMap);
   TShaderMapRef<FCopyRectPS> PixelShader(ShaderMap);

   FCopyRectPS::FParameters *Parameters = GraphBuilder.AllocParameters<FCopyRectPS::FParameters>();
   Parameters->InputTexture = Overlay;
   Parameters->InputSampler = TStaticSamplerState<SF_Bilinear>::GetRHI();
   Parameters->RenderTargets[0] = FRenderTargetBinding(Dest, ERenderTargetLoadAction::ELoad);

   // Slate writes premultiplied colour: out.rgb = widget.rgb + frame.rgb * (1 - widget.a). The frame's alpha is kept.
   AddDrawScreenPass(GraphBuilder, RDG_EVENT_NAME("SyphonWidgetOverlay"), FScreenPassViewInfo(FeatureLevel),
                     FScreenPassTextureViewport(Dest), FScreenPassTextureViewport(Overlay, OverlayRect),
                     VertexShader, PixelShader,
                     TStaticBlendState<CW_RGB, BO_Add, BF_One, BF_InverseSourceAlpha>::GetRHI(),
                     TStaticDepthStencilState<false, CF_Always>::GetRHI(),
                     Parameters);
}

void FSyphonViewExtension::Publish_RenderThread(FRDGBuilder &GraphBuilder, ERHIFeatureLevel::Type FeatureLevel,
                                                FRDGTextureRef Source, const FIntRect &Rect, const FIntRect &OverlayRect)
{
#if PLATFORM_MAC
   const FIntPoint Size = Rect.Size();
   if (Size.X <= 0 || Size.Y <= 0)
   {
      return;
   }

   const EPixelFormat Format = Source->Desc.Format;
   const ETextureCreateFlags SRGB = Source->Desc.Flags & ETextureCreateFlags::SRGB;
   if (Size != AllocSize || Format != AllocFormat || SRGB != AllocSRGB)
   {
      ResetRing_RenderThread();
      AllocSize = Size;
      AllocFormat = Format;
      AllocSRGB = SRGB;
   }

   const int32 Current = RingIndex;
   const int32 Previous = (RingIndex + RingSize - 1) % RingSize;
   RingIndex = (RingIndex + 1) % RingSize;

   // 1) Copy this frame into the ring.
   FRDGTextureRef Dest;
   if (Ring[Current].IsValid())
   {
      Dest = GraphBuilder.RegisterExternalTexture(Ring[Current]);
   }
   else
   {
      const FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(
          Size, Format, FClearValueBinding::Black,
          ETextureCreateFlags::ShaderResource | ETextureCreateFlags::RenderTargetable | SRGB);
      Dest = GraphBuilder.CreateTexture(Desc, TEXT("SyphonPublish"));
      Ring[Current] = GraphBuilder.ConvertToExternalTexture(Dest);
   }

   FRHICopyTextureInfo CopyInfo;
   CopyInfo.SourcePosition = FIntVector(Rect.Min.X, Rect.Min.Y, 0);
   CopyInfo.Size = FIntVector(Size.X, Size.Y, 1);
   AddCopyTexturePass(GraphBuilder, Source, Dest, CopyInfo);

   // 1b) Widgets on top of this frame's copy.
   DrawOverlay_RenderThread(GraphBuilder, FeatureLevel, Dest, OverlayRect);

   // 2) Hand the previous frame's copy to Syphon. It was submitted to the same Metal queue last frame,
   //    so Syphon's blit (committed now) is guaranteed to run after it.
   if (RingValid[Previous] && Ring[Previous].IsValid())
   {
      FTextureRHIRef PreviousTexture = Ring[Previous]->GetRHI();
      GraphBuilder.AddPass(
          RDG_EVENT_NAME("SyphonPublish"),
          ERDGPassFlags::NeverCull,
          [PreviousTexture, Size](FRHICommandListImmediate &RHICmdList)
          {
             RHICmdList.EnqueueLambda(TEXT("SyphonPublish"),
                                      [PreviousTexture, Size](FRHICommandListImmediate &)
                                      {
                                         FSyphonServerBridge::PublishTexture(PreviousTexture->GetNativeResource(), Size.X, Size.Y);
                                      });
          });
   }

   RingValid[Current] = true;
   LastW = Size.X;
   LastH = Size.Y;
#endif
}
