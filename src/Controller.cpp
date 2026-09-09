#include "Controller.h"

#include "NodeUtils.h"

#include <cmath>

//--------------------------------------------------------------
Controller::Controller() {
    init(KalmanSettings());
}

//--------------------------------------------------------------
Controller::Controller(const KalmanSettings & settings) {
    init(settings);
}

//--------------------------------------------------------------
void Controller::init(const KalmanSettings & settings) {
    kalmanEnabled = settings.enabled;

    // Drawing: half the smoothing, so the line follows the fingertip.
    const float drawQ = settings.Q * 2.0f;
    const float drawR = settings.R * 0.5f;
    drawKalmanX = KalmanFilter1D(drawQ, drawR);
    drawKalmanY = KalmanFilter1D(drawQ, drawR);
    drawKalmanZ = KalmanFilter1D(drawQ, drawR);

    // Navigation: double the smoothing. Grabbing the world with a jittery
    // position would shake the whole drawing, so this one lags on purpose.
    const float navQ = settings.Q * 0.5f;
    const float navR = settings.R * 2.0f;
    navKalmanX = KalmanFilter1D(navQ, navR);
    navKalmanY = KalmanFilter1D(navQ, navR);
    navKalmanZ = KalmanFilter1D(navQ, navR);
}

//--------------------------------------------------------------
void Controller::clearButtons() {
    grip_Down = grip_Held = false;
    trigger_Down = trigger_Held = trigger_Up = false;
    buttonA_Down = buttonA_Held = false;
    buttonB_Down = buttonB_Held = false;
    buttonC_Down = buttonC_Held = false;

    wasGrip_Held = false;
    wasTrigger_Held = false;
    wasButtonA_Held = false;
    wasButtonB_Held = false;
    wasButtonC_Held = false;
}

//--------------------------------------------------------------
void Controller::updatePose(const glm::vec3 & position) {
    if (kalmanEnabled) {
        drawPosition = glm::vec3(
            drawKalmanX.update(position.x),
            drawKalmanY.update(position.y),
            drawKalmanZ.update(position.z));

        navPosition = glm::vec3(
            navKalmanX.update(position.x),
            navKalmanY.update(position.y),
            navKalmanZ.update(position.z));

        // The node itself sits at the navigation position: it's what the
        // on-screen pointer and the palette hit test both follow.
        setPosition(navPosition);
    } else {
        setPosition(position);
        drawPosition = position;
        navPosition = position;
    }

    updateConfidence(getPosition());
}

//--------------------------------------------------------------
void Controller::updatePoseMissing() {
    // Deliberately empty. A hand that leaves the frame for a moment keeps its
    // last pose, so a stroke isn't yanked to the origin by one dropped frame.
}

//--------------------------------------------------------------
void Controller::updateConfidence(const glm::vec3 & currentPosition) {
    if (!hasHistory) {
        prevPosition = currentPosition;
        prevVelocity = glm::vec3(0.0f);
        hasHistory = true;
        return;
    }

    const glm::vec3 velocity = currentPosition - prevPosition;
    const glm::vec3 acceleration = velocity - prevVelocity;
    const float accelMag = glm::length(acceleration);

    // 0 acceleration -> 1.0, kAccelMidpoint -> ~0.5, and falling away from there.
    const float instantConfidence = std::exp(-accelMag / kAccelMidpoint * 0.693f); // ln(2)

    confidence = confidence * (1.0f - kConfidenceSmoothing) + instantConfidence * kConfidenceSmoothing;
    confidence = std::max(0.0f, std::min(1.0f, confidence));

    // Separate thresholds so a score hovering at the boundary doesn't flip the
    // gate on and off every frame.
    if (buttonsBlocked) {
        if (confidence > kUnblockThreshold) buttonsBlocked = false;
    } else {
        if (confidence < kBlockThreshold) buttonsBlocked = true;
    }

    prevPosition = currentPosition;
    prevVelocity = velocity;
}

//--------------------------------------------------------------
glm::vec3 Controller::getDrawPosition() const {
    if (getParent() == nullptr) return drawPosition;
    return NapDraw::localToWorld(*getParent(), drawPosition);
}

//--------------------------------------------------------------
glm::vec3 Controller::getNavPosition() const {
    if (getParent() == nullptr) return navPosition;
    return NapDraw::localToWorld(*getParent(), navPosition);
}

//--------------------------------------------------------------
// Grip latches: a fist closes it, an open palm opens it. Nothing else releases
// it except the timeout, so the hand can be turned or partly hidden mid-grab
// without dropping the world.
void Controller::updateGrip(bool isClosedFist, bool isOpenPalm) {
    const uint64_t now = ofGetElapsedTimeMillis();

    if (isClosedFist) lastClosedFistTime = now;

    if (buttonsBlocked) {
        // Tracking is poor: don't act on gestures, but still honour the timeout
        // so a grip can't get stuck on.
        if (grip_Held && (now - lastClosedFistTime > gripTimeout)) {
            grip_Held = false;
        }
    } else if (isClosedFist) {
        grip_Held = true;
    } else if (isOpenPalm) {
        // An open palm is the universal release.
        grip_Held = false;
        trigger_Held = false;
        buttonA_Held = false;
        buttonB_Held = false;
    } else if (grip_Held && (now - lastClosedFistTime > gripTimeout)) {
        grip_Held = false;
    }
    // Otherwise grip_Held is left as it was.

    grip_Down = grip_Held && !wasGrip_Held;
    wasGrip_Held = grip_Held;
}

//--------------------------------------------------------------
// The trigger latches the same way: Pointing_Up starts a stroke, and either an
// open palm or a fist ends it.
void Controller::updateTrigger(bool isHeld, bool isOpenPalm, bool isClosedFist) {
    if (!buttonsBlocked && isHeld) {
        trigger_Held = true;
    } else if (isOpenPalm || isClosedFist) {
        trigger_Held = false;
    }

    trigger_Down = trigger_Held && !wasTrigger_Held;
    trigger_Up = !trigger_Held && wasTrigger_Held;
    wasTrigger_Held = trigger_Held;
}

//--------------------------------------------------------------
void Controller::updateButtonA(bool isHeld) {
    buttonA_Held = buttonsBlocked ? false : isHeld;
    buttonA_Down = buttonA_Held && !wasButtonA_Held;
    wasButtonA_Held = buttonA_Held;
}

//--------------------------------------------------------------
void Controller::updateButtonB(bool isHeld) {
    buttonB_Held = buttonsBlocked ? false : isHeld;
    buttonB_Down = buttonB_Held && !wasButtonB_Held;
    wasButtonB_Held = buttonB_Held;
}

//--------------------------------------------------------------
void Controller::updateButtonC(bool isHeld) {
    buttonC_Held = buttonsBlocked ? false : isHeld;
    buttonC_Down = buttonC_Held && !wasButtonC_Held;
    wasButtonC_Held = buttonC_Held;
}
