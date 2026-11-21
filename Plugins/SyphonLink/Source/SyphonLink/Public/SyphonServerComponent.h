// Copyright 2026 Yoshitsugu Kosaka. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "SyphonServerComponent.generated.h"

UENUM(BlueprintType)
enum class ESyphonCaptureMode : uint8
{
   /** Publish the game viewport's final image. The Camera Actor becomes the player's view target,
    *  so every camera setting (lens, filmback, DOF, exposure, post-process, aspect ratio) applies exactly.
    *  Output size = game viewport size. No extra render cost. */
   MainView UMETA(DisplayName = "Main View (exact camera)"),

   /** Render the Camera Actor with a separate Scene Capture that copies all of the camera's settings.
    *  Output size is independent of the window and the player's view is left alone. Costs a second render. */
   SceneCapture UMETA(DisplayName = "Scene Capture (independent)")
};

UENUM(BlueprintType)
enum class ESyphonResolution : uint8
{
   R720p UMETA(DisplayName = "1280 x 720"),
   R1080p UMETA(DisplayName = "1920 x 1080"),
   R1440p UMETA(DisplayName = "2560 x 1440"),
   R4K UMETA(DisplayName = "3840 x 2160"),
   Custom UMETA(DisplayName = "Custom")
};

UCLASS(ClassGroup = (Rendering), meta = (BlueprintSpawnableComponent))
class SYPHONLINK_API USyphonServerComponent : public UActorComponent
{
   GENERATED_BODY()
public:
   USyphonServerComponent();

   UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Syphon")
   FString ServerName = TEXT("UE5 Output");

   /** Start/stop publishing. Can be toggled while playing. */
   UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Syphon")
   bool bEnabled = true;

   /** How the camera is rendered for Syphon. Can be changed while playing. */
   UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Syphon")
   ESyphonCaptureMode CaptureMode = ESyphonCaptureMode::MainView;

   UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Syphon")
   TObjectPtr<AActor> CameraActor;

   /** Main View: keep the Camera Actor as player 0's view target (re-applied if something else changes it).
    *  Turn off if your own Blueprint logic drives the view target / camera blends. */
   UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Syphon|Main View",
             meta = (EditCondition = "CaptureMode == ESyphonCaptureMode::MainView", EditConditionHides))
   bool bSetViewTarget = true;

   /** Main View: in Standalone / packaged builds, resize the game window to Resolution on start.
    *  (In the editor, set Editor Preferences > Level Editor > Play > New Window Size instead.) */
   UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Syphon|Main View",
             meta = (EditCondition = "CaptureMode == ESyphonCaptureMode::MainView", EditConditionHides))
   bool bApplyResolutionToWindow = false;

   /** Scene Capture: output resolution. Main View: window size used when Apply Resolution To Window is on. */
   UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Syphon|Resolution")
   ESyphonResolution Resolution = ESyphonResolution::R1080p;

   UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Syphon|Resolution",
             meta = (EditCondition = "Resolution == ESyphonResolution::Custom", EditConditionHides, ClampMin = "16", ClampMax = "8192"))
   FIntPoint CustomResolution = FIntPoint(1920, 1080);

   /** Scene Capture: keep the width from Resolution and derive the height from the camera's aspect ratio
    *  (CineCamera filmback / Camera aspect ratio), so the image is never stretched or cropped. */
   UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Syphon|Resolution",
             meta = (EditCondition = "CaptureMode == ESyphonCaptureMode::SceneCapture", EditConditionHides))
   bool bMatchCameraAspect = true;

   UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Syphon")
   bool bShowDebug = false;

   UFUNCTION(BlueprintCallable, Category = "Syphon")
   void StartServer();
   UFUNCTION(BlueprintCallable, Category = "Syphon")
   void StopServer();

   /** Retarget the output to another camera. In Main View mode this also sets player 0's view target. */
   UFUNCTION(BlueprintCallable, Category = "Syphon")
   void SetCamera(AActor *NewCamera);

   UFUNCTION(BlueprintCallable, Category = "Syphon")
   void SetCaptureMode(ESyphonCaptureMode NewMode);

   UFUNCTION(BlueprintCallable, Category = "Syphon")
   void SetServerName(const FString &NewName);

   UFUNCTION(BlueprintPure, Category = "Syphon")
   bool IsServerRunning() const { return bRunning; }

   virtual void BeginPlay() override;
   virtual void EndPlay(const EEndPlayReason::Type Reason) override;
   virtual void TickComponent(float DeltaTime, ELevelTick TickType,
                              FActorComponentTickFunction *ThisTickFunction) override;

private:
   void StartInternal();
   void StopInternal();
   FIntPoint GetBaseResolution() const;
   class UCameraComponent *FindCameraComponent() const;
   void StartSceneCapture();
   void StopSceneCapture();
   void UpdateSceneCapture(float DeltaTime);
   void UpdateViewTarget();
   void ApplyWindowResolution();

   bool bRunning = false;
   ESyphonCaptureMode ActiveMode = ESyphonCaptureMode::MainView;

   TSharedPtr<class FSyphonViewExtension, ESPMode::ThreadSafe> ViewExtension;

   UPROPERTY()
   TObjectPtr<class USceneCaptureComponent2D> SceneCapture;
   UPROPERTY()
   TObjectPtr<class UTextureRenderTarget2D> RenderTarget;
};
