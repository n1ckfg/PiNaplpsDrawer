#pragma once

#include "ofMain.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

// ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~
// Ported from public/js/drawing/tools.js
//
// A Stroke is a 3D polyline that knows how to thicken itself into a ribbon; a
// Frame is the collection of them, parented to the world node so the two-handed
// grab moves the drawing as a whole.
// ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~

/// Controllers are keyed by string so the two hands ("0", "1") and the mouse
/// ("mouse") can all hold a stroke at once, as they do in the JS.
using ControllerId = std::string;

class Stroke {

    public:

        Stroke() = default;
        explicit Stroke(const ofColor & _color) : color(_color) {}

        std::vector<glm::vec3> points;
        ofColor color = ofColor(255);

        int smoothReps = 10;
        int splitReps = 2;
        float thickness = 0.25f;  // brush width in world units
        float taperPower = 0.4f;  // taper exponent for the ends
        float minThickness = 0.3f; // floor, as a multiple of thickness

        std::vector<float> pressures;

        void addPoint(const glm::vec3 & point);

        /// Subdivide + smooth, run at the end of a stroke. Hand-drawn input is
        /// one point per frame, so it arrives both jagged and unevenly spaced.
        void refine();

        /// Best-fit plane normal (Newell's method). The brush ribbon is built in
        /// this plane, which is what keeps a stroke readable from the angle it
        /// was drawn at.
        glm::vec3 computeNormal() const;

        /// Pushes the stroke along its own normal, so overlapping strokes don't
        /// z-fight. Frame spaces successive strokes this way.
        void offsetAlongNormal(float amount);

        /// Sine falloff: fattest in the middle, tapering to nothing at the ends.
        void computePressures();

        /// The ribbon, as a drawable triangle mesh.
        const ofVboMesh & getBrushMesh();

        /// How far simplification may move a point of the exported centreline,
        /// in frame widths -- 1 is the whole drawing, so this is about a pixel
        /// of a 640-wide one.
        static constexpr float kBrushSimplify = 0.002f;

        /// Carries a point of this stroke into the flat 0..1 space the NAPLPS
        /// encoder works in. DrawingMode owns it, being the only thing that
        /// knows the camera and what the two-handed grab has left on the world.
        using ProjectFn = std::function<glm::vec2(const glm::vec3 &)>;

        /// A centreline and its brush radii, both in that flat space.
        struct ScreenPath {
            std::vector<glm::vec2> points;
            std::vector<float> radii;
        };

        /// Brush radius at one point: tapered along the stroke and scaled by
        /// pressure, with both tips pinched so it doesn't start with a flare.
        float radiusAt(size_t i) const;

        /// Projects the stroke, measuring the brush width in the projection
        /// rather than in its own plane: a point offset along widthAxis is
        /// projected beside each centre point, so the width follows perspective
        /// and survives however the stroke was drawn -- including one drawn
        /// straight at the camera, which has no width in its own plane at all.
        ScreenPath toScreenPath(const ProjectFn & project, const glm::vec3 & widthAxis) const;

        /// The stroke as a run of short filled polygons -- one quad per segment
        /// of the simplified centreline -- which is the shape NAPLPS wants.
        ///
        /// A quad built on one segment's own perpendicular is convex, so every
        /// renderer fills it alike whichever winding rule it uses, where a single
        /// long outline of the whole stroke crosses itself at every tight turn
        /// and fills differently on each. Four points is also short enough that
        /// the encoder's running delta cursor is reset before its rounding error
        /// can accumulate into visible drift. Consecutive quads reach a little
        /// way into each other so no gap shows at a turn.
        std::vector<std::vector<glm::vec2>> toBrushQuads(const ProjectFn & project,
                                                         const glm::vec3 & widthAxis,
                                                         float epsilon = kBrushSimplify) const;

        /// Invalidates the cached mesh. Needed after anything that moves points.
        void setDirty() { meshDirty = true; }

    private:

        /// Walks the centreline and offsets each point perpendicular to it by
        /// the local radius. The drawable mesh is built from this; what gets
        /// encoded is not -- see toBrushQuads().
        void buildEdges(std::vector<glm::vec3> & leftEdge,
                        std::vector<glm::vec3> & rightEdge) const;

        void splitStroke();
        void smoothStroke();

        ofVboMesh brushMesh;
        bool meshDirty = true;

};

class Frame : public ofNode {

    public:

        /// Points dropped from each end of a stroke. The first frames of a
        /// gesture are where the classifier is least sure, and the last few are
        /// contaminated by the hand already moving away to end the stroke.
        static constexpr int kPointsTrimStart = 1;
        static constexpr int kPointsTrimEnd = 5;

        static constexpr uint64_t kFlickerDuration = 300; // ms
        static constexpr uint64_t kFlickerInterval = 50;  // ms

        void setup(ofNode & worldOrigin);

        /// Advances the undo/clear flicker animations. Call once per frame.
        void update();
        void draw();

        bool hasActiveStroke(const ControllerId & id) const;

        void beginStroke(const glm::vec3 & worldPosition, const ControllerId & id, const ofColor & color);
        void continueStroke(const glm::vec3 & worldPosition, const ControllerId & id);
        void endStroke(const ControllerId & id);

        /// Removes the last completed stroke. Returns false when there's none.
        bool undo();
        /// undo(), but the doomed stroke blinks first so the gesture is legible.
        bool undoWithFlicker();

        void clear();
        /// clear(), after blinking the whole drawing.
        void clearWithFlicker();

        bool isFlickering() const { return flickerMode != FlickerMode::None; }

        const std::vector<Stroke> & getStrokes() const { return strokes; }
        size_t getStrokeCount() const { return strokes.size(); }

    private:

        void rebuildTempStroke(const ControllerId & id);

        enum class FlickerMode { None, Undo, Clear };

        std::vector<Stroke> strokes;
        ofNode * worldOrigin = nullptr;

        // One in-progress stroke per controller.
        std::map<ControllerId, Stroke> activeStrokes;
        /// The raw arrival buffer. A point isn't committed until kPointsTrimEnd
        /// more have landed behind it, which is how the tail gets trimmed
        /// without knowing in advance where the stroke ends.
        std::map<ControllerId, std::vector<glm::vec3>> rawPoints;
        /// The committed prefix, rebuilt into a mesh so the stroke is visible
        /// while it's still being drawn.
        std::map<ControllerId, Stroke> tempStrokes;

        /// Spacing between successive strokes along their normals.
        int strokeCounter = 0;
        static constexpr float kZOffsetPerStroke = 0.002f;

        FlickerMode flickerMode = FlickerMode::None;
        uint64_t flickerStart = 0;
        /// The stroke being blinked away by an undo, kept out of `strokes` so it
        /// can't be undone twice, but still drawn until the blink finishes.
        Stroke flickerStroke;

};
