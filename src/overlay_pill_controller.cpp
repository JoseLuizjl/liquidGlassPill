#include "overlay_pill_controller.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr float kTransitionRate = 9.25f;
constexpr float kTransitionSnap = 0.35f;
constexpr float kGooDecayRate = 3.85f;
constexpr float kGooSnap = 0.015f;

float Clamp01(float value) {
    return std::clamp(value, 0.0f, 1.0f);
}

float SmoothStep(float edge0, float edge1, float value) {
    const float t = Clamp01((value - edge0) / (edge1 - edge0));
    return t * t * (3.0f - 2.0f * t);
}

float Lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

float RoundedPillSdf(float centerX, float centerY, float width, float height, float x, float y) {
    const float radius = std::min(width, height) * 0.5f;
    const float qx = std::fabs(x - centerX) - width * 0.5f + radius;
    const float qy = std::fabs(y - centerY) - height * 0.5f + radius;
    return std::min(std::max(qx, qy), 0.0f) + std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) - radius;
}

} // namespace

void OverlayPillController::Reset(int width, int height, float centerX, float centerY, float scale) {
    width_ = std::max(width, 1);
    height_ = std::max(height, 1);
    scale_ = scale;
    pillVertical_ = false;
    pillDockSide_ = 0;
    pillDockEdgeY_ = 0;
    pillAnimating_ = false;
    sideGooAmount_ = 0.0f;
    sideGooSide_ = 0;
    dragging_ = false;
    dragOffsetX_ = 0.0f;
    dragOffsetY_ = 0.0f;
    pillX_ = centerX;
    pillY_ = centerY;
    Resize(width_, height_, centerX, centerY);
    SyncVisualToTarget();
    pillAnimationUpdatedAt_ = std::chrono::steady_clock::now();
}

void OverlayPillController::Resize(int width, int height, float centerX, float centerY) {
    width_ = std::max(width, 1);
    height_ = std::max(height, 1);
    pillBaseW_ = std::min(430.0f * scale_, static_cast<float>(width_) * 0.78f);
    pillBaseH_ = std::min(150.0f * scale_, std::max(88.0f * scale_, static_cast<float>(height_) * 0.28f));
    if (pillBaseW_ < pillBaseH_ * 2.2f) {
        pillBaseW_ = pillBaseH_ * 2.2f;
    }
    pillBaseW_ = std::min(pillBaseW_, static_cast<float>(width_) * 0.92f);

    pillX_ = centerX;
    pillY_ = centerY;
    ApplyOrientation();
    if (pillVertical_) {
        pillX_ = pillDockSide_ < 0 ? pillW_ * 0.5f : static_cast<float>(width_) - pillW_ * 0.5f;
    } else if (pillDockEdgeY_ != 0) {
        pillY_ = pillDockEdgeY_ < 0 ? pillH_ * 0.5f : static_cast<float>(height_) - pillH_ * 0.5f;
    }
    ClampPill();
    SyncVisualToTarget();
}

void OverlayPillController::BeginDrag(float pointerX, float pointerY) {
    dragging_ = true;
    dragOffsetX_ = pointerX - pillX_;
    dragOffsetY_ = pointerY - pillY_;
    pillAnimationUpdatedAt_ = std::chrono::steady_clock::now();
}

void OverlayPillController::DragTo(float pointerX, float pointerY) {
    const float wantedX = pointerX - dragOffsetX_;
    const float wantedY = pointerY - dragOffsetY_;
    pillX_ = wantedX;
    pillY_ = wantedY;
    ClampPill();

    if (pillVertical_) {
        const int side = pillDockSide_ < 0 ? -1 : 1;
        const float dockX = side < 0 ? pillW_ * 0.5f : static_cast<float>(width_) - pillW_ * 0.5f;
        const float inwardDistance = side < 0 ? wantedX - dockX : dockX - wantedX;
        const float releaseDistance = pillW_ * 0.50f;
        if (inwardDistance > releaseDistance) {
            PulseSideGoo(side, 0.92f);
            SetOrientation(false, 0);
            dragOffsetX_ = pointerX - pillX_;
            dragOffsetY_ = pointerY - pillY_;
        } else {
            pillX_ = dockX;
            ClampPill();
            const float pull = SmoothStep(0.0f, 1.0f, Clamp01(inwardDistance / releaseDistance));
            SetSideGoo(side, 0.18f + pull * 0.72f);
        }
        if (!pillAnimating_) {
            SyncVisualToTarget();
        }
        return;
    }

    constexpr float edgeEpsilon = 0.5f;
    const float leftLimit = pillW_ * 0.5f;
    const float rightLimit = static_cast<float>(width_) - pillW_ * 0.5f;
    if (pillX_ <= leftLimit + edgeEpsilon) {
        SetOrientation(true, -1);
        dragOffsetX_ = pointerX - pillX_;
        dragOffsetY_ = pointerY - pillY_;
    } else if (pillX_ >= rightLimit - edgeEpsilon) {
        SetOrientation(true, 1);
        dragOffsetX_ = pointerX - pillX_;
        dragOffsetY_ = pointerY - pillY_;
    }

    if (!pillVertical_) {
        const float topLimit = pillH_ * 0.5f;
        const float bottomLimit = static_cast<float>(height_) - pillH_ * 0.5f;
        const float releaseDistance = pillH_ * 0.48f;
        if (pillDockEdgeY_ != 0) {
            const int edge = pillDockEdgeY_ < 0 ? -1 : 1;
            const int gooEdge = edge < 0 ? -2 : 2;
            const float dockY = edge < 0 ? topLimit : bottomLimit;
            const float inwardDistance = edge < 0 ? wantedY - dockY : dockY - wantedY;
            if (inwardDistance > releaseDistance) {
                PulseSideGoo(gooEdge, 0.86f);
                pillDockEdgeY_ = 0;
                pillY_ = wantedY;
                ClampPill();
            } else {
                pillY_ = dockY;
                const float pull = SmoothStep(0.0f, 1.0f, Clamp01(inwardDistance / releaseDistance));
                SetSideGoo(gooEdge, 0.18f + pull * 0.72f);
            }
        } else if (pillY_ <= topLimit + edgeEpsilon) {
            pillDockEdgeY_ = -1;
            pillY_ = topLimit;
            PulseSideGoo(-2, 0.58f);
        } else if (pillY_ >= bottomLimit - edgeEpsilon) {
            pillDockEdgeY_ = 1;
            pillY_ = bottomLimit;
            PulseSideGoo(2, 0.58f);
        }
    }

    if (!pillAnimating_) {
        SyncVisualToTarget();
    }
}

void OverlayPillController::EndDrag() {
    dragging_ = false;
    pillAnimationUpdatedAt_ = std::chrono::steady_clock::now();
}

bool OverlayPillController::Tick() {
    if (!pillAnimating_ && sideGooAmount_ <= 0.0f) {
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    const float elapsed = std::chrono::duration<float>(now - pillAnimationUpdatedAt_).count();
    const float dt = std::clamp(elapsed, 0.0f, 0.050f);
    pillAnimationUpdatedAt_ = now;

    bool changed = false;
    if (pillAnimating_) {
        const float t = 1.0f - std::exp(-kTransitionRate * dt);
        visualPillX_ = Lerp(visualPillX_, pillX_, t);
        visualPillY_ = Lerp(visualPillY_, pillY_, t);
        visualPillW_ = Lerp(visualPillW_, pillW_, t);
        visualPillH_ = Lerp(visualPillH_, pillH_, t);
        if (PillVisualDelta() <= kTransitionSnap) {
            SyncVisualToTarget();
        }
        changed = true;
    }

    if (sideGooAmount_ > 0.0f && !dragging_) {
        const float t = 1.0f - std::exp(-kGooDecayRate * dt);
        sideGooAmount_ = Lerp(sideGooAmount_, 0.0f, t);
        if (sideGooAmount_ <= kGooSnap) {
            sideGooAmount_ = 0.0f;
            sideGooSide_ = 0;
        }
        changed = true;
    }
    return changed;
}

bool OverlayPillController::HitTest(float x, float y) const {
    return PillSdf(x, y) <= 0.0f;
}

RECT OverlayPillController::VisualBounds() const {
    return {
        static_cast<LONG>(std::floor(visualPillX_ - visualPillW_ * 0.5f)),
        static_cast<LONG>(std::floor(visualPillY_ - visualPillH_ * 0.5f)),
        static_cast<LONG>(std::ceil(visualPillX_ + visualPillW_ * 0.5f)),
        static_cast<LONG>(std::ceil(visualPillY_ + visualPillH_ * 0.5f)),
    };
}

POINT OverlayPillController::CenterScreenPoint(const RECT& virtualScreen) const {
    return {
        virtualScreen.left + static_cast<LONG>(std::lround(pillX_)),
        virtualScreen.top + static_cast<LONG>(std::lround(pillY_)),
    };
}

void OverlayPillController::FillPushConstants(LiquidGlassPushConstants& constants, float seconds) const {
    constants = {};
    constants.resolution[0] = static_cast<float>(width_);
    constants.resolution[1] = static_cast<float>(height_);
    constants.time = seconds;
    constants.dragging = dragging_ ? 1.0f : 0.0f;
    constants.pillCenter[0] = visualPillX_;
    constants.pillCenter[1] = visualPillY_;
    constants.pillSize[0] = visualPillW_;
    constants.pillSize[1] = visualPillH_;
    constants.gooAmount = sideGooAmount_;
    constants.edgeDockSide = static_cast<float>(sideGooSide_);
    constants.desktopMode = 1.0f;
}

float OverlayPillController::PillSdf(float x, float y) const {
    return RoundedPillSdf(visualPillX_, visualPillY_, visualPillW_, visualPillH_, x, y);
}

float OverlayPillController::PillVisualDelta() const {
    const float positionDelta = std::max(std::fabs(visualPillX_ - pillX_), std::fabs(visualPillY_ - pillY_));
    const float sizeDelta = std::max(std::fabs(visualPillW_ - pillW_), std::fabs(visualPillH_ - pillH_));
    return std::max(positionDelta, sizeDelta);
}

void OverlayPillController::SyncVisualToTarget() {
    visualPillX_ = pillX_;
    visualPillY_ = pillY_;
    visualPillW_ = pillW_;
    visualPillH_ = pillH_;
    pillAnimating_ = false;
}

void OverlayPillController::StartTransition() {
    if (PillVisualDelta() <= kTransitionSnap) {
        SyncVisualToTarget();
        return;
    }
    pillAnimating_ = true;
    pillAnimationUpdatedAt_ = std::chrono::steady_clock::now();
}

void OverlayPillController::PulseSideGoo(int side, float amount) {
    if (side == 0) {
        return;
    }
    sideGooSide_ = std::clamp(side, -2, 2);
    sideGooAmount_ = std::max(sideGooAmount_, Clamp01(amount));
    pillAnimationUpdatedAt_ = std::chrono::steady_clock::now();
}

void OverlayPillController::SetSideGoo(int side, float amount) {
    if (side == 0) {
        return;
    }
    sideGooSide_ = std::clamp(side, -2, 2);
    sideGooAmount_ = Clamp01(amount);
    pillAnimationUpdatedAt_ = std::chrono::steady_clock::now();
}

void OverlayPillController::ApplyOrientation() {
    pillW_ = pillVertical_ ? pillBaseH_ : pillBaseW_;
    pillH_ = pillVertical_ ? pillBaseW_ : pillBaseH_;
}

void OverlayPillController::ClampPill() {
    const float halfW = pillW_ * 0.5f;
    const float halfH = pillH_ * 0.5f;
    const float maxX = std::max(halfW, static_cast<float>(width_) - halfW);
    const float maxY = std::max(halfH, static_cast<float>(height_) - halfH);
    pillX_ = std::clamp(pillX_, halfW, maxX);
    pillY_ = std::clamp(pillY_, halfH, maxY);
}

void OverlayPillController::SetOrientation(bool vertical, int dockSide) {
    pillVertical_ = vertical;
    pillDockSide_ = vertical ? (dockSide < 0 ? -1 : 1) : 0;
    pillDockEdgeY_ = 0;
    ApplyOrientation();
    if (pillVertical_) {
        pillX_ = pillDockSide_ < 0 ? pillW_ * 0.5f : static_cast<float>(width_) - pillW_ * 0.5f;
        PulseSideGoo(pillDockSide_, 0.66f);
    }
    ClampPill();
    StartTransition();
}
