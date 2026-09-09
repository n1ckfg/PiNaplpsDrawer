#include "WorldScale.h"

#include "NodeUtils.h"

#include <algorithm>
#include <cmath>

//--------------------------------------------------------------
void WorldScale::setup(Controller & main, Controller & alt, ofNode & _target) {
    cltMain = &main;
    cltAlt = &alt;
    target = &_target;
    origParent = _target.getParent();
}

//--------------------------------------------------------------
void WorldScale::update() {
    if (cltMain == nullptr || cltAlt == nullptr || target == nullptr) return;

    const bool isGrip_Held = cltMain->grip_Held || cltAlt->grip_Held;
    const uint64_t now = ofGetElapsedTimeMillis();

    if (isGrip_Held && !wasGrip_Held) {
        positionBuffer.clear();
        isLerping = false;
    }

    if (!isGrip_Held && wasGrip_Held) {
        startRewindLerp(now);
    }

    wasGrip_Held = isGrip_Held;

    if (isLerping) {
        updateLerp(now);
        return;
    }

    if (cltMain->grip_Down || cltAlt->grip_Down) {
        if (origParent != nullptr) target->setParent(*origParent, true);
        armed = true;
    }

    if (cltMain->grip_Held && cltAlt->grip_Held) {
        sevenMode = SevenMode::Both;
    } else if (cltMain->grip_Held || cltAlt->grip_Held) {
        // One hand alone is deliberately inert; see the header.
        sevenMode = SevenMode::None;
    } else {
        sevenMode = SevenMode::None;
        if (origParent != nullptr) target->setParent(*origParent, true);
        armed = false;
        return;
    }

    // "Armed" fires once per grab: it snapshots the starting relationship
    // between the hands and the drawing, which every later frame measures
    // against.
    if (armed) {
        if (sevenMode == SevenMode::Both) attachTargetBoth();
        armed = false;
    }

    if (sevenMode == SevenMode::Both) updateTargetBoth();

    if (isGrip_Held) recordPosition(now);
}

//--------------------------------------------------------------
void WorldScale::attachTargetBoth() {
    initialHandPosition1 = cltMain->getNavPosition();
    initialHandPosition2 = cltAlt->getNavPosition();

    initialObjectRotation = target->getGlobalOrientation();
    initialObjectScale = target->getGlobalScale();

    const glm::vec3 targetPos = target->getGlobalPosition();
    const glm::vec3 midpoint = (initialHandPosition1 + initialHandPosition2) * 0.5f;

    // Where the drawing sits relative to the hands. Held through the grab, it's
    // what lets the drawing swing around the hands rather than snapping to them.
    initialObjectDirection = targetPos - midpoint;
}

//--------------------------------------------------------------
void WorldScale::updateTargetBoth() {
    const glm::vec3 currentHandPosition1 = cltMain->getNavPosition();
    const glm::vec3 currentHandPosition2 = cltAlt->getNavPosition();

    const glm::vec3 initialSpan = initialHandPosition1 - initialHandPosition2;
    const glm::vec3 currentSpan = currentHandPosition1 - currentHandPosition2;

    const float initialGrabDistance = glm::length(initialSpan);
    const float currentGrabDistance = glm::length(currentSpan);

    // Hands on top of each other give no direction and no meaningful ratio;
    // hold still rather than dividing by ~0 and flinging the drawing away.
    if (initialGrabDistance < 1e-6f || currentGrabDistance < 1e-6f) return;

    const glm::vec3 handDir1 = initialSpan / initialGrabDistance;
    const glm::vec3 handDir2 = currentSpan / currentGrabDistance;

    // The rotation that carries the old hand axis onto the new one.
    const glm::quat handRot = glm::rotation(handDir1, handDir2);

    const float p = currentGrabDistance / initialGrabDistance;

    const glm::vec3 newScale = initialObjectScale * p;
    const glm::quat newRotation = handRot * initialObjectRotation;

    const glm::vec3 midpoint = (currentHandPosition1 + currentHandPosition2) * 0.5f;
    const glm::vec3 offset = handRot * (initialObjectDirection * p);
    const glm::vec3 newPosition = midpoint + offset;

    NapDraw::setWorldTransform(*target, newPosition, newRotation, newScale);
}

//--------------------------------------------------------------
void WorldScale::recordPosition(uint64_t now) {
    TransformSample sample;
    sample.time = now;
    sample.position = target->getGlobalPosition();
    sample.orientation = target->getGlobalOrientation();
    sample.scale = target->getGlobalScale();

    positionBuffer.push_back(sample);

    // Drop anything older than the rewind window.
    const uint64_t cutoff = (now > kBufferDuration) ? (now - kBufferDuration) : 0;
    auto firstKept = std::find_if(positionBuffer.begin(), positionBuffer.end(),
        [cutoff](const TransformSample & s) { return s.time >= cutoff; });
    positionBuffer.erase(positionBuffer.begin(), firstKept);
}

//--------------------------------------------------------------
void WorldScale::startRewindLerp(uint64_t now) {
    if (positionBuffer.empty()) return;

    // The oldest sample still in the window: where the drawing was before the
    // release gesture started to disturb it.
    lerpTarget = positionBuffer.front();

    lerpStart.position = target->getGlobalPosition();
    lerpStart.orientation = target->getGlobalOrientation();
    lerpStart.scale = target->getGlobalScale();

    lerpStartTime = now;
    isLerping = true;

    if (origParent != nullptr) target->setParent(*origParent, true);
}

//--------------------------------------------------------------
void WorldScale::updateLerp(uint64_t now) {
    const uint64_t elapsed = now - lerpStartTime;
    float t = std::min((float)elapsed / (float)kLerpDuration, 1.0f);

    // Ease out cubic.
    t = 1.0f - std::pow(1.0f - t, 3.0f);

    const glm::vec3 newPos = glm::mix(lerpStart.position, lerpTarget.position, t);
    const glm::quat newRot = glm::slerp(lerpStart.orientation, lerpTarget.orientation, t);
    const glm::vec3 newScale = glm::mix(lerpStart.scale, lerpTarget.scale, t);

    NapDraw::setWorldTransform(*target, newPos, newRot, newScale);

    if (t >= 1.0f) {
        isLerping = false;
        positionBuffer.clear();
    }
}
