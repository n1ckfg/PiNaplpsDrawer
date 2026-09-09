#pragma once

#include "ofMain.h"

#include "ofxMediaPipe.h"

#include "Controller.h"
#include "MouseController.h"
#include "Palette.h"
#include "Tools.h"
#include "VideoSource.h"
#include "WorldScale.h"

#include <array>
#include <string>

// ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~
// Ported from public/js/drawing/drawing.js
//
// Live drawing in 3D with the hands, replacing the browser's Three.js scene and
// MediaPipe-in-WASM with ofNode and ofxMediaPipe. Exiting encodes whatever was
// drawn to NAPLPS, which is what makes it a drawing tool for this format rather
// than a sketchpad.
//
// The gesture vocabulary, unchanged from the JS:
//
//   Pointing_Up   trigger   draw a stroke
//   Closed_Fist   grip      grab the world; hold 1.6s to open the palette
//   Open_Palm     --        release everything
//   Thumb_Down    button B  hold 2s: one hand undo, two hands clear
//   Thumb_Up      button A  hold 2s: one hand recentre, two hands exit
//   Victory       button C  both hands: instant full reset
//
// Every destructive action is a timed hold with a shrinking circle, because the
// classifier does misfire and an instant undo on a misread gesture would be
// unrecoverable.
// ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~

class DrawingMode {

    public:

        static constexpr int kMaxHands = 2;

        /// The view is 4:3 to match the NAPLPS canvas, letterboxed in the window.
        static constexpr float kDrawAspect = 640.0f / 480.0f;

        static constexpr uint64_t kPaletteHoldDuration = 1600; // ms to reveal
        static constexpr uint64_t kPaletteFlickerDuration = 300;
        static constexpr uint64_t kHoldDuration = 2000; // undo/confirm hold
        static constexpr uint64_t kFlickerDuration = 300;
        static constexpr uint64_t kOrientationFadeDuration = 5000;

        static constexpr float kCircleMaxSize = 400.0f; // px
        static constexpr float kCircleMinScale = 0.1f;
        static constexpr float kCircleSpacing = 0.5f;

        /// Loads the models and opens the camera. Both are slow, so this is
        /// called once at app startup rather than on entering the mode.
        void setup();
        void exit();

        /// Enters/leaves drawing mode. stop() encodes the drawing first, which
        /// is why it isn't just a flag.
        void start();
        void stop();
        bool isActive() const { return active; }

        void update();
        void draw();

        void keyPressed(int key);
        void keyReleased(int key);
        void mouseMoved(int x, int y);
        void mouseDragged(int x, int y, int button);
        void mousePressed(int x, int y, int button);
        void mouseReleased(int x, int y, int button);
        void mouseScrolled(float scrollY);

        /// Swaps the Pi's ribbon camera for a USB webcam and back, which is what
        /// `c` does in this mode. Keeps the current camera if the other one
        /// isn't there, and says so in the HUD either way.
        void toggleCameraSource();

        /// The NAPLPS produced by the last stop(), or "" if nothing was drawn.
        /// ofApp picks this up to display, publish and optionally mint.
        const std::string & getEncodedNaplps() const { return encodedNaplps; }
        void clearEncodedNaplps() { encodedNaplps.clear(); }

        /// Set when a two-handed Thumb_Up asks to leave. ofApp polls and clears
        /// it, so the mode never has to know what the app does next.
        bool isExitRequested() const { return exitRequested; }
        void clearExitRequested() { exitRequested = false; }

        /// The 4:3 box the scene renders into, centred in the window.
        ofRectangle getViewport() const;

        std::string getStatusText() const;

    private:

        // ~ ~ ~ per-frame stages, in the order update() runs them ~ ~ ~
        void updateTracking();
        void updateHand(int index, const ofxMediaPipe::Hand & hand);
        void updateDrawing();
        void updateMouse();
        void updatePalettes();
        void updateUndoAndReset();
        void updateConfirm();
        void updateOrientationFade();
        void updateCameraNavigation();

        void resetCamera();
        void resetWorld();
        void updateCameraFromSpherical();

        void setDrawColor(const ofColor & color);

        /// Projects every stroke to the NAPLPS unit screen and encodes it.
        /// Called by stop().
        void convertToNaplps();

        /// World point -> NDC, using the same projection the scene is drawn
        /// with, so what gets encoded is what the user saw.
        glm::vec3 worldToNdc(const glm::vec3 & world) const;

        void drawScene();
        void drawOverlay();
        void drawHoldCircles(float scale, bool isDouble, const ofColor & color);
        void drawOrientationObjects();

        bool active = false;
        bool exitRequested = false;
        std::string encodedNaplps;

        // ~ ~ ~ vision ~ ~ ~
        VideoSource video;
        ofxMediaPipe::Tracker tracker;
        ofxMediaPipe::Tracker::Results results;
        /// Kept so the HUD can show what the camera sees; the scene itself is
        /// drawn over it rather than beside it.
        ofTexture cameraTexture;
        /// What the last camera switch did, shown for a few seconds. A failed
        /// switch is silent otherwise -- the picture simply doesn't change,
        /// which reads as the key not working.
        std::string videoMessage;
        uint64_t videoMessageStart = 0;
        static constexpr uint64_t kVideoMessageDuration = 4000;

        // ~ ~ ~ scene ~ ~ ~
        ofCamera camera;
        ofNode worldNode;
        Frame frame;
        WorldScale worldScale;

        std::array<Controller, kMaxHands> controllers;
        std::array<Palette, kMaxHands> palettes;
        std::array<ofColor, kMaxHands> controllerDrawColor;

        /// Palette state, one slot per hand.
        std::array<uint64_t, kMaxHands> paletteGripStart {};
        std::array<bool, kMaxHands> paletteVisible {};
        std::array<glm::vec3, kMaxHands> paletteSpawnPos {};
        std::array<uint64_t, kMaxHands> paletteFlickerStart {};
        std::array<int, kMaxHands> paletteFlickerIndex {};

        /// The gesture label shown next to each hand.
        std::array<std::string, kMaxHands> handLabel;
        std::array<std::string, kMaxHands> handSubLabel;

        /// This frame's raw gesture name per hand, and how far the fingertip sat
        /// from the camera. The pointer sphere is drawn from these rather than
        /// from the latched buttons, so what it reports is the classifier's
        /// reading right now -- which is what the JS shows, and what tells you
        /// whether a gesture is landing before you commit to a hold.
        std::array<std::string, kMaxHands> handGesture;
        std::array<float, kMaxHands> handDepthScale { };

        MouseController mouseController;
        Palette mousePalette;
        bool mousePaletteVisible = false;
        ofColor mouseDrawColor = ofColor(255);

        // ~ ~ ~ camera orbit ~ ~ ~
        float cameraRadius = 5.0f;
        float cameraTheta = HALF_PI;
        float cameraPhi = HALF_PI;
        glm::vec3 cameraTarget { 0.0f };

        static constexpr float kMouseSensitivity = 0.003f;
        static constexpr float kPanSensitivity = 0.01f;
        static constexpr float kZoomSensitivity = 0.001f;
        static constexpr float kMoveSpeed = 0.1f;

        bool keyW = false, keyA = false, keyS = false, keyD = false;
        /// Held rather than tapped, `c` would arrive as a key repeat every few
        /// frames and reopen the camera each time; this makes it fire once per
        /// press.
        bool keyC = false;
        bool altDown = false, shiftDown = false;
        bool isMouseOrbiting = false;
        glm::vec2 lastMouse { 0.0f, 0.0f };

        // ~ ~ ~ timed holds ~ ~ ~
        enum class PendingAction { None, Undo, Reset };
        PendingAction pendingAction = PendingAction::None;
        uint64_t undoHoldStart = 0;
        uint64_t undoFlickerStart = 0;
        bool undoHolding = false;
        bool undoFlickering = false;

        enum class ConfirmAction { None, Single, Double };
        ConfirmAction pendingConfirm = ConfirmAction::None;
        uint64_t confirmHoldStart = 0;
        uint64_t confirmFlickerStart = 0;
        bool confirmHolding = false;
        bool confirmFlickering = false;

        uint64_t orientationFadeStart = 0;
        bool orientationFading = false;

        ofTrueTypeFont font;

};
