#pragma once

#include "ofMain.h"

#include "Controller.h"

#include <vector>

// ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~
// Ported from public/js/drawing/worldscale.js
//
// The two-handed grab: fists closed, the drawing follows the hands, rotating
// with the line between them and scaling with the distance. This is the
// OpenXR-style world-scale interaction, driven by MediaPipe gestures instead of
// controllers.
//
// Only the both-hands mode is wired up. The single-hand modes exist in the
// original but are mapped to NONE there too, so grabbing with one hand does
// nothing on purpose -- one tracked hand is not steady enough to carry the
// whole drawing.
// ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~

class WorldScale {

    public:

        void setup(Controller & main, Controller & alt, ofNode & target);

        /// Call once per frame.
        void update();

    private:

        enum class SevenMode { Both, Main, Alt, None };

        struct TransformSample {
            uint64_t time = 0;
            glm::vec3 position { 0.0f };
            glm::quat orientation { 1.0f, 0.0f, 0.0f, 0.0f };
            glm::vec3 scale { 1.0f };
        };

        void attachTargetBoth();
        void updateTargetBoth();

        void recordPosition(uint64_t now);
        void startRewindLerp(uint64_t now);
        void updateLerp(uint64_t now);

        Controller * cltMain = nullptr;
        Controller * cltAlt = nullptr;
        ofNode * target = nullptr;
        ofNode * origParent = nullptr;

        bool armed = false;
        SevenMode sevenMode = SevenMode::None;
        bool wasGrip_Held = false;

        glm::vec3 initialHandPosition1 { 0.0f };
        glm::vec3 initialHandPosition2 { 0.0f };
        glm::quat initialObjectRotation { 1.0f, 0.0f, 0.0f, 0.0f };
        glm::vec3 initialObjectScale { 1.0f };
        glm::vec3 initialObjectDirection { 0.0f };

        // ~ ~ ~ rewind ~ ~ ~
        // Letting go of a fist is itself a gesture, and the hands drift while
        // the classifier notices. So the last third of a second of movement is
        // kept, and releasing eases the drawing back to where it was before that
        // drift -- otherwise every release would nudge the whole drawing.
        static constexpr uint64_t kBufferDuration = 300; // ms
        static constexpr uint64_t kLerpDuration = 300;   // ms

        std::vector<TransformSample> positionBuffer;

        bool isLerping = false;
        uint64_t lerpStartTime = 0;
        TransformSample lerpStart;
        TransformSample lerpTarget;

};
