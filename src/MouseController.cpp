#include "MouseController.h"

#include "NodeUtils.h"

//--------------------------------------------------------------
void MouseController::setup() {
    lastMoveTime = ofGetElapsedTimeMillis();
}

//--------------------------------------------------------------
void MouseController::enable() {
    enabled = true;
    hide(); // starts hidden until the mouse actually moves
}

//--------------------------------------------------------------
void MouseController::disable() {
    enabled = false;
    isLeftDown = false;
    isRightDown = false;
    trigger_Down = trigger_Held = trigger_Up = false;
    paletteActive = false;
    paletteJustOpened = false;
    hide();
}

//--------------------------------------------------------------
void MouseController::mouseMoved(int x, int y) {
    if (!enabled) return;

    mouseScreen = glm::vec2((float)x, (float)y);
    show();
    lastMoveTime = ofGetElapsedTimeMillis();
}

//--------------------------------------------------------------
void MouseController::mousePressed(int x, int y, int button, bool altPressed) {
    if (!enabled) return;

    // Alt-drag orbits the camera; it must not also start a stroke.
    if (altPressed) return;

    mouseScreen = glm::vec2((float)x, (float)y);

    if (button == OF_MOUSE_BUTTON_LEFT) {
        isLeftDown = true;
    } else if (button == OF_MOUSE_BUTTON_RIGHT) {
        isRightDown = true;
    }
}

//--------------------------------------------------------------
void MouseController::mouseReleased(int x, int y, int button) {
    if (button == OF_MOUSE_BUTTON_LEFT) {
        isLeftDown = false;
    } else if (button == OF_MOUSE_BUTTON_RIGHT) {
        isRightDown = false;
    }
}

//--------------------------------------------------------------
bool MouseController::checkRightClick() {
    if (isRightDown) {
        isRightDown = false; // consume it, so one press is one toggle
        return true;
    }
    return false;
}

//--------------------------------------------------------------
void MouseController::update(const ofCamera & camera, const ofRectangle & viewport) {
    cameraPosition = camera.getGlobalPosition();

    // The drawing plane sits a fixed distance ahead of the camera, square to it.
    const glm::vec3 forward = glm::normalize(camera.getLookAtDir());
    const glm::vec3 planeCenter = cameraPosition + forward * drawDistance;

    // Ray from the camera through the pointer. screenToWorld at depth 0 lands on
    // the near plane, which is enough to give the direction.
    const glm::vec3 nearPoint = camera.screenToWorld(
        glm::vec3(mouseScreen.x, mouseScreen.y, 0.0f), viewport);
    const glm::vec3 rayDir = glm::normalize(nearPoint - cameraPosition);

    // Intersect it with the plane. The denominator only vanishes for a ray
    // parallel to the plane, which can't happen while the plane faces the
    // camera, but guard it rather than emit a NaN position.
    const float denom = glm::dot(rayDir, forward);
    if (std::abs(denom) > 1e-6f) {
        const float t = glm::dot(planeCenter - cameraPosition, forward) / denom;
        if (t > 0.0f) setPosition(cameraPosition + rayDir * t);
    }

    // Same 50% smoothing the hand controllers use for drawing, so a mouse line
    // and a hand line have the same character.
    if (!initialized) {
        smoothPosition = getPosition();
        drawPosition = getPosition();
        initialized = true;
    } else {
        smoothPosition = glm::mix(smoothPosition, getPosition(), 0.5f);
        drawPosition = smoothPosition;
    }

    // Auto-hide after a spell of stillness.
    if (!cursorHidden && ofGetElapsedTimeMillis() - lastMoveTime > kHideDelay) {
        hide();
    }

    const bool wasHeld = trigger_Held;

    if (!paletteActive && isLeftDown) {
        trigger_Down = !wasHeld;
        trigger_Held = true;
        trigger_Up = false;
    } else {
        trigger_Down = false;
        trigger_Up = wasHeld;
        trigger_Held = false;
    }
}

//--------------------------------------------------------------
void MouseController::show() {
    cursorHidden = false;
}

//--------------------------------------------------------------
void MouseController::hide() {
    cursorHidden = true;
}

//--------------------------------------------------------------
void MouseController::draw() {
    if (!enabled || cursorHidden) return;

    const glm::vec3 pos = getGlobalPosition();

    ofPushStyle();

    ofSetColor(255, 255, 0);
    ofDrawSphere(pos, 0.075f);

    // A ring in the current colour, turned to face the camera so it reads as a
    // ring from wherever the viewer is.
    ofPushMatrix();
    ofTranslate(pos);

    ofNode facing;
    facing.setPosition(pos);
    NapDraw::faceToward(facing, cameraPosition);
    ofMultMatrix(glm::mat4_cast(facing.getGlobalOrientation()));

    ofDisableDepthTest();
    ofNoFill();
    ofSetLineWidth(2.0f);
    ofSetColor(drawColor);
    ofDrawCircle(0.0f, 0.0f, 0.0f, 0.1f);
    ofFill();
    ofEnableDepthTest();

    ofPopMatrix();
    ofPopStyle();
    ofSetColor(255);
}
