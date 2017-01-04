/*
 * Copyright (C) 2016 The Android Open Source Project
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

#define LOG_TAG "EffectHalHidl"
//#define LOG_NDEBUG 0

#include <media/EffectsFactoryApi.h>
#include <utils/Log.h>

#include "ConversionHelperHidl.h"
#include "EffectBufferHalHidl.h"
#include "EffectHalHidl.h"
#include "HidlUtils.h"

using ::android::hardware::audio::effect::V2_0::AudioBuffer;
using ::android::hardware::audio::effect::V2_0::MessageQueueFlagBits;
using ::android::hardware::audio::effect::V2_0::Result;
using ::android::hardware::hidl_vec;
using ::android::hardware::MQDescriptorSync;
using ::android::hardware::Return;
using ::android::hardware::Status;

namespace android {

EffectHalHidl::EffectHalHidl(const sp<IEffect>& effect, uint64_t effectId)
        : mEffect(effect), mEffectId(effectId), mBuffersChanged(true) {
}

EffectHalHidl::~EffectHalHidl() {
    close();
}

// static
void EffectHalHidl::effectDescriptorToHal(
        const EffectDescriptor& descriptor, effect_descriptor_t* halDescriptor) {
    HidlUtils::uuidToHal(descriptor.type, &halDescriptor->type);
    HidlUtils::uuidToHal(descriptor.uuid, &halDescriptor->uuid);
    halDescriptor->flags = static_cast<uint32_t>(descriptor.flags);
    halDescriptor->cpuLoad = descriptor.cpuLoad;
    halDescriptor->memoryUsage = descriptor.memoryUsage;
    memcpy(halDescriptor->name, descriptor.name.data(), descriptor.name.size());
    memcpy(halDescriptor->implementor,
            descriptor.implementor.data(), descriptor.implementor.size());
}

// static
status_t EffectHalHidl::analyzeResult(const Result& result) {
    switch (result) {
        case Result::OK: return OK;
        case Result::INVALID_ARGUMENTS: return BAD_VALUE;
        case Result::INVALID_STATE: return NOT_ENOUGH_DATA;
        case Result::NOT_INITIALIZED: return NO_INIT;
        case Result::NOT_SUPPORTED: return INVALID_OPERATION;
        case Result::RESULT_TOO_BIG: return NO_MEMORY;
        default: return NO_INIT;
    }
}

status_t EffectHalHidl::setInBuffer(const sp<EffectBufferHalInterface>& buffer) {
    if (mInBuffer == 0 || buffer->audioBuffer() != mInBuffer->audioBuffer()) {
        mBuffersChanged = true;
    }
    mInBuffer = buffer;
    return OK;
}

status_t EffectHalHidl::setOutBuffer(const sp<EffectBufferHalInterface>& buffer) {
    if (mOutBuffer == 0 || buffer->audioBuffer() != mOutBuffer->audioBuffer()) {
        mBuffersChanged = true;
    }
    mOutBuffer = buffer;
    return OK;
}

status_t EffectHalHidl::process() {
    return processImpl(static_cast<uint32_t>(MessageQueueFlagBits::REQUEST_PROCESS));
}

status_t EffectHalHidl::processReverse() {
    return processImpl(static_cast<uint32_t>(MessageQueueFlagBits::REQUEST_PROCESS_REVERSE));
}

status_t EffectHalHidl::prepareForProcessing() {
    std::unique_ptr<StatusMQ> tempStatusMQ;
    Result retval;
    Return<void> ret = mEffect->prepareForProcessing(
            [&](Result r, const MQDescriptorSync<Result>& statusMQ) {
                retval = r;
                if (retval == Result::OK) {
                    tempStatusMQ.reset(new StatusMQ(statusMQ));
                    if (tempStatusMQ->isValid() && tempStatusMQ->getEventFlagWord()) {
                        EventFlag::createEventFlag(tempStatusMQ->getEventFlagWord(), &mEfGroup);
                    }
                }
            });
    if (!ret.isOk() || retval != Result::OK) {
        return ret.isOk() ? analyzeResult(retval) : FAILED_TRANSACTION;
    }
    if (!tempStatusMQ || !tempStatusMQ->isValid() || !mEfGroup) {
        ALOGE_IF(!tempStatusMQ, "Failed to obtain status message queue for effects");
        ALOGE_IF(tempStatusMQ && !tempStatusMQ->isValid(),
                "Status message queue for effects is invalid");
        ALOGE_IF(!mEfGroup, "Event flag creation for effects failed");
        return NO_INIT;
    }
    mStatusMQ = std::move(tempStatusMQ);
    return OK;
}

status_t EffectHalHidl::processImpl(uint32_t mqFlag) {
    if (mEffect == 0 || mInBuffer == 0 || mOutBuffer == 0) return NO_INIT;
    status_t status;
    if (!mStatusMQ && (status = prepareForProcessing()) != OK) {
        return status;
    }
    if (mBuffersChanged && (status = setProcessBuffers()) != OK) {
        return status;
    }
    // The data is already in the buffers, just need to flush it and wake up the server side.
    std::atomic_thread_fence(std::memory_order_release);
    mEfGroup->wake(mqFlag);
    uint32_t efState = 0;
retry:
    status_t ret = mEfGroup->wait(
            static_cast<uint32_t>(MessageQueueFlagBits::DONE_PROCESSING), &efState, NS_PER_SEC);
    if (efState & static_cast<uint32_t>(MessageQueueFlagBits::DONE_PROCESSING)) {
        Result retval = Result::NOT_INITIALIZED;
        mStatusMQ->read(&retval);
        if (retval == Result::OK || retval == Result::INVALID_STATE) {
            // Sync back the changed contents of the buffer.
            std::atomic_thread_fence(std::memory_order_acquire);
        }
        return analyzeResult(retval);
    }
    if (ret == -EAGAIN) {
        // This normally retries no more than once.
        goto retry;
    }
    return ret;
}

status_t EffectHalHidl::setProcessBuffers() {
    Return<Result> ret = mEffect->setProcessBuffers(
            reinterpret_cast<EffectBufferHalHidl*>(mInBuffer.get())->hidlBuffer(),
            reinterpret_cast<EffectBufferHalHidl*>(mOutBuffer.get())->hidlBuffer());
    if (ret.isOk() && ret == Result::OK) {
        mBuffersChanged = false;
        return OK;
    }
    return ret.isOk() ? analyzeResult(ret) : FAILED_TRANSACTION;
}

status_t EffectHalHidl::command(uint32_t cmdCode, uint32_t cmdSize, void *pCmdData,
        uint32_t *replySize, void *pReplyData) {
    if (mEffect == 0) return NO_INIT;
    hidl_vec<uint8_t> hidlData;
    if (pCmdData != nullptr && cmdSize > 0) {
        hidlData.setToExternal(reinterpret_cast<uint8_t*>(pCmdData), cmdSize);
    }
    status_t status;
    uint32_t replySizeStub = 0;
    if (replySize == nullptr) replySize = &replySizeStub;
    Return<void> ret = mEffect->command(cmdCode, hidlData, *replySize,
            [&](int32_t s, const hidl_vec<uint8_t>& result) {
                status = s;
                if (status == 0) {
                    if (*replySize > result.size()) *replySize = result.size();
                    if (pReplyData != nullptr && *replySize > 0) {
                        memcpy(pReplyData, &result[0], *replySize);
                    }
                }
            });
    return ret.isOk() ? status : FAILED_TRANSACTION;
}

status_t EffectHalHidl::getDescriptor(effect_descriptor_t *pDescriptor) {
    if (mEffect == 0) return NO_INIT;
    Result retval = Result::NOT_INITIALIZED;
    Return<void> ret = mEffect->getDescriptor(
            [&](Result r, const EffectDescriptor& result) {
                retval = r;
                if (retval == Result::OK) {
                    effectDescriptorToHal(result, pDescriptor);
                }
            });
    return ret.isOk() ? analyzeResult(retval) : FAILED_TRANSACTION;
}

status_t EffectHalHidl::close() {
    if (mEffect == 0) return NO_INIT;
    Return<Result> ret = mEffect->close();
    return ret.isOk() ? analyzeResult(ret) : FAILED_TRANSACTION;
}

} // namespace android