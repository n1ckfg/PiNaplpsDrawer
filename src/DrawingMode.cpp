#include "DrawingMode.h"

#include "NodeUtils.h"

#include "ofxNaplps.h"

#include <algorithm>
#include <cctype>
#include <cmath>

//--------------------------------------------------------------
void DrawingMode::setup() {
    font.load(OF_TTF_SANS, 11, true, true);

    // Fixed 4:3 regardless of the window, so what's encoded matches what the
    // NAPLPS canvas will show.
    camera.setFov(75.0f);
    camera.setNearClip(0.1f);
    camera.setFarClip(1000.0f);
    camera.setForceAspectRatio(true);
    camera.setAspectRatio(kDrawAspect);

    frame.setup(worldNode);
    worldScale.setup(controllers[0], controllers[1], worldNode);

    for (int i = 0; i < kMaxHands; i++) {
        palettes[i].setup(0.6f, 0.08f);
        controllerDrawColor[i] = ofColor(255);
        paletteGripStart[i] = 0;
        paletteVisible[i] = false;
        paletteFlickerStart[i] = 0;
        paletteFlickerIndex[i] = -1;
    }

    mousePalette.setup(0.6f, 0.08f);
    mouseController.setup();
    mouseController.setDrawPlaneDistance(5.0f);

    resetCamera();

    // The camera and the models both take seconds to come up, so they're
    // started once here rather than each time drawing mode is entered.
    VideoSource::Settings videoSettings;
    videoSettings.width = 640;
    videoSettings.height = 480;
    videoSettings.frameRate = 30;
    video.setup(videoSettings);

    ofxMediaPipe::Tracker::Settings trackerSettings;
    // Only the hands matter here. Pose landmarking would roughly double the
    // cost of a pass for nothing this mode uses.
    trackerSettings.enablePose = false;
    trackerSettings.enableGesture = true;
    trackerSettings.gesture.modelPath = "gesture_recognizer.task";
    trackerSettings.gesture.numHands = kMaxHands;
    // Below this a gesture is reported as "None" rather than guessed at, which
    // matters when a misread fires an undo.
    trackerSettings.gesture.minGestureScore = 0.3f;
    trackerSettings.inferenceWidth = 256;
    tracker.setup(trackerSettings);

    orientationFadeStart = ofGetElapsedTimeMillis();
    orientationFading = true;
}

//--------------------------------------------------------------
void DrawingMode::exit() {
    tracker.stop();
    video.close();
}

//--------------------------------------------------------------
void DrawingMode::start() {
    if (active) return;
    active = true;
    exitRequested = false;
    encodedNaplps.clear();

    mouseController.enable();

    // The guide cube and pyramids reappear on entry and fade out again, so
    // there's always something to orient against in an empty scene.
    orientationFadeStart = ofGetElapsedTimeMillis();
    orientationFading = true;
}

//--------------------------------------------------------------
void DrawingMode::stop() {
    if (!active) return;
    active = false;

    // Encode before anything is torn down: this is the whole point of the mode.
    convertToNaplps();

    mouseController.disable();
    mousePaletteVisible = false;
    mousePalette.visible = false;

    for (int i = 0; i < kMaxHands; i++) {
        palettes[i].visible = false;
        paletteVisible[i] = false;
        controllers[i].clearButtons();
        controllers[i].visible = false;
    }
}

//--------------------------------------------------------------
ofRectangle DrawingMode::getViewport() const {
    // Largest 4:3 box that fits the window, centred (a "contain" fit).
    const float w = std::min((float)ofGetWidth(), (float)ofGetHeight() * kDrawAspect);
    const float h = w / kDrawAspect;
    return ofRectangle((ofGetWidth() - w) * 0.5f, (ofGetHeight() - h) * 0.5f, w, h);
}

// ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~
// update
// ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~

//--------------------------------------------------------------
void DrawingMode::update() {
    if (!active) return;

    updateCameraNavigation();
    updateTracking();
    updateDrawing();
    updateMouse();
    updatePalettes();

    // Both hands flashing Victory is the escape hatch: it clears every latched
    // button and recentres, for when a stuck gesture has left the scene wedged.
    bool bothVictory = true;
    for (int i = 0; i < kMaxHands; i++) {
        if (!controllers[i].buttonC_Held) bothVictory = false;
    }
    if (bothVictory) {
        for (auto & controller : controllers) controller.clearButtons();
        resetCamera();
        resetWorld();

        undoHolding = undoFlickering = false;
        pendingAction = PendingAction::None;

        orientationFadeStart = ofGetElapsedTimeMillis();
        orientationFading = true;
    }

    updateUndoAndReset();
    updateConfirm();

    worldScale.update();
    frame.update();

    updateOrientationFade();
}

//--------------------------------------------------------------
void DrawingMode::updateTracking() {
    video.update();
    if (video.isFrameNew()) {
        // Fed unmirrored: flipping the image would swap MediaPipe's Left/Right
        // handedness labels. The mirroring happens in the coordinate mapping.
        tracker.setPixels(video.getPixels());

        if (!cameraTexture.isAllocated()
            || cameraTexture.getWidth() != video.getPixels().getWidth()
            || cameraTexture.getHeight() != video.getPixels().getHeight()) {
            cameraTexture.allocate(video.getPixels());
        }
        cameraTexture.loadData(video.getPixels());
    }

    tracker.getResults(results);

    for (int i = 0; i < kMaxHands; i++) {
        controllers[i].visible = false;
        handLabel[i].clear();
        handSubLabel[i].clear();
        handGesture[i].clear();
        handDepthScale[i] = 1.0f;
    }

    const int handCount = std::min((int)results.hands.size(), kMaxHands);
    for (int i = 0; i < handCount; i++) {
        updateHand(i, results.hands[i]);
    }

    // Hands that dropped out keep their latched state; only an Open_Palm or a
    // timeout releases a grip, so a hand leaving the frame mid-grab doesn't
    // drop the world.
    for (int i = handCount; i < kMaxHands; i++) {
        controllers[i].updatePoseMissing();
        controllers[i].updateGrip(false, false);
        controllers[i].updateTrigger(false, false, false);
        controllers[i].updateButtonA(false);
        controllers[i].updateButtonB(false);
        controllers[i].updateButtonC(false);
    }
}

//--------------------------------------------------------------
void DrawingMode::updateHand(int index, const ofxMediaPipe::Hand & hand) {
    if (hand.landmarks.size() <= ofxMediaPipe::HandLandmarkIndex::IndexTip) return;

    Controller & controller = controllers[index];
    controller.visible = true;

    const ofxMediaPipe::Landmark & pointer =
        hand.landmarks[ofxMediaPipe::HandLandmarkIndex::IndexTip];

    const std::string & gestureName = hand.gesture.categoryName;
    const bool isClosedFist = (gestureName == "Closed_Fist");
    const bool isOpenPalm   = (gestureName == "Open_Palm");
    const bool isPointingUp = (gestureName == "Pointing_Up");
    const bool isThumbUp    = (gestureName == "Thumb_Up");
    const bool isThumbDown  = (gestureName == "Thumb_Down");
    const bool isVictory    = (gestureName == "Victory");

    // Size of the view plane at the depth the drawing sits at, so a hand
    // sweeping the camera's field of view sweeps the whole canvas.
    //
    // The JS reads camera.position.z here, which only equals the viewing
    // distance while the camera is on the z axis -- orbiting a quarter turn
    // collapses it to zero and the hands stop moving. The orbit radius is what
    // that expression was standing in for, and it survives orbiting.
    const float depth = cameraRadius;
    const float vFov = ofDegToRad(camera.getFov());
    const float heightAtDepth = 2.0f * std::tan(vFov * 0.5f) * depth;
    const float widthAtDepth = heightAtDepth * kDrawAspect;

    // Mirrored in x so the drawing follows the hand the way a mirror does.
    const float ndcX = 1.0f - 2.0f * pointer.position.x;
    const float ndcY = 1.0f - 2.0f * pointer.position.y;

    const glm::vec3 newPos(
        (ndcX * widthAtDepth) * 0.5f,
        (ndcY * heightAtDepth) * 0.5f,
        -pointer.position.z * 5.0f);

    controller.updatePose(newPos);
    controller.updateGrip(isClosedFist, isOpenPalm);
    controller.updateTrigger(isPointingUp, isOpenPalm, isClosedFist);
    controller.updateButtonA(isThumbUp);
    controller.updateButtonB(isThumbDown);
    controller.updateButtonC(isVictory);

    // Handedness is reported from the person's point of view; the label is
    // flipped again to match the mirrored image they're looking at.
    std::string handedness = hand.handedness.categoryName;
    if (handedness == "Left") {
        handedness = "Right";
    } else if (handedness == "Right") {
        handedness = "Left";
    }

    handGesture[index] = gestureName;

    // The pointer shrinks as the hand goes back, which is the only depth cue in
    // a scene with no shadows.
    handDepthScale[index] = std::max(0.1f, 1.0f - pointer.position.z * 2.0f);

    handLabel[index] = handedness + ": " + (gestureName.empty() ? "None" : gestureName);

    // The JS second line is the metric, wrist-centred landmark. The confidence
    // gate is appended only while it's actually swallowing button presses,
    // since that is otherwise invisible and looks like the app ignoring you.
    const ofxMediaPipe::Landmark & worldTip =
        (hand.worldLandmarks.size() > ofxMediaPipe::HandLandmarkIndex::IndexTip)
            ? hand.worldLandmarks[ofxMediaPipe::HandLandmarkIndex::IndexTip]
            : pointer;

    handSubLabel[index] = "3D World: " + ofToString(worldTip.position.x, 2)
        + ", " + ofToString(worldTip.position.y, 2)
        + ", " + ofToString(worldTip.position.z, 2)
        + (controller.areButtonsBlocked() ? "  (blocked)" : "");
}

//--------------------------------------------------------------
void DrawingMode::updateDrawing() {
    for (int i = 0; i < kMaxHands; i++) {
        Controller & controller = controllers[i];
        const ControllerId id = ofToString(i);

        // While the palette is open the hand is picking a colour, not drawing.
        if (paletteVisible[i]) {
            if (frame.hasActiveStroke(id)) frame.endStroke(id);
            continue;
        }

        if (controller.trigger_Down) {
            frame.beginStroke(controller.getDrawPosition(), id, controllerDrawColor[i]);
        } else if (controller.trigger_Held && frame.hasActiveStroke(id)) {
            frame.continueStroke(controller.getDrawPosition(), id);
        } else if (controller.trigger_Up) {
            frame.endStroke(id);
        }
    }
}

//--------------------------------------------------------------
void DrawingMode::updateMouse() {
    if (!mouseController.isEnabled()) return;

    mouseController.update(camera, getViewport());

    const ControllerId id = "mouse";

    // Right click toggles the palette.
    if (mouseController.checkRightClick()) {
        if (mousePaletteVisible) {
            mousePalette.visible = false;
            mousePaletteVisible = false;
            mouseController.paletteActive = false;
        } else {
            mousePalette.setPosition(mouseController.getPosition());
            NapDraw::faceToward(mousePalette, camera.getGlobalPosition());
            mousePalette.visible = true;
            mousePaletteVisible = true;
            mouseController.paletteActive = true;
            mouseController.paletteJustOpened = true;
        }
    }

    // Pick a colour, unless this is still the click that opened the palette.
    if (mousePaletteVisible && mouseController.isLeftDown && !mouseController.paletteJustOpened) {
        if (mousePalette.hitTest(mouseController.getPosition(), 0.1f)) {
            setDrawColor(mousePalette.getSelectedColor());

            mousePalette.visible = false;
            mousePaletteVisible = false;
            mouseController.paletteActive = false;
        }
    }

    if (!mouseController.isLeftDown) mouseController.paletteJustOpened = false;

    if (!mousePaletteVisible) {
        if (mouseController.trigger_Down) {
            frame.beginStroke(mouseController.getDrawPosition(), id, mouseDrawColor);
        } else if (mouseController.trigger_Held && frame.hasActiveStroke(id)) {
            frame.continueStroke(mouseController.getDrawPosition(), id);
        } else if (mouseController.trigger_Up) {
            frame.endStroke(id);
        }
    }
}

//--------------------------------------------------------------
void DrawingMode::setDrawColor(const ofColor & color) {
    // One colour across every pointer: the palette is the drawing's colour, not
    // the hand's, which is how the JS behaves.
    for (int i = 0; i < kMaxHands; i++) controllerDrawColor[i] = color;
    mouseDrawColor = color;
    mouseController.setColor(color);
}

//--------------------------------------------------------------
void DrawingMode::updatePalettes() {
    const uint64_t now = ofGetElapsedTimeMillis();

    for (int i = 0; i < kMaxHands; i++) {
        Controller & controller = controllers[i];
        Palette & palette = palettes[i];

        // ~ ~ ~ confirming a choice ~ ~ ~
        if (paletteFlickerStart[i] != 0) {
            const uint64_t elapsed = now - paletteFlickerStart[i];
            if (elapsed < kPaletteFlickerDuration) {
                // Handled in draw(): the chosen swatch blinks alone.
            } else {
                setDrawColor(palette.getSelectedColor());

                palette.visible = false;
                palette.soloIndex = -1;
                palette.showSelectionRing = true;
                paletteVisible[i] = false;
                paletteFlickerStart[i] = 0;
                paletteFlickerIndex[i] = -1;
                paletteGripStart[i] = 0;
            }
            continue;
        }

        if (controller.grip_Held && controller.visible) {
            if (paletteGripStart[i] == 0) paletteGripStart[i] = now;

            const uint64_t gripElapsed = now - paletteGripStart[i];

            if (gripElapsed >= kPaletteHoldDuration && !paletteVisible[i]) {
                // Two fists means the world grab, not the palette.
                const bool otherGrip = controllers[(i + 1) % kMaxHands].grip_Held;
                if (!otherGrip) {
                    paletteVisible[i] = true;
                    palette.visible = true;

                    // Spawns at the hand, facing the camera.
                    const glm::vec3 pos = controller.getGlobalPosition();
                    paletteSpawnPos[i] = pos;
                    palette.setPosition(pos);
                    NapDraw::faceToward(palette, camera.getGlobalPosition());
                }
            }

            if (paletteVisible[i]) {
                if (palette.hitTest(controller.getGlobalPosition(), 0.05f)) {
                    palette.soloIndex = palette.getSelectedIndex();
                    palette.showSelectionRing = false;
                    paletteFlickerStart[i] = now;
                    paletteFlickerIndex[i] = palette.getSelectedIndex();
                }
            }
        } else {
            if (paletteVisible[i] && paletteFlickerStart[i] == 0) {
                palette.visible = false;
                palette.soloIndex = -1;
                palette.showSelectionRing = true;
                paletteVisible[i] = false;
            }
            paletteGripStart[i] = 0;
        }
    }
}

//--------------------------------------------------------------
// Thumb_Down: one hand undoes the last stroke, two hands clear the drawing.
// The circles shrink as the hold progresses, so it's obvious how long is left
// and letting go early cancels.
void DrawingMode::updateUndoAndReset() {
    const uint64_t now = ofGetElapsedTimeMillis();

    int buttonBCount = 0;
    for (const auto & controller : controllers) {
        if (controller.buttonB_Held) buttonBCount++;
    }

    if (undoFlickering) {
        if (now - undoFlickerStart >= kFlickerDuration) {
            undoFlickering = false;
            undoHolding = false;

            const bool wasReset = (pendingAction == PendingAction::Reset);
            pendingAction = PendingAction::None;

            for (auto & controller : controllers) controller.clearButtons();

            if (wasReset) {
                orientationFadeStart = now;
                orientationFading = true;
            }
        }
        return;
    }

    if (buttonBCount > 0) {
        const PendingAction currentAction =
            (buttonBCount == 2) ? PendingAction::Reset : PendingAction::Undo;

        if (!undoHolding) {
            undoHolding = true;
            undoHoldStart = now;
            pendingAction = currentAction;
        } else if (currentAction == PendingAction::Reset) {
            // A second hand joining mid-hold upgrades undo to clear.
            pendingAction = PendingAction::Reset;
        }

        const uint64_t elapsed = now - undoHoldStart;
        if (elapsed >= kHoldDuration) {
            undoFlickering = true;
            undoFlickerStart = now;

            if (pendingAction == PendingAction::Reset) {
                frame.clearWithFlicker();
                resetCamera();
            } else {
                frame.undoWithFlicker();
            }
        }
    } else {
        undoHolding = false;
        pendingAction = PendingAction::None;
    }
}

//--------------------------------------------------------------
// Thumb_Up: one hand recentres the view, two hands leave drawing mode. These
// circles expand rather than shrink, to read as the opposite of the red ones.
void DrawingMode::updateConfirm() {
    const uint64_t now = ofGetElapsedTimeMillis();

    int buttonACount = 0;
    for (const auto & controller : controllers) {
        if (controller.buttonA_Held) buttonACount++;
    }

    if (confirmFlickering) {
        if (now - confirmFlickerStart >= kFlickerDuration) {
            confirmFlickering = false;
            confirmHolding = false;

            const bool wasSingle = (pendingConfirm == ConfirmAction::Single);
            pendingConfirm = ConfirmAction::None;

            for (auto & controller : controllers) controller.clearButtons();

            if (wasSingle) {
                resetCamera();
                resetWorld();
                orientationFadeStart = now;
                orientationFading = true;
            } else {
                // ofApp owns what happens next -- it has to show and publish the
                // encoded drawing, which this class knows nothing about.
                exitRequested = true;
            }
        }
        return;
    }

    if (buttonACount > 0) {
        const ConfirmAction currentAction =
            (buttonACount == 2) ? ConfirmAction::Double : ConfirmAction::Single;

        if (!confirmHolding) {
            confirmHolding = true;
            confirmHoldStart = now;
            pendingConfirm = currentAction;
        } else if (currentAction == ConfirmAction::Double) {
            pendingConfirm = ConfirmAction::Double;
        }

        if (now - confirmHoldStart >= kHoldDuration) {
            confirmFlickering = true;
            confirmFlickerStart = now;
        }
    } else {
        confirmHolding = false;
        pendingConfirm = ConfirmAction::None;
    }
}

//--------------------------------------------------------------
void DrawingMode::updateOrientationFade() {
    if (!orientationFading) return;
    if (ofGetElapsedTimeMillis() - orientationFadeStart >= kOrientationFadeDuration) {
        orientationFading = false;
    }
}

// ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~
// camera
// ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~

//--------------------------------------------------------------
void DrawingMode::updateCameraFromSpherical() {
    const glm::vec3 position(
        cameraTarget.x + cameraRadius * std::sin(cameraPhi) * std::cos(cameraTheta),
        cameraTarget.y + cameraRadius * std::cos(cameraPhi),
        cameraTarget.z + cameraRadius * std::sin(cameraPhi) * std::sin(cameraTheta));

    camera.setPosition(position);
    camera.lookAt(cameraTarget, glm::vec3(0.0f, 1.0f, 0.0f));
}

//--------------------------------------------------------------
void DrawingMode::resetCamera() {
    cameraRadius = 5.0f;
    cameraTheta = HALF_PI;
    cameraPhi = HALF_PI;
    cameraTarget = glm::vec3(0.0f);
    updateCameraFromSpherical();
}

//--------------------------------------------------------------
void DrawingMode::resetWorld() {
    worldNode.setPosition(0.0f, 0.0f, 0.0f);
    worldNode.setOrientation(glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
    worldNode.setScale(1.0f);
}

//--------------------------------------------------------------
void DrawingMode::updateCameraNavigation() {
    if (!keyW && !keyA && !keyS && !keyD) return;

    // Movement is on the ground plane, so looking up doesn't fly the view.
    glm::vec3 forward = camera.getLookAtDir();
    forward.y = 0.0f;
    if (glm::dot(forward, forward) < 1e-9f) return;
    forward = glm::normalize(forward);

    const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));

    glm::vec3 movement(0.0f);
    if (keyW) movement += forward * kMoveSpeed;
    if (keyS) movement -= forward * kMoveSpeed;
    if (keyA) movement -= right * kMoveSpeed;
    if (keyD) movement += right * kMoveSpeed;

    cameraTarget += movement;
    updateCameraFromSpherical();
}

// ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~
// input
// ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~

//--------------------------------------------------------------
void DrawingMode::keyPressed(int key) {
    switch (std::tolower(key)) {
        case 'w': keyW = true; break;
        case 'a': keyA = true; break;
        case 's': keyS = true; break;
        case 'd': keyD = true; break;
        case 'c':
            if (!keyC) toggleCameraSource();
            keyC = true;
            break;
        default: break;
    }

    if (key == OF_KEY_ALT) altDown = true;
    if (key == OF_KEY_SHIFT) shiftDown = true;
}

//--------------------------------------------------------------
void DrawingMode::keyReleased(int key) {
    switch (std::tolower(key)) {
        case 'w': keyW = false; break;
        case 'a': keyA = false; break;
        case 's': keyS = false; break;
        case 'd': keyD = false; break;
        case 'c': keyC = false; break;
        default: break;
    }

    if (key == OF_KEY_ALT) altDown = false;
    if (key == OF_KEY_SHIFT) shiftDown = false;
}

//--------------------------------------------------------------
void DrawingMode::toggleCameraSource() {
    // Only ever between the two real cameras. If the app came up on a movie,
    // a still or the synthetic feed there was no camera to begin with, so this
    // starts at the Pi's -- the one that is there on the hardware this runs on.
    const VideoSource::Backend wanted = (video.getBackend() == VideoSource::Backend::Csi)
        ? VideoSource::Backend::Webcam
        : VideoSource::Backend::Csi;

    // Safe to pull the source out from under the tracker: setPixels() copies,
    // so an inference already running on the worker thread finishes on its own
    // frame rather than on freed pixels.
    if (video.switchTo(wanted)) {
        videoMessage = "camera: " + video.getBackendName() + " - " + video.getDescription();
    } else {
        videoMessage = "no " + VideoSource::toString(wanted) + " (" + video.getLastError()
            + ") - still on " + video.getBackendName();
    }
    videoMessageStart = ofGetElapsedTimeMillis();

    // The two cameras need not agree on frame size, and a stale texture would
    // be drawn at the wrong one until the first new frame lands.
    cameraTexture.clear();
}

//--------------------------------------------------------------
void DrawingMode::mouseMoved(int x, int y) {
    mouseController.mouseMoved(x, y);
    lastMouse = glm::vec2((float)x, (float)y);
}

//--------------------------------------------------------------
void DrawingMode::mousePressed(int x, int y, int button) {
    // Alt claims the drag for the camera, so the mouse controller ignores it.
    if (altDown) {
        isMouseOrbiting = true;
        lastMouse = glm::vec2((float)x, (float)y);
    }
    mouseController.mousePressed(x, y, button, altDown);
}

//--------------------------------------------------------------
void DrawingMode::mouseReleased(int x, int y, int button) {
    isMouseOrbiting = false;
    mouseController.mouseReleased(x, y, button);
}

//--------------------------------------------------------------
void DrawingMode::mouseDragged(int x, int y, int button) {
    if (!isMouseOrbiting || !altDown) {
        isMouseOrbiting = false;
        mouseController.mouseMoved(x, y);
        lastMouse = glm::vec2((float)x, (float)y);
        return;
    }

    const float deltaX = (float)x - lastMouse.x;
    const float deltaY = (float)y - lastMouse.y;
    lastMouse = glm::vec2((float)x, (float)y);

    if (shiftDown) {
        // Alt + Shift: pan the orbit target across the view plane.
        const glm::vec3 forward = glm::normalize(camera.getLookAtDir());
        const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));
        const glm::vec3 up = glm::normalize(glm::cross(right, forward));

        cameraTarget += right * (-deltaX * kPanSensitivity);
        cameraTarget += up * (deltaY * kPanSensitivity);
    } else {
        // Alt: orbit.
        cameraTheta += deltaX * kMouseSensitivity;
        cameraPhi -= deltaY * kMouseSensitivity;
        // Stop short of the poles, where lookAt would flip the view over.
        cameraPhi = std::max(0.1f, std::min(PI - 0.1f, cameraPhi));
    }

    updateCameraFromSpherical();
}

//--------------------------------------------------------------
void DrawingMode::mouseScrolled(float scrollY) {
    // Proportional to the current distance, so zoom feels the same near and far.
    cameraRadius -= scrollY * kZoomSensitivity * cameraRadius * 100.0f;
    cameraRadius = std::max(0.5f, std::min(50.0f, cameraRadius));
    updateCameraFromSpherical();
}

// ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~
// draw
// ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~

//--------------------------------------------------------------
void DrawingMode::draw() {
    if (!active) return;

    const ofRectangle viewport = getViewport();

    ofBackground(34);

    ofPushStyle();
    ofEnableDepthTest();

    camera.begin(viewport);
    drawScene();
    camera.end();

    ofDisableDepthTest();
    ofPopStyle();

    drawOverlay();
}

//--------------------------------------------------------------
void DrawingMode::drawScene() {
    drawOrientationObjects();

    frame.draw();

    // Pointers: a sphere per tracked hand, ringed in the current colour.
    for (int i = 0; i < kMaxHands; i++) {
        if (!controllers[i].visible) continue;

        const glm::vec3 pos = controllers[i].getGlobalPosition();

        // Colour echoes the gesture the classifier is reading *this frame*, not
        // the latched button. A fist reads red and an open palm green, so the
        // grab and the release are both visible before either takes effect.
        ofColor sphereColor(255);
        if (handGesture[i] == "Closed_Fist") {
            sphereColor = ofColor(255, 0, 0);
        } else if (handGesture[i] == "Open_Palm") {
            sphereColor = ofColor(0, 255, 0);
        }

        ofSetColor(sphereColor);
        ofDrawSphere(pos, 0.2f * handDepthScale[i]);

        ofPushStyle();
        ofDisableDepthTest();
        ofNoFill();
        ofSetLineWidth(3.0f);
        ofSetColor(controllerDrawColor[i]);
        ofDrawCircle(pos, 0.25f * handDepthScale[i]);
        ofFill();
        ofEnableDepthTest();
        ofPopStyle();
    }

    mouseController.draw();

    // Palettes, plus the line back to the hand that opened one.
    for (int i = 0; i < kMaxHands; i++) {
        if (!palettes[i].visible) continue;

        // While a choice is confirming, the chosen swatch blinks alone. An index
        // past the end of the list matches nothing, which is the off phase.
        if (paletteFlickerStart[i] != 0) {
            const uint64_t elapsed = ofGetElapsedTimeMillis() - paletteFlickerStart[i];
            const bool blinkOn = ((elapsed / 50) % 2) == 0;
            palettes[i].soloIndex = blinkOn
                ? paletteFlickerIndex[i]
                : (int)palettes[i].getSwatches().size();
        } else if (controllers[i].visible) {
            ofPushStyle();
            ofDisableDepthTest();
            ofSetColor(255);
            ofSetLineWidth(1.0f);
            ofDrawLine(paletteSpawnPos[i], controllers[i].getGlobalPosition());
            ofEnableDepthTest();
            ofPopStyle();
        }

        palettes[i].draw();
    }

    mousePalette.draw();
}

//--------------------------------------------------------------
void DrawingMode::drawOrientationObjects() {
    if (!orientationFading) return;

    const float elapsed = (float)(ofGetElapsedTimeMillis() - orientationFadeStart);
    const float progress = std::min(elapsed / (float)kOrientationFadeDuration, 1.0f);
    const float alpha = (1.0f - progress) * 255.0f;
    if (alpha <= 0.0f) return;

    ofPushMatrix();
    ofMultMatrix(worldNode.getGlobalTransformMatrix());
    ofEnableAlphaBlending();

    // A cube at the origin with pyramids on +x and +y: enough to tell which way
    // the world has been turned once a two-handed grab has rolled it.
    ofSetColor(0, 255, 0, alpha);
    ofDrawBox(0.0f, 0.0f, 0.0f, 0.5f);

    ofSetColor(255, 0, 0, alpha);
    ofPushMatrix();
    ofTranslate(2.0f, 0.0f, 0.0f);
    ofRotateZDeg(-90.0f);
    ofDrawCone(0.0f, 0.0f, 0.0f, 0.3f, 0.5f);
    ofPopMatrix();

    ofSetColor(0, 0, 255, alpha);
    ofDrawCone(0.0f, 2.0f, 0.0f, 0.3f, 0.5f);

    ofDisableAlphaBlending();
    ofPopMatrix();
    ofSetColor(255);
}

//--------------------------------------------------------------
void DrawingMode::drawOverlay() {
    const ofRectangle viewport = getViewport();

    // Gesture labels, pinned above each hand's pointer.
    for (int i = 0; i < kMaxHands; i++) {
        if (!controllers[i].visible || handLabel[i].empty()) continue;

        const glm::vec3 screen = camera.worldToScreen(controllers[i].getGlobalPosition(), viewport);
        if (screen.z > 1.0f) continue; // behind the camera

        const float x = screen.x;
        const float y = screen.y - 40.0f;

        ofPushStyle();

        // The plate has to cover both lines, and the coordinate readout is
        // routinely the wider of the two.
        const ofRectangle box = font.getStringBoundingBox(handLabel[i], 0, 0);
        const ofRectangle subBox = font.getStringBoundingBox(handSubLabel[i], 0, 0);
        const float plateWidth = std::max(box.width, subBox.width);

        ofSetColor(0, 0, 0, 180);
        ofDrawRectRounded(x - plateWidth * 0.5f - 6.0f, y - box.height - 6.0f,
                          plateWidth + 12.0f, box.height + 24.0f, 4.0f);
        ofSetColor(235);
        font.drawString(handLabel[i], x - box.width * 0.5f, y);
        ofSetColor(170);
        font.drawString(handSubLabel[i], x - subBox.width * 0.5f, y + 14.0f);
        ofPopStyle();
    }

    // ~ ~ ~ hold circles ~ ~ ~
    // Red shrinks towards a destructive action; green expands towards a
    // confirming one.
    if (undoFlickering) {
        const uint64_t elapsed = ofGetElapsedTimeMillis() - undoFlickerStart;
        if (((elapsed / 50) % 2) == 0) {
            drawHoldCircles(kCircleMinScale, pendingAction == PendingAction::Reset,
                            ofColor(255, 60, 60));
        }
    } else if (undoHolding) {
        const float progress = std::min(
            (float)(ofGetElapsedTimeMillis() - undoHoldStart) / (float)kHoldDuration, 1.0f);
        const float scale = 1.0f - progress * (1.0f - kCircleMinScale);
        drawHoldCircles(scale, pendingAction == PendingAction::Reset, ofColor(255, 60, 60));
    }

    if (confirmFlickering) {
        const uint64_t elapsed = ofGetElapsedTimeMillis() - confirmFlickerStart;
        if (((elapsed / 50) % 2) == 0) {
            drawHoldCircles(1.0f, pendingConfirm == ConfirmAction::Double, ofColor(60, 255, 60));
        }
    } else if (confirmHolding) {
        const float progress = std::min(
            (float)(ofGetElapsedTimeMillis() - confirmHoldStart) / (float)kHoldDuration, 1.0f);
        const float scale = kCircleMinScale + progress * (1.0f - kCircleMinScale);
        drawHoldCircles(scale, pendingConfirm == ConfirmAction::Double, ofColor(60, 255, 60));
    }

    // The camera feed, small and in the corner. The browser hides its <video>
    // element outright, but with no DOM to fall back on this is the only way to
    // see why a hand isn't being picked up -- bad framing, backlight, or an
    // arm out of shot.
    if (cameraTexture.isAllocated()) {
        const float pipWidth = 160.0f;
        const float pipHeight = pipWidth * cameraTexture.getHeight() / cameraTexture.getWidth();
        const float pipX = ofGetWidth() - pipWidth - 10.0f;
        const float pipY = ofGetHeight() - pipHeight - 10.0f;

        ofPushStyle();
        ofSetColor(255);
        // Mirrored, so what's on screen moves the way the hand does.
        cameraTexture.draw(pipX + pipWidth, pipY, -pipWidth, pipHeight);

        ofNoFill();
        ofSetColor(90);
        ofDrawRectangle(pipX, pipY, pipWidth, pipHeight);
        ofFill();
        ofPopStyle();
    }

    ofDrawBitmapStringHighlight(getStatusText(), 10, 20);
}

//--------------------------------------------------------------
void DrawingMode::drawHoldCircles(float scale, bool isDouble, const ofColor & color) {
    const float radius = kCircleMaxSize * 0.5f * scale;
    const float cx = ofGetWidth() * 0.5f;
    const float cy = ofGetHeight() * 0.5f;

    ofPushStyle();
    ofNoFill();
    ofSetLineWidth(4.0f);
    ofSetColor(color);

    if (isDouble) {
        // Two circles, one per hand, so a two-handed action looks different from
        // a one-handed one at a glance.
        const float offset = (kCircleMaxSize * kCircleSpacing) * 0.5f;
        ofDrawCircle(cx - offset, cy, radius);
        ofDrawCircle(cx + offset, cy, radius);
    } else {
        ofDrawCircle(cx, cy, radius);
    }

    ofFill();
    ofPopStyle();
}

//--------------------------------------------------------------
std::string DrawingMode::getStatusText() const {
    std::string text = "LIVE DRAWING\n";
    text += "source: " + video.getBackendName() + "\n";

    if (!videoMessage.empty()
        && ofGetElapsedTimeMillis() - videoMessageStart < kVideoMessageDuration) {
        text += videoMessage + "\n";
    }

    if (tracker.isFailed()) {
        text += "models: FAILED - " + tracker.getError() + "\n";
    } else if (!tracker.isReady()) {
        text += "models: loading...\n";
    } else {
        text += "hands:  " + ofToString(results.hands.size())
            + ", " + ofToString(results.gestureMs, 0) + " ms"
            + " (" + ofToString(tracker.getInferenceFps(), 1) + "/sec)\n";
    }

    text += "strokes: " + ofToString(frame.getStrokeCount()) + "\n";
    text += "\n";
    text += "point: draw   fist: grab world / hold for palette\n";
    text += "thumb down: hold to undo (both hands: clear)\n";
    text += "thumb up:   hold to recentre (both hands: exit)\n";
    text += "mouse: left draw, right palette, alt+drag orbit\n";
    text += "c: switch between Pi camera and USB webcam";
    return text;
}

// ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~
// NAPLPS encoding
// ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~

//--------------------------------------------------------------
glm::vec3 DrawingMode::worldToNdc(const glm::vec3 & world) const {
    const glm::mat4 mvp = camera.getProjectionMatrix(getViewport()) * camera.getModelViewMatrix();
    const glm::vec4 clip = mvp * glm::vec4(world, 1.0f);

    // A point on the camera plane has no projection; push it far off screen so
    // the clamp below discards it rather than producing a NaN coordinate.
    if (std::abs(clip.w) < 1e-9f) return glm::vec3(-2.0f, -2.0f, -2.0f);

    return glm::vec3(clip) / clip.w;
}

//--------------------------------------------------------------
// Flattens the 3D drawing to the NAPLPS unit screen, from exactly the viewpoint
// the user last had. NAPLPS has no line thickness, so each stroke is encoded as
// its filled brush outline rather than its centreline.
void DrawingMode::convertToNaplps() {
    encodedNaplps.clear();

    const std::vector<Stroke> & strokes = frame.getStrokes();
    if (strokes.empty()) {
        ofLogNotice("DrawingMode") << "no strokes to convert";
        return;
    }

    std::vector<NapInputWrapper> input;

    for (const Stroke & stroke : strokes) {
        if (stroke.points.size() < 2) continue;

        const std::vector<glm::vec3> outline3D = stroke.toBrushOutline();
        if (outline3D.size() < 3) continue;

        std::vector<glm::vec2> points2D;
        points2D.reserve(outline3D.size());

        for (const glm::vec3 & localPoint : outline3D) {
            // Stroke points live in the frame's space; the world node's
            // transform is what a two-handed grab has been changing.
            const glm::vec3 world = NapDraw::localToWorld(frame, localPoint);
            const glm::vec3 ndc = worldToNdc(world);

            const float nx = (ndc.x + 1.0f) * 0.5f;

            // The main canvas draws NAPLPS into a square 640x640 space, so this
            // 4:3 view has to be squeezed vertically by 480/640 and pushed down
            // by the remainder -- the same convention the SVG importer uses
            // (y/sH*0.75 + 0.25). Without it the drawing comes out stretched.
            const float vScale = 1.0f / kDrawAspect; // 0.75
            const float ny = ((1.0f - ndc.y) * 0.5f) * vScale + (1.0f - vScale);

            points2D.push_back(glm::vec2(
                ofClamp(nx, 0.0f, 1.0f),
                ofClamp(ny, 0.0f, 1.0f)));
        }

        // One point per frame of drawing is far more than the format needs, and
        // a token is capped at ~30 KB.
        points2D = NapDraw::rdpSimplify(points2D, 0.002f);
        if (points2D.size() < 3) continue;

        input.push_back(NapInputWrapper(stroke.color, points2D, true /* filled */));
    }

    if (input.empty()) {
        ofLogNotice("DrawingMode") << "no valid strokes to encode";
        return;
    }

    NapEncoder encoder;
    encodedNaplps = encoder.encode(input, 4);

    ofLogNotice("DrawingMode") << "encoded " << input.size() << " strokes to "
                               << encodedNaplps.size() << " bytes of NAPLPS";
}
