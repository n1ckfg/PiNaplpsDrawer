#pragma once

#include "ofMain.h"

// ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~
// Ported from public/js/drawing/mouse.js
//
// Draw with a mouse when there's no camera, or no hands in front of it. The
// pointer is projected onto a plane held at a fixed distance in front of the
// camera, so it behaves like a hand at arm's length.
//
// Left button draws, right button toggles the palette, and the cursor fades out
// after a few idle seconds so it doesn't sit on top of the artwork.
// ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~

class MouseController : public ofNode {

    public:

        static constexpr uint64_t kHideDelay = 5000; // ms of stillness before hiding

        void setup();

        /// Call once per frame, before reading the trigger state.
        void update(const ofCamera & camera, const ofRectangle & viewport);

        void enable();
        void disable();
        bool isEnabled() const { return enabled; }

        void draw();

        // ~ ~ ~ events, forwarded from ofApp ~ ~ ~
        void mouseMoved(int x, int y);
        /// `altPressed` is passed in because Alt-drag belongs to the camera, not
        /// to drawing.
        void mousePressed(int x, int y, int button, bool altPressed);
        void mouseReleased(int x, int y, int button);

        /// Consumes a pending right-click. Returns true once per click.
        bool checkRightClick();

        // Mirrors Controller's trigger, so the drawing code treats both alike.
        bool trigger_Down = false;
        bool trigger_Held = false;
        bool trigger_Up = false;

        bool isLeftDown = false;

        /// Set while the palette is open, which suppresses drawing.
        bool paletteActive = false;
        /// Guards against the click that opened the palette also picking a
        /// colour from it on the same frame.
        bool paletteJustOpened = false;

        glm::vec3 getDrawPosition() const { return drawPosition; }

        void setDrawPlaneDistance(float distance) { drawDistance = distance; }
        void setColor(const ofColor & color) { drawColor = color; }
        ofColor getColor() const { return drawColor; }

    private:

        void show();
        void hide();

        bool enabled = false;

        glm::vec2 mouseScreen { 0.0f, 0.0f };
        bool isRightDown = false;

        ofColor drawColor = ofColor(255);

        glm::vec3 smoothPosition { 0.0f };
        glm::vec3 drawPosition { 0.0f };
        bool initialized = false;

        float drawDistance = 5.0f;

        bool cursorHidden = true;
        uint64_t lastMoveTime = 0;

        /// Set when the cursor is facing the camera, so the rim reads as a ring
        /// rather than an edge-on line.
        glm::vec3 cameraPosition { 0.0f };

};
