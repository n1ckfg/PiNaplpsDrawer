#pragma once

#include "ofMain.h"

// ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~
// Ported from public/js/drawing/controller.js
//
// Turns MediaPipe's per-frame gesture labels into something that behaves like a
// VR controller: buttons that latch, a filtered position, and a confidence
// score that ignores input while the hand is being tracked badly.
// ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~

/// Scalar Kalman filter. Lower Q is smoother and slower to respond; higher R
/// trusts each measurement less.
class KalmanFilter1D {

    public:

        KalmanFilter1D(float _Q = 0.1f, float _R = 0.5f) : Q(_Q), R(_R) {}

        float update(float measurement) {
            if (!initialized) {
                x = measurement;
                initialized = true;
                return x;
            }

            // Prediction
            P = P + Q;

            // Update
            const float K = P / (P + R); // Kalman gain
            x = x + K * (measurement - x);
            P = (1.0f - K) * P;

            return x;
        }

        void reset() {
            x = 0.0f;
            P = 1.0f;
            initialized = false;
        }

    private:

        float Q;                 // process noise
        float R;                 // measurement noise
        float x = 0.0f;          // estimated value
        float P = 1.0f;          // estimation error covariance
        bool initialized = false;

};

class Controller : public ofNode {

    public:

        struct KalmanSettings {
            float Q = 0.1f;
            float R = 0.5f;
            bool enabled = true;
        };

        Controller();
        explicit Controller(const KalmanSettings & settings);

        // ~ ~ ~ buttons ~ ~ ~
        // _Down is the rising edge, _Up the falling one; _Held is the level.
        // Only the trigger reports _Up, matching the JS.

        bool grip_Down = false;
        bool grip_Held = false;

        bool trigger_Down = false;
        bool trigger_Held = false;
        bool trigger_Up = false;

        bool buttonA_Down = false;
        bool buttonA_Held = false;

        bool buttonB_Down = false;
        bool buttonB_Held = false;

        bool buttonC_Down = false;
        bool buttonC_Held = false;

        /// Whether the hand was seen at all this frame. ofNode has no visibility
        /// of its own, so the drawing code checks this before using the pose.
        bool visible = false;

        /// Clears every button and edge. Used by the two-handed Victory reset.
        void clearButtons();

        void updatePose(const glm::vec3 & position);
        /// The overload for a frame where the hand wasn't found: the pose is
        /// left where it was rather than snapping to the origin.
        void updatePoseMissing();

        void updateGrip(bool isClosedFist, bool isOpenPalm);
        void updateTrigger(bool isHeld, bool isOpenPalm, bool isClosedFist);
        void updateButtonA(bool isHeld);
        void updateButtonB(bool isHeld);
        void updateButtonC(bool isHeld);

        /// Lightly filtered: responsive enough to draw with.
        glm::vec3 getDrawPosition() const;
        /// Heavily filtered: steady enough to navigate with, which is why the
        /// two-handed world grab uses this one and never the draw position.
        glm::vec3 getNavPosition() const;

        /// Rolling 0-1 score; low means the hand is jittering and buttons are
        /// being ignored.
        float getConfidence() const { return confidence; }
        bool areButtonsBlocked() const { return buttonsBlocked; }

        /// How long grip survives without another Closed_Fist sighting, in ms.
        /// The classifier drops the label for a frame or two mid-grab, and
        /// without this the world would be released every time it did.
        uint64_t gripTimeout = 1000;

    private:

        void init(const KalmanSettings & settings);
        void updateConfidence(const glm::vec3 & currentPosition);

        bool kalmanEnabled = true;

        // Two filters per axis: the drawing one is tuned at half the smoothing
        // (double Q, half R) and the navigation one at double it.
        KalmanFilter1D drawKalmanX, drawKalmanY, drawKalmanZ;
        KalmanFilter1D navKalmanX, navKalmanY, navKalmanZ;

        glm::vec3 drawPosition { 0.0f };
        glm::vec3 navPosition { 0.0f };

        bool wasGrip_Held = false;
        bool wasTrigger_Held = false;
        bool wasButtonA_Held = false;
        bool wasButtonB_Held = false;
        bool wasButtonC_Held = false;

        uint64_t lastClosedFistTime = 0;

        // ~ ~ ~ confidence ~ ~ ~
        // Acceleration is the tell: a hand the model is tracking cleanly moves
        // smoothly, while a mis-tracked one jumps. Buttons are gated on a
        // smoothed version of that, with hysteresis so the gate doesn't chatter.
        float confidence = 1.0f;
        glm::vec3 prevPosition { 0.0f };
        glm::vec3 prevVelocity { 0.0f };
        bool hasHistory = false;
        bool buttonsBlocked = false;

        static constexpr float kConfidenceSmoothing = 0.1f; // EMA factor
        static constexpr float kBlockThreshold = 0.2f;
        static constexpr float kUnblockThreshold = 0.4f;
        /// Acceleration magnitude that maps to a confidence of ~0.5.
        static constexpr float kAccelMidpoint = 0.15f;

};
