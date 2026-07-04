#pragma once

#include <windows.h>

#include <chrono>

#include "liquid_glass_shared.h"

class OverlayPillController {
public:
    void Reset(int width, int height, float centerX, float centerY, float scale);
    void Resize(int width, int height, float centerX, float centerY);
    void BeginDrag(float pointerX, float pointerY);
    void DragTo(float pointerX, float pointerY);
    void EndDrag();
    bool Tick();

    bool HitTest(float x, float y) const;
    RECT VisualBounds() const;
    POINT CenterScreenPoint(const RECT& virtualScreen) const;

    void FillPushConstants(LiquidGlassPushConstants& constants, float seconds) const;

    bool dragging() const { return dragging_; }
    bool animating() const { return pillAnimating_ || sideGooAmount_ > 0.0f; }
    float x() const { return pillX_; }
    float y() const { return pillY_; }

private:
    float PillSdf(float x, float y) const;
    float PillVisualDelta() const;
    void SyncVisualToTarget();
    void StartTransition();
    void PulseSideGoo(int side, float amount);
    void SetSideGoo(int side, float amount);
    void ApplyOrientation();
    void ClampPill();
    void SetOrientation(bool vertical, int dockSide);

    int width_ = 1;
    int height_ = 1;
    float scale_ = 0.88f;

    float pillX_ = 0.0f;
    float pillY_ = 0.0f;
    float pillBaseW_ = 430.0f;
    float pillBaseH_ = 150.0f;
    float pillW_ = 430.0f;
    float pillH_ = 150.0f;
    float visualPillX_ = 0.0f;
    float visualPillY_ = 0.0f;
    float visualPillW_ = 430.0f;
    float visualPillH_ = 150.0f;

    bool pillVertical_ = false;
    int pillDockSide_ = 0;
    int pillDockEdgeY_ = 0;
    bool pillAnimating_ = false;
    float sideGooAmount_ = 0.0f;
    int sideGooSide_ = 0;
    bool dragging_ = false;
    float dragOffsetX_ = 0.0f;
    float dragOffsetY_ = 0.0f;
    std::chrono::steady_clock::time_point pillAnimationUpdatedAt_{};
};
