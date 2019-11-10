/*
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#undef LOG_TAG
#define LOG_TAG "BufferLayer"
#define ATRACE_TAG ATRACE_TAG_GRAPHICS

#include "BufferLayer.h"

#include <compositionengine/CompositionEngine.h>
#include <compositionengine/Layer.h>
#include <compositionengine/LayerCreationArgs.h>
#include <compositionengine/LayerFECompositionState.h>
#include <compositionengine/OutputLayer.h>
#include <compositionengine/impl/OutputLayerCompositionState.h>
#include <cutils/compiler.h>
#include <cutils/native_handle.h>
#include <cutils/properties.h>
#include <gui/BufferItem.h>
#include <gui/BufferQueue.h>
#include <gui/GLConsumer.h>
#include <gui/LayerDebugInfo.h>
#include <gui/Surface.h>
#include <renderengine/RenderEngine.h>
#include <ui/DebugUtils.h>
#include <utils/Errors.h>
#include <utils/Log.h>
#include <utils/NativeHandle.h>
#include <utils/StopWatch.h>
#include <utils/Trace.h>

#include <cmath>
#include <cstdlib>
#include <mutex>
#include <sstream>

#include "Colorizer.h"
#include "DisplayDevice.h"
#include "FrameTracer/FrameTracer.h"
#include "LayerRejecter.h"
#include "TimeStats/TimeStats.h"

namespace android {

BufferLayer::BufferLayer(const LayerCreationArgs& args)
      : Layer(args),
        mTextureName(args.textureName),
        mCompositionLayer{mFlinger->getCompositionEngine().createLayer(
                compositionengine::LayerCreationArgs{this})} {
    ALOGV("Creating Layer %s", getDebugName());

    mPremultipliedAlpha = !(args.flags & ISurfaceComposerClient::eNonPremultiplied);

    mPotentialCursor = args.flags & ISurfaceComposerClient::eCursorWindow;
    mProtectedByApp = args.flags & ISurfaceComposerClient::eProtectedByApp;
}

BufferLayer::~BufferLayer() {
    if (!isClone()) {
        // The original layer and the clone layer share the same texture. Therefore, only one of
        // the layers, in this case the original layer, needs to handle the deletion. The original
        // layer and the clone should be removed at the same time so there shouldn't be any issue
        // with the clone layer trying to use the deleted texture.
        mFlinger->deleteTextureAsync(mTextureName);
    }
    const int32_t layerID = getSequence();
    mFlinger->mTimeStats->onDestroy(layerID);
    mFlinger->mFrameTracer->onDestroy(layerID);
}

void BufferLayer::useSurfaceDamage() {
    if (mFlinger->mForceFullDamage) {
        surfaceDamageRegion = Region::INVALID_REGION;
    } else {
        surfaceDamageRegion = mBufferInfo.mSurfaceDamage;
    }
}

void BufferLayer::useEmptyDamage() {
    surfaceDamageRegion.clear();
}

bool BufferLayer::isOpaque(const Layer::State& s) const {
    // if we don't have a buffer or sidebandStream yet, we're translucent regardless of the
    // layer's opaque flag.
    if ((mSidebandStream == nullptr) && (mBufferInfo.mBuffer == nullptr)) {
        return false;
    }

    // if the layer has the opaque flag, then we're always opaque,
    // otherwise we use the current buffer's format.
    return ((s.flags & layer_state_t::eLayerOpaque) != 0) || getOpacityForFormat(getPixelFormat());
}

bool BufferLayer::isVisible() const {
    bool visible = !(isHiddenByPolicy()) && getAlpha() > 0.0f &&
            (mBufferInfo.mBuffer != nullptr || mSidebandStream != nullptr);
    mFlinger->mScheduler->setLayerVisibility(mSchedulerLayerHandle, visible);

    return visible;
}

bool BufferLayer::isFixedSize() const {
    return getEffectiveScalingMode() != NATIVE_WINDOW_SCALING_MODE_FREEZE;
}

bool BufferLayer::usesSourceCrop() const {
    return true;
}

static constexpr mat4 inverseOrientation(uint32_t transform) {
    const mat4 flipH(-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0, 1);
    const mat4 flipV(1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1, 0, 0, 1, 0, 1);
    const mat4 rot90(0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 1);
    mat4 tr;

    if (transform & NATIVE_WINDOW_TRANSFORM_ROT_90) {
        tr = tr * rot90;
    }
    if (transform & NATIVE_WINDOW_TRANSFORM_FLIP_H) {
        tr = tr * flipH;
    }
    if (transform & NATIVE_WINDOW_TRANSFORM_FLIP_V) {
        tr = tr * flipV;
    }
    return inverse(tr);
}

std::optional<renderengine::LayerSettings> BufferLayer::prepareClientComposition(
        compositionengine::LayerFE::ClientCompositionTargetSettings& targetSettings) {
    ATRACE_CALL();

    auto result = Layer::prepareClientComposition(targetSettings);
    if (!result) {
        return result;
    }

    if (CC_UNLIKELY(mBufferInfo.mBuffer == 0)) {
        // the texture has not been created yet, this Layer has
        // in fact never been drawn into. This happens frequently with
        // SurfaceView because the WindowManager can't know when the client
        // has drawn the first time.

        // If there is nothing under us, we paint the screen in black, otherwise
        // we just skip this update.

        // figure out if there is something below us
        Region under;
        bool finished = false;
        mFlinger->mDrawingState.traverseInZOrder([&](Layer* layer) {
            if (finished || layer == static_cast<BufferLayer const*>(this)) {
                finished = true;
                return;
            }

            under.orSelf(layer->getScreenBounds());
        });
        // if not everything below us is covered, we plug the holes!
        Region holes(targetSettings.clip.subtract(under));
        if (!holes.isEmpty()) {
            targetSettings.clearRegion.orSelf(holes);
        }
        return std::nullopt;
    }
    bool blackOutLayer = (isProtected() && !targetSettings.supportsProtectedContent) ||
            (isSecure() && !targetSettings.isSecure);
    const State& s(getDrawingState());
    auto& layer = *result;
    if (!blackOutLayer) {
        layer.source.buffer.buffer = mBufferInfo.mBuffer;
        layer.source.buffer.isOpaque = isOpaque(s);
        layer.source.buffer.fence = mBufferInfo.mFence;
        layer.source.buffer.textureName = mTextureName;
        layer.source.buffer.usePremultipliedAlpha = getPremultipledAlpha();
        layer.source.buffer.isY410BT2020 = isHdrY410();
        // TODO: we could be more subtle with isFixedSize()
        const bool useFiltering = targetSettings.needsFiltering || mNeedsFiltering || isFixedSize();

        // Query the texture matrix given our current filtering mode.
        float textureMatrix[16];
        getDrawingTransformMatrix(useFiltering, textureMatrix);

        if (getTransformToDisplayInverse()) {
            /*
             * the code below applies the primary display's inverse transform to
             * the texture transform
             */
            uint32_t transform = DisplayDevice::getPrimaryDisplayOrientationTransform();
            mat4 tr = inverseOrientation(transform);

            /**
             * TODO(b/36727915): This is basically a hack.
             *
             * Ensure that regardless of the parent transformation,
             * this buffer is always transformed from native display
             * orientation to display orientation. For example, in the case
             * of a camera where the buffer remains in native orientation,
             * we want the pixels to always be upright.
             */
            sp<Layer> p = mDrawingParent.promote();
            if (p != nullptr) {
                const auto parentTransform = p->getTransform();
                tr = tr * inverseOrientation(parentTransform.getOrientation());
            }

            // and finally apply it to the original texture matrix
            const mat4 texTransform(mat4(static_cast<const float*>(textureMatrix)) * tr);
            memcpy(textureMatrix, texTransform.asArray(), sizeof(textureMatrix));
        }

        const Rect win{getBounds()};
        float bufferWidth = getBufferSize(s).getWidth();
        float bufferHeight = getBufferSize(s).getHeight();

        // BufferStateLayers can have a "buffer size" of [0, 0, -1, -1] when no display frame has
        // been set and there is no parent layer bounds. In that case, the scale is meaningless so
        // ignore them.
        if (!getBufferSize(s).isValid()) {
            bufferWidth = float(win.right) - float(win.left);
            bufferHeight = float(win.bottom) - float(win.top);
        }

        const float scaleHeight = (float(win.bottom) - float(win.top)) / bufferHeight;
        const float scaleWidth = (float(win.right) - float(win.left)) / bufferWidth;
        const float translateY = float(win.top) / bufferHeight;
        const float translateX = float(win.left) / bufferWidth;

        // Flip y-coordinates because GLConsumer expects OpenGL convention.
        mat4 tr = mat4::translate(vec4(.5, .5, 0, 1)) * mat4::scale(vec4(1, -1, 1, 1)) *
                mat4::translate(vec4(-.5, -.5, 0, 1)) *
                mat4::translate(vec4(translateX, translateY, 0, 1)) *
                mat4::scale(vec4(scaleWidth, scaleHeight, 1.0, 1.0));

        layer.source.buffer.useTextureFiltering = useFiltering;
        layer.source.buffer.textureTransform = mat4(static_cast<const float*>(textureMatrix)) * tr;
    } else {
        // If layer is blacked out, force alpha to 1 so that we draw a black color
        // layer.
        layer.source.buffer.buffer = nullptr;
        layer.alpha = 1.0;
    }

    return result;
}

bool BufferLayer::isHdrY410() const {
    // pixel format is HDR Y410 masquerading as RGBA_1010102
    return (mBufferInfo.mDataspace == ui::Dataspace::BT2020_ITU_PQ &&
            mBufferInfo.mApi == NATIVE_WINDOW_API_MEDIA &&
            mBufferInfo.mBuffer->getPixelFormat() == HAL_PIXEL_FORMAT_RGBA_1010102);
}

void BufferLayer::latchPerFrameState(
        compositionengine::LayerFECompositionState& compositionState) const {
    Layer::latchPerFrameState(compositionState);

    // Sideband layers
    if (compositionState.sidebandStream.get()) {
        compositionState.compositionType = Hwc2::IComposerClient::Composition::SIDEBAND;
    } else {
        // Normal buffer layers
        compositionState.hdrMetadata = mBufferInfo.mHdrMetadata;
        compositionState.compositionType = mPotentialCursor
                ? Hwc2::IComposerClient::Composition::CURSOR
                : Hwc2::IComposerClient::Composition::DEVICE;
    }
}

bool BufferLayer::onPreComposition(nsecs_t refreshStartTime) {
    if (mBufferInfo.mBuffer != nullptr) {
        Mutex::Autolock lock(mFrameEventHistoryMutex);
        mFrameEventHistory.addPreComposition(mCurrentFrameNumber, refreshStartTime);
    }
    mRefreshPending = false;
    return hasReadyFrame();
}

bool BufferLayer::onPostComposition(const std::optional<DisplayId>& displayId,
                                    const std::shared_ptr<FenceTime>& glDoneFence,
                                    const std::shared_ptr<FenceTime>& presentFence,
                                    const CompositorTiming& compositorTiming) {
    // mFrameLatencyNeeded is true when a new frame was latched for the
    // composition.
    if (!mBufferInfo.mFrameLatencyNeeded) return false;

    // Update mFrameEventHistory.
    {
        Mutex::Autolock lock(mFrameEventHistoryMutex);
        mFrameEventHistory.addPostComposition(mCurrentFrameNumber, glDoneFence, presentFence,
                                              compositorTiming);
    }

    // Update mFrameTracker.
    nsecs_t desiredPresentTime = mBufferInfo.mDesiredPresentTime;
    mFrameTracker.setDesiredPresentTime(desiredPresentTime);

    const int32_t layerID = getSequence();
    mFlinger->mTimeStats->setDesiredTime(layerID, mCurrentFrameNumber, desiredPresentTime);

    std::shared_ptr<FenceTime> frameReadyFence = mBufferInfo.mFenceTime;
    if (frameReadyFence->isValid()) {
        mFrameTracker.setFrameReadyFence(std::move(frameReadyFence));
    } else {
        // There was no fence for this frame, so assume that it was ready
        // to be presented at the desired present time.
        mFrameTracker.setFrameReadyTime(desiredPresentTime);
    }

    if (presentFence->isValid()) {
        mFlinger->mTimeStats->setPresentFence(layerID, mCurrentFrameNumber, presentFence);
        mFlinger->mFrameTracer->traceFence(layerID, getCurrentBufferId(), mCurrentFrameNumber,
                                           presentFence, FrameTracer::FrameEvent::PRESENT_FENCE);
        mFrameTracker.setActualPresentFence(std::shared_ptr<FenceTime>(presentFence));
    } else if (displayId && mFlinger->getHwComposer().isConnected(*displayId)) {
        // The HWC doesn't support present fences, so use the refresh
        // timestamp instead.
        const nsecs_t actualPresentTime = mFlinger->getHwComposer().getRefreshTimestamp(*displayId);
        mFlinger->mTimeStats->setPresentTime(layerID, mCurrentFrameNumber, actualPresentTime);
        mFlinger->mFrameTracer->traceTimestamp(layerID, getCurrentBufferId(), mCurrentFrameNumber,
                                               actualPresentTime,
                                               FrameTracer::FrameEvent::PRESENT_FENCE);
        mFrameTracker.setActualPresentTime(actualPresentTime);
    }

    mFrameTracker.advanceFrame();
    mBufferInfo.mFrameLatencyNeeded = false;
    return true;
}

bool BufferLayer::latchBuffer(bool& recomputeVisibleRegions, nsecs_t latchTime,
                              nsecs_t expectedPresentTime) {
    ATRACE_CALL();

    bool refreshRequired = latchSidebandStream(recomputeVisibleRegions);

    if (refreshRequired) {
        return refreshRequired;
    }

    if (!hasReadyFrame()) {
        return false;
    }

    // if we've already called updateTexImage() without going through
    // a composition step, we have to skip this layer at this point
    // because we cannot call updateTeximage() without a corresponding
    // compositionComplete() call.
    // we'll trigger an update in onPreComposition().
    if (mRefreshPending) {
        return false;
    }

    // If the head buffer's acquire fence hasn't signaled yet, return and
    // try again later
    if (!fenceHasSignaled()) {
        ATRACE_NAME("!fenceHasSignaled()");
        mFlinger->signalLayerUpdate();
        return false;
    }

    // Capture the old state of the layer for comparisons later
    const State& s(getDrawingState());
    const bool oldOpacity = isOpaque(s);

    BufferInfo oldBufferInfo = mBufferInfo;

    if (!allTransactionsSignaled(expectedPresentTime)) {
        mFlinger->setTransactionFlags(eTraversalNeeded);
        return false;
    }

    status_t err = updateTexImage(recomputeVisibleRegions, latchTime, expectedPresentTime);
    if (err != NO_ERROR) {
        return false;
    }

    err = updateActiveBuffer();
    if (err != NO_ERROR) {
        return false;
    }

    err = updateFrameNumber(latchTime);
    if (err != NO_ERROR) {
        return false;
    }

    gatherBufferInfo();

    mRefreshPending = true;
    mBufferInfo.mFrameLatencyNeeded = true;
    if (oldBufferInfo.mBuffer == nullptr) {
        // the first time we receive a buffer, we need to trigger a
        // geometry invalidation.
        recomputeVisibleRegions = true;
    }

    if ((mBufferInfo.mCrop != oldBufferInfo.mCrop) ||
        (mBufferInfo.mTransform != oldBufferInfo.mTransform) ||
        (mBufferInfo.mScaleMode != oldBufferInfo.mScaleMode) ||
        (mBufferInfo.mTransformToDisplayInverse != oldBufferInfo.mTransformToDisplayInverse)) {
        recomputeVisibleRegions = true;
    }

    if (oldBufferInfo.mBuffer != nullptr) {
        uint32_t bufWidth = mBufferInfo.mBuffer->getWidth();
        uint32_t bufHeight = mBufferInfo.mBuffer->getHeight();
        if (bufWidth != uint32_t(oldBufferInfo.mBuffer->width) ||
            bufHeight != uint32_t(oldBufferInfo.mBuffer->height)) {
            recomputeVisibleRegions = true;
        }
    }

    if (oldOpacity != isOpaque(s)) {
        recomputeVisibleRegions = true;
    }

    // Remove any sync points corresponding to the buffer which was just
    // latched
    {
        Mutex::Autolock lock(mLocalSyncPointMutex);
        auto point = mLocalSyncPoints.begin();
        while (point != mLocalSyncPoints.end()) {
            if (!(*point)->frameIsAvailable() || !(*point)->transactionIsApplied()) {
                // This sync point must have been added since we started
                // latching. Don't drop it yet.
                ++point;
                continue;
            }

            if ((*point)->getFrameNumber() <= mCurrentFrameNumber) {
                std::stringstream ss;
                ss << "Dropping sync point " << (*point)->getFrameNumber();
                ATRACE_NAME(ss.str().c_str());
                point = mLocalSyncPoints.erase(point);
            } else {
                ++point;
            }
        }
    }

    return true;
}

// transaction
void BufferLayer::notifyAvailableFrames(nsecs_t expectedPresentTime) {
    const auto headFrameNumber = getHeadFrameNumber(expectedPresentTime);
    const bool headFenceSignaled = fenceHasSignaled();
    const bool presentTimeIsCurrent = framePresentTimeIsCurrent(expectedPresentTime);
    Mutex::Autolock lock(mLocalSyncPointMutex);
    for (auto& point : mLocalSyncPoints) {
        if (headFrameNumber >= point->getFrameNumber() && headFenceSignaled &&
            presentTimeIsCurrent) {
            point->setFrameAvailable();
            sp<Layer> requestedSyncLayer = point->getRequestedSyncLayer();
            if (requestedSyncLayer) {
                // Need to update the transaction flag to ensure the layer's pending transaction
                // gets applied.
                requestedSyncLayer->setTransactionFlags(eTransactionNeeded);
            }
        }
    }
}

bool BufferLayer::hasReadyFrame() const {
    return hasFrameUpdate() || getSidebandStreamChanged() || getAutoRefresh();
}

uint32_t BufferLayer::getEffectiveScalingMode() const {
    if (mOverrideScalingMode >= 0) {
        return mOverrideScalingMode;
    }

    return mBufferInfo.mScaleMode;
}

bool BufferLayer::isProtected() const {
    const sp<GraphicBuffer>& buffer(mBufferInfo.mBuffer);
    return (buffer != 0) && (buffer->getUsage() & GRALLOC_USAGE_PROTECTED);
}

bool BufferLayer::latchUnsignaledBuffers() {
    static bool propertyLoaded = false;
    static bool latch = false;
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    if (!propertyLoaded) {
        char value[PROPERTY_VALUE_MAX] = {};
        property_get("debug.sf.latch_unsignaled", value, "0");
        latch = atoi(value);
        propertyLoaded = true;
    }
    return latch;
}

// h/w composer set-up
bool BufferLayer::allTransactionsSignaled(nsecs_t expectedPresentTime) {
    const auto headFrameNumber = getHeadFrameNumber(expectedPresentTime);
    bool matchingFramesFound = false;
    bool allTransactionsApplied = true;
    Mutex::Autolock lock(mLocalSyncPointMutex);

    for (auto& point : mLocalSyncPoints) {
        if (point->getFrameNumber() > headFrameNumber) {
            break;
        }
        matchingFramesFound = true;

        if (!point->frameIsAvailable()) {
            // We haven't notified the remote layer that the frame for
            // this point is available yet. Notify it now, and then
            // abort this attempt to latch.
            point->setFrameAvailable();
            allTransactionsApplied = false;
            break;
        }

        allTransactionsApplied = allTransactionsApplied && point->transactionIsApplied();
    }
    return !matchingFramesFound || allTransactionsApplied;
}

// As documented in libhardware header, formats in the range
// 0x100 - 0x1FF are specific to the HAL implementation, and
// are known to have no alpha channel
// TODO: move definition for device-specific range into
// hardware.h, instead of using hard-coded values here.
#define HARDWARE_IS_DEVICE_FORMAT(f) ((f) >= 0x100 && (f) <= 0x1FF)

bool BufferLayer::getOpacityForFormat(uint32_t format) {
    if (HARDWARE_IS_DEVICE_FORMAT(format)) {
        return true;
    }
    switch (format) {
        case HAL_PIXEL_FORMAT_RGBA_8888:
        case HAL_PIXEL_FORMAT_BGRA_8888:
        case HAL_PIXEL_FORMAT_RGBA_FP16:
        case HAL_PIXEL_FORMAT_RGBA_1010102:
            return false;
    }
    // in all other case, we have no blending (also for unknown formats)
    return true;
}

bool BufferLayer::needsFiltering(const sp<const DisplayDevice>& displayDevice) const {
    // If we are not capturing based on the state of a known display device,
    // just return false.
    if (displayDevice == nullptr) {
        return false;
    }

    const auto outputLayer = findOutputLayerForDisplay(displayDevice);
    if (outputLayer == nullptr) {
        return false;
    }

    // We need filtering if the sourceCrop rectangle size does not match the
    // displayframe rectangle size (not a 1:1 render)
    const auto& compositionState = outputLayer->getState();
    const auto displayFrame = compositionState.displayFrame;
    const auto sourceCrop = compositionState.sourceCrop;
    return sourceCrop.getHeight() != displayFrame.getHeight() ||
            sourceCrop.getWidth() != displayFrame.getWidth();
}

uint64_t BufferLayer::getHeadFrameNumber(nsecs_t expectedPresentTime) const {
    if (hasFrameUpdate()) {
        return getFrameNumber(expectedPresentTime);
    } else {
        return mCurrentFrameNumber;
    }
}

Rect BufferLayer::getBufferSize(const State& s) const {
    // If we have a sideband stream, or we are scaling the buffer then return the layer size since
    // we cannot determine the buffer size.
    if ((s.sidebandStream != nullptr) ||
        (getEffectiveScalingMode() != NATIVE_WINDOW_SCALING_MODE_FREEZE)) {
        return Rect(getActiveWidth(s), getActiveHeight(s));
    }

    if (mBufferInfo.mBuffer == nullptr) {
        return Rect::INVALID_RECT;
    }

    uint32_t bufWidth = mBufferInfo.mBuffer->getWidth();
    uint32_t bufHeight = mBufferInfo.mBuffer->getHeight();

    // Undo any transformations on the buffer and return the result.
    if (mBufferInfo.mTransform & ui::Transform::ROT_90) {
        std::swap(bufWidth, bufHeight);
    }

    if (getTransformToDisplayInverse()) {
        uint32_t invTransform = DisplayDevice::getPrimaryDisplayOrientationTransform();
        if (invTransform & ui::Transform::ROT_90) {
            std::swap(bufWidth, bufHeight);
        }
    }

    return Rect(bufWidth, bufHeight);
}

std::shared_ptr<compositionengine::Layer> BufferLayer::getCompositionLayer() const {
    return mCompositionLayer;
}

FloatRect BufferLayer::computeSourceBounds(const FloatRect& parentBounds) const {
    const State& s(getDrawingState());

    // If we have a sideband stream, or we are scaling the buffer then return the layer size since
    // we cannot determine the buffer size.
    if ((s.sidebandStream != nullptr) ||
        (getEffectiveScalingMode() != NATIVE_WINDOW_SCALING_MODE_FREEZE)) {
        return FloatRect(0, 0, getActiveWidth(s), getActiveHeight(s));
    }

    if (mBufferInfo.mBuffer == nullptr) {
        return parentBounds;
    }

    uint32_t bufWidth = mBufferInfo.mBuffer->getWidth();
    uint32_t bufHeight = mBufferInfo.mBuffer->getHeight();

    // Undo any transformations on the buffer and return the result.
    if (mBufferInfo.mTransform & ui::Transform::ROT_90) {
        std::swap(bufWidth, bufHeight);
    }

    if (getTransformToDisplayInverse()) {
        uint32_t invTransform = DisplayDevice::getPrimaryDisplayOrientationTransform();
        if (invTransform & ui::Transform::ROT_90) {
            std::swap(bufWidth, bufHeight);
        }
    }

    return FloatRect(0, 0, bufWidth, bufHeight);
}

void BufferLayer::latchAndReleaseBuffer() {
    mRefreshPending = false;
    if (hasReadyFrame()) {
        bool ignored = false;
        latchBuffer(ignored, systemTime(), 0 /* expectedPresentTime */);
    }
    releasePendingBuffer(systemTime());
}

PixelFormat BufferLayer::getPixelFormat() const {
    return mBufferInfo.mPixelFormat;
}

bool BufferLayer::getTransformToDisplayInverse() const {
    return mBufferInfo.mTransformToDisplayInverse;
}

Rect BufferLayer::getBufferCrop() const {
    // this is the crop rectangle that applies to the buffer
    // itself (as opposed to the window)
    if (!mBufferInfo.mCrop.isEmpty()) {
        // if the buffer crop is defined, we use that
        return mBufferInfo.mCrop;
    } else if (mBufferInfo.mBuffer != nullptr) {
        // otherwise we use the whole buffer
        return mBufferInfo.mBuffer->getBounds();
    } else {
        // if we don't have a buffer yet, we use an empty/invalid crop
        return Rect();
    }
}

uint32_t BufferLayer::getBufferTransform() const {
    return mBufferInfo.mTransform;
}

ui::Dataspace BufferLayer::getDataSpace() const {
    return mBufferInfo.mDataspace;
}

ui::Dataspace BufferLayer::translateDataspace(ui::Dataspace dataspace) {
    ui::Dataspace updatedDataspace = dataspace;
    // translate legacy dataspaces to modern dataspaces
    switch (dataspace) {
        case ui::Dataspace::SRGB:
            updatedDataspace = ui::Dataspace::V0_SRGB;
            break;
        case ui::Dataspace::SRGB_LINEAR:
            updatedDataspace = ui::Dataspace::V0_SRGB_LINEAR;
            break;
        case ui::Dataspace::JFIF:
            updatedDataspace = ui::Dataspace::V0_JFIF;
            break;
        case ui::Dataspace::BT601_625:
            updatedDataspace = ui::Dataspace::V0_BT601_625;
            break;
        case ui::Dataspace::BT601_525:
            updatedDataspace = ui::Dataspace::V0_BT601_525;
            break;
        case ui::Dataspace::BT709:
            updatedDataspace = ui::Dataspace::V0_BT709;
            break;
        default:
            break;
    }

    return updatedDataspace;
}

sp<GraphicBuffer> BufferLayer::getBuffer() const {
    return mBufferInfo.mBuffer;
}

void BufferLayer::getDrawingTransformMatrix(bool filteringEnabled, float outMatrix[16]) {
    GLConsumer::computeTransformMatrix(outMatrix, mBufferInfo.mBuffer, mBufferInfo.mCrop,
                                       mBufferInfo.mTransform, filteringEnabled);
}

void BufferLayer::setInitialValuesForClone(const sp<Layer>& clonedFrom) {
    Layer::setInitialValuesForClone(clonedFrom);

    sp<BufferLayer> bufferClonedFrom = static_cast<BufferLayer*>(clonedFrom.get());
    mPremultipliedAlpha = bufferClonedFrom->mPremultipliedAlpha;
    mPotentialCursor = bufferClonedFrom->mPotentialCursor;
    mProtectedByApp = bufferClonedFrom->mProtectedByApp;

    updateCloneBufferInfo();
}

void BufferLayer::updateCloneBufferInfo() {
    if (!isClone() || !isClonedFromAlive()) {
        return;
    }

    sp<BufferLayer> clonedFrom = static_cast<BufferLayer*>(getClonedFrom().get());
    mBufferInfo = clonedFrom->mBufferInfo;
    mSidebandStream = clonedFrom->mSidebandStream;
    surfaceDamageRegion = clonedFrom->surfaceDamageRegion;
    mCurrentFrameNumber = clonedFrom->mCurrentFrameNumber.load();
    mPreviousFrameNumber = clonedFrom->mPreviousFrameNumber;

    // After buffer info is updated, the drawingState from the real layer needs to be copied into
    // the cloned. This is because some properties of drawingState can change when latchBuffer is
    // called. However, copying the drawingState would also overwrite the cloned layer's relatives.
    // Therefore, temporarily store the relatives so they can be set in the cloned drawingState
    // again.
    wp<Layer> tmpZOrderRelativeOf = mDrawingState.zOrderRelativeOf;
    SortedVector<wp<Layer>> tmpZOrderRelatives = mDrawingState.zOrderRelatives;
    mDrawingState = clonedFrom->mDrawingState;
    // TODO: (b/140756730) Ignore input for now since InputDispatcher doesn't support multiple
    // InputWindows per client token yet.
    mDrawingState.inputInfo.token = nullptr;
    mDrawingState.zOrderRelativeOf = tmpZOrderRelativeOf;
    mDrawingState.zOrderRelatives = tmpZOrderRelatives;
}

} // namespace android

#if defined(__gl_h_)
#error "don't include gl/gl.h in this file"
#endif

#if defined(__gl2_h_)
#error "don't include gl2/gl2.h in this file"
#endif