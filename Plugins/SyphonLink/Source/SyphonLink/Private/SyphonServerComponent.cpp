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
   if (bEnabled)
   {
      StartInternal();
   }
}

void USyphonServerComponent::EndPlay(const EEndPlayReason::Type Reason)
{
   StopInternal();
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

   if (bShowDebug && GEngine)
   {
      const FIntPoint Out = ViewExtension.IsValid() ? ViewExtension->GetLastOutputSize() : FIntPoint::ZeroValue;
      GEngine->AddOnScreenDebugMessage(
          DebugKey, 0.f, FColor::Green,
          FString::Printf(TEXT("SyphonServer '%s' [%s] -> %s | %dx%d"),
                          *ServerName,
                          ActiveMode == ESyphonCaptureMode::MainView ? TEXT("Main View") : TEXT("Scene Capture"),
                          *CameraActor->GetName(), Out.X, Out.Y));
   }
#endif
}
