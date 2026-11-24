// Copyright 2026 Yoshitsugu Kosaka. All Rights Reserved.
#include "SyphonServerComponent.h"
#include "SyphonViewExtension.h"
#include "SceneViewExtension.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Camera/CameraComponent.h"
#include "Camera/CameraTypes.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/GameUserSettings.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "DynamicRHI.h"
#include "RenderDeferredCleanup.h"
#include "Blueprint/UserWidget.h"
#include "Slate/WidgetRenderer.h"
#include "Engine/UserInterfaceSettings.h"
#include "Engine/GameViewportClient.h"
#include "UnrealClient.h"
#if PLATFORM_MAC
#include "SyphonServerBridge.h"
#endif

#if PLATFORM_MAC
// Server lifetime calls go through the render thread onto the RHI thread, so they stay ordered
// with the publish calls issued by FSyphonViewExtension.
static void EnqueueSyphonStart(const FString &Name, bool bForceOpaque)
{
   ENQUEUE_RENDER_COMMAND(SyphonStart)(
       [Name, bForceOpaque](FRHICommandListImmediate &RHICmdList)
       {
          RHICmdList.EnqueueLambda(TEXT("SyphonStart"),
                                   [Name, bForceOpaque](FRHICommandListImmediate &)
                                   {
                                      void *Device = GDynamicRHI ? GDynamicRHI->RHIGetNativeDevice() : nullptr;
                                      void *Queue = GDynamicRHI ? GDynamicRHI->RHIGetNativeGraphicsQueue() : nullptr;
                                      FSyphonServerBridge::Start(Name, Device, Queue, bForceOpaque);
                                   });
       });
}

static void EnqueueSyphonStop()
{
   ENQUEUE_RENDER_COMMAND(SyphonStop)(
       [](FRHICommandListImmediate &RHICmdList)
       {
          RHICmdList.EnqueueLambda(TEXT("SyphonStop"),
                                   [](FRHICommandListImmediate &)
                                   {
                                      FSyphonServerBridge::Stop();
                                   });
       });
}
#endif

USyphonServerComponent::USyphonServerComponent()
{
   PrimaryComponentTick.bCanEverTick = true;
   // Run after cameras/pawns have moved so the Scene Capture follows this frame's camera.
   PrimaryComponentTick.TickGroup = TG_PostUpdateWork;
}

FIntPoint USyphonServerComponent::GetBaseResolution() const
{
   switch (Resolution)
   {
   case ESyphonResolution::R720p:
      return FIntPoint(1280, 720);
   case ESyphonResolution::R1440p:
      return FIntPoint(2560, 1440);
   case ESyphonResolution::R4K:
      return FIntPoint(3840, 2160);
   case ESyphonResolution::Custom:
      return FIntPoint(FMath::Clamp(CustomResolution.X, 16, 8192), FMath::Clamp(CustomResolution.Y, 16, 8192));
   case ESyphonResolution::R1080p:
   default:
      return FIntPoint(1920, 1080);
   }
}

UCameraComponent *USyphonServerComponent::FindCameraComponent() const
{
   if (!CameraActor)
   {
      return nullptr;
   }
   // UCineCameraComponent derives from UCameraComponent. Prefer an active one if the actor has several.
   TInlineComponentArray<UCameraComponent *> Cameras(CameraActor.Get());
   for (UCameraComponent *Cam : Cameras)
   {
      if (Cam && Cam->IsActive())
      {
         return Cam;
      }
   }
   return Cameras.Num() > 0 ? Cameras[0] : nullptr;
}

// ---------------------------------------------------------------------------------------------
// Public API

void USyphonServerComponent::StartServer()
{
   bEnabled = true;
   StartInternal();
}

void USyphonServerComponent::StopServer()
{
   bEnabled = false;
   StopInternal();
}

void USyphonServerComponent::SetCamera(AActor *NewCamera)
{
   CameraActor = NewCamera;
   if (bRunning && ActiveMode == ESyphonCaptureMode::MainView)
   {
      UpdateViewTarget();
   }
}

void USyphonServerComponent::SetCaptureMode(ESyphonCaptureMode NewMode)
{
   CaptureMode = NewMode;
   if (bRunning && ActiveMode != CaptureMode)
   {
      StopInternal();
      StartInternal();
   }
}

void USyphonServerComponent::SetServerName(const FString &NewName)
{
   ServerName = NewName;
#if PLATFORM_MAC
   if (bRunning) // running -> restart the Syphon server under the new name
   {
      EnqueueSyphonStop();
      EnqueueSyphonStart(ServerName, bActiveForceOpaque);
   }
#endif
}

UUserWidget *USyphonServerComponent::AddOverlayWidget(TSubclassOf<UUserWidget> WidgetClass)
{
   UWorld *World = GetWorld();
   if (!WidgetClass || !World || !World->IsGameWorld())
   {
      return nullptr;
   }
   UUserWidget *Widget = nullptr;
   if (APlayerController *PC = World->GetFirstPlayerController())
   {
      Widget = CreateWidget<UUserWidget>(PC, WidgetClass);
   }
   else
   {
      Widget = CreateWidget<UUserWidget>(World, WidgetClass);
   }
   if (Widget)
   {
      OverlayWidgets.Add(Widget);
   }
   return Widget;
}

void USyphonServerComponent::AddOverlayWidgetInstance(UUserWidget *Widget)
{
   if (IsValid(Widget))
   {
      OverlayWidgets.AddUnique(Widget);
   }
}

void USyphonServerComponent::RemoveOverlayWidget(UUserWidget *Widget)
{
   OverlayWidgets.Remove(Widget);
}

void USyphonServerComponent::ClearOverlayWidgets()
{
   OverlayWidgets.Empty();
}

TArray<UUserWidget *> USyphonServerComponent::GetOverlayWidgets() const
{
   TArray<UUserWidget *> Result;
   for (UUserWidget *Widget : OverlayWidgets)
   {
      if (IsValid(Widget))
      {
         Result.Add(Widget);
      }
   }
   return Result;
}

// ---------------------------------------------------------------------------------------------
// Lifetime

void USyphonServerComponent::StartInternal()
{
#if PLATFORM_MAC
   UWorld *World = GetWorld();
   if (bRunning || !World || !World->IsGameWorld())
   {
      return;
   }
   bRunning = true;
   ActiveMode = CaptureMode;
   bActiveForceOpaque = bForceOpaqueAlpha;

   EnqueueSyphonStart(ServerName, bActiveForceOpaque);
   ViewExtension = FSceneViewExtensions::NewExtension<FSyphonViewExtension>(World);

   FTextureRenderTargetResource *CaptureResource = nullptr;
   if (ActiveMode == ESyphonCaptureMode::SceneCapture)
   {
      StartSceneCapture();
      CaptureResource = RenderTarget ? RenderTarget->GameThread_GetRenderTargetResource() : nullptr;
   }
   else
   {
      UpdateViewTarget();
      if (bApplyResolutionToWindow)
      {
         ApplyWindowResolution();
      }
   }

   ViewExtension->SetSource_GameThread(ActiveMode, CaptureResource);
   ViewExtension->SetEnabled_GameThread(true);
#endif
}

void USyphonServerComponent::StopInternal()
{
#if PLATFORM_MAC
   if (!bRunning)
   {
      return;
   }
   bRunning = false;

   StopOverlay();
   if (ViewExtension.IsValid())
   {
      // Disable on the render thread before the capture render target goes away.
      ViewExtension->SetEnabled_GameThread(false);
      ViewExtension.Reset();
   }
   StopSceneCapture();
   EnqueueSyphonStop();
#endif
}

void USyphonServerComponent::StartSceneCapture()
{
   const FIntPoint Size = GetBaseResolution();

   RenderTarget = NewObject<UTextureRenderTarget2D>(this);
   RenderTarget->RenderTargetFormat = RTF_RGBA8_SRGB;
   RenderTarget->InitAutoFormat(Size.X, Size.Y);
   RenderTarget->TargetGamma = 2.2f;
   RenderTarget->UpdateResourceImmediate(true);

   SceneCapture = NewObject<USceneCaptureComponent2D>(GetOwner());
   SceneCapture->TextureTarget = RenderTarget;
   SceneCapture->CaptureSource = SCS_FinalColorLDR;
   SceneCapture->bCaptureEveryFrame = true;
   SceneCapture->bCaptureOnMovement = false;
   // Keeps temporal AA / TSR, auto-exposure and Lumen history between frames.
   SceneCapture->bAlwaysPersistRenderingState = true;
   SceneCapture->ShowFlags.SetMotionBlur(false);
   SceneCapture->RegisterComponent();
}

void USyphonServerComponent::StopSceneCapture()
{
   if (SceneCapture)
   {
      SceneCapture->DestroyComponent();
      SceneCapture = nullptr;
   }
   RenderTarget = nullptr;
}

void USyphonServerComponent::BeginPlay()
{
   Super::BeginPlay();
   for (const TSubclassOf<UUserWidget> &WidgetClass : OverlayWidgetClasses)
   {
      AddOverlayWidget(WidgetClass);
   }
   if (bEnabled)
   {
      StartInternal();
   }
}

void USyphonServerComponent::EndPlay(const EEndPlayReason::Type Reason)
{
   StopInternal();
   ClearOverlayWidgets();
   Super::EndPlay(Reason);
}

// ---------------------------------------------------------------------------------------------
// Per frame

void USyphonServerComponent::UpdateViewTarget()
{
   if (!bSetViewTarget || !CameraActor)
   {
      return;
   }
   if (UWorld *World = GetWorld())
   {
      if (APlayerController *PC = World->GetFirstPlayerController())
      {
         if (PC->GetViewTarget() != CameraActor)
         {
            PC->SetViewTarget(CameraActor);
         }
      }
   }
}

void USyphonServerComponent::ApplyWindowResolution()
{
   // In the editor this would change the editor's own game settings; only do it in Standalone/packaged.
   if (GIsEditor || !GEngine)
   {
      return;
   }
   if (UGameUserSettings *Settings = GEngine->GetGameUserSettings())
   {
      Settings->SetScreenResolution(GetBaseResolution());
      Settings->ApplyResolutionSettings(false);
   }
}

void USyphonServerComponent::UpdateSceneCapture(float DeltaTime)
{
   UCameraComponent *Cam = FindCameraComponent();
   if (!SceneCapture || !RenderTarget || !Cam)
   {
      return;
   }

   // Ask the camera for its full view, exactly as the player camera manager would: location, rotation,
   // FOV (CineCamera: from focal length + filmback), projection, aspect ratio and post-process settings
   // (CineCamera writes its depth of field - focus distance, aperture, sensor size - into these).
   FMinimalViewInfo View;
   Cam->GetCameraView(DeltaTime, View);

   SceneCapture->SetWorldLocationAndRotation(View.Location, View.Rotation);
   SceneCapture->ProjectionType = View.ProjectionMode;
   SceneCapture->FOVAngle = View.FOV;
   SceneCapture->OrthoWidth = View.OrthoWidth;
   SceneCapture->PostProcessSettings = View.PostProcessSettings;
   SceneCapture->PostProcessBlendWeight = View.PostProcessBlendWeight;

   FIntPoint Size = GetBaseResolution();
   if (bMatchCameraAspect && View.AspectRatio > KINDA_SMALL_NUMBER)
   {
      Size.Y = FMath::Clamp(FMath::RoundToInt(Size.X / View.AspectRatio) & ~1, 16, 8192);
   }

   if (RenderTarget->SizeX != Size.X || RenderTarget->SizeY != Size.Y)
   {
      RenderTarget->ResizeTarget(Size.X, Size.Y);
      if (ViewExtension.IsValid())
      {
         // Resizing recreates the render target resource.
         ViewExtension->SetSource_GameThread(ActiveMode, RenderTarget->GameThread_GetRenderTargetResource());
      }
   }
}

FIntPoint USyphonServerComponent::GetOverlaySize() const
{
   if (ActiveMode == ESyphonCaptureMode::SceneCapture)
   {
      return RenderTarget ? FIntPoint(RenderTarget->SizeX, RenderTarget->SizeY) : FIntPoint::ZeroValue;
   }
   // Main View: the whole game viewport, the area UMG lays widgets out in on screen.
   UWorld *World = GetWorld();
   UGameViewportClient *GameViewport = World ? World->GetGameViewport() : nullptr;
   if (GameViewport && GameViewport->Viewport)
   {
      return GameViewport->Viewport->GetSizeXY();
   }
   return FIntPoint::ZeroValue;
}

void USyphonServerComponent::StopOverlay()
{
   if (OverlayTarget && ViewExtension.IsValid())
   {
      // Ordered before the render target is released.
      ViewExtension->SetOverlay_GameThread(nullptr);
   }
   OverlayTarget = nullptr;
   if (WidgetRenderer)
   {
      BeginCleanup(WidgetRenderer);
      WidgetRenderer = nullptr;
   }
}

void USyphonServerComponent::UpdateOverlay(float DeltaTime)
{
   OverlayWidgets.RemoveAll([](const TObjectPtr<UUserWidget> &Widget) { return !IsValid(Widget); });

   const FIntPoint Size = GetOverlaySize();
   if (!bRenderWidgets || OverlayWidgets.Num() == 0 || !ViewExtension.IsValid() || Size.X <= 0 || Size.Y <= 0)
   {
      StopOverlay();
      return;
   }

   // Main View's target holds display-encoded colour in a non-sRGB texture, so draw the widgets gamma-encoded
   // (as Slate does on screen). The Scene Capture target is sRGB, so draw them linear into an sRGB texture.
   const bool bGamma = ActiveMode == ESyphonCaptureMode::MainView;
   if (WidgetRenderer && bOverlayGamma != bGamma)
   {
      StopOverlay();
   }
   if (!WidgetRenderer)
   {
      WidgetRenderer = new FWidgetRenderer(bGamma, true);
      bOverlayGamma = bGamma;
   }

   if (!OverlayTarget)
   {
      OverlayTarget = FWidgetRenderer::CreateTargetFor(FVector2D(Size), TF_Bilinear, bGamma);
      if (!OverlayTarget)
      {
         return;
      }
      ViewExtension->SetOverlay_GameThread(OverlayTarget->GameThread_GetRenderTargetResource());
   }
   else if (OverlayTarget->SizeX != Size.X || OverlayTarget->SizeY != Size.Y)
   {
      OverlayTarget->ResizeTarget(Size.X, Size.Y);
      ViewExtension->SetOverlay_GameThread(OverlayTarget->GameThread_GetRenderTargetResource());
   }

   // Same UI scale UMG uses for a viewport of this size, so the layout matches the screen in Main View.
   const float Scale = GetDefault<UUserInterfaceSettings>()->GetDPIScaleBasedOnSize(Size);

   bool bFirst = true;
   for (UUserWidget *Widget : OverlayWidgets)
   {
      // Widgets that are also on screen are already ticked by Slate this frame: don't advance their
      // Tick / Delay nodes a second time.
      const float WidgetDeltaTime = Widget->IsInViewport() ? 0.f : DeltaTime;
      WidgetRenderer->SetShouldClearTarget(bFirst);
      WidgetRenderer->DrawWidget(OverlayTarget, Widget->TakeWidget(), Scale, FVector2D(Size), WidgetDeltaTime);
      bFirst = false;
   }
}

void USyphonServerComponent::TickComponent(float DeltaTime, ELevelTick TickType,
                                           FActorComponentTickFunction *ThisTickFunction)
{
   Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
#if PLATFORM_MAC
   // Live toggles from the details panel or Blueprint property sets.
   if (bEnabled && !bRunning)
   {
      StartInternal();
   }
   else if (!bEnabled && bRunning)
   {
      StopInternal();
   }
   if (!bRunning)
   {
      return;
   }
   if (ActiveMode != CaptureMode)
   {
      StopInternal();
      StartInternal();
   }
   if (bActiveForceOpaque != bForceOpaqueAlpha)
   {
      // Restart only the Syphon server; the capture pipeline is unaffected.
      bActiveForceOpaque = bForceOpaqueAlpha;
      EnqueueSyphonStop();
      EnqueueSyphonStart(ServerName, bActiveForceOpaque);
   }

   const uint64 DebugKey = 0x5359504800000000ull + GetUniqueID();
   if (!CameraActor)
   {
      if (bShowDebug && GEngine)
      {
         GEngine->AddOnScreenDebugMessage(DebugKey, 0.f, FColor::Red, TEXT("SyphonServer: NO CAMERA SET"));
      }
      return;
   }

   if (ActiveMode == ESyphonCaptureMode::SceneCapture)
   {
      UpdateSceneCapture(DeltaTime);
   }
   else
   {
      UpdateViewTarget();
   }
   UpdateOverlay(DeltaTime);

   if (bShowDebug && GEngine)
   {
      const FIntPoint Out = ViewExtension.IsValid() ? ViewExtension->GetLastOutputSize() : FIntPoint::ZeroValue;
      GEngine->AddOnScreenDebugMessage(
          DebugKey, 0.f, FColor::Green,
          FString::Printf(TEXT("SyphonServer '%s' [%s] -> %s | %dx%d | widgets: %d"),
                          *ServerName,
                          ActiveMode == ESyphonCaptureMode::MainView ? TEXT("Main View") : TEXT("Scene Capture"),
                          *CameraActor->GetName(), Out.X, Out.Y, OverlayTarget ? OverlayWidgets.Num() : 0));
   }
#endif
}
