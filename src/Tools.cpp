#include "Tools.h"

#include "NodeUtils.h"

#include <algorithm>
#include <cmath>

// ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~
// Stroke
// ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~

namespace {

/// The pinch at each end of a stroke, in the stroke's own units.
constexpr float kTipRadius = 0.01f;

/// These three are in frame widths, the 0..1 space toBrushQuads() works in.
constexpr float kMinStep = 0.0005f;   // 1/2048: a shorter step is a rounding error to the encoder
constexpr float kMinRadius = 0.0005f; // keeps a quad from collapsing into a line

/// How far inside the frame a clamped point is held. Each delta the encoder
/// writes can fall a quantum short of where it was asked for, and a quad is four
/// of them, so a point pinned to the very edge can decode just outside it -- and
/// both renderers drop a point that lands outside rather than pulling it back.
constexpr float kFrameMargin = 4.0f * kMinStep;

/// Ramer-Douglas-Peucker over indices rather than points, so that anything held
/// per point -- here the brush radius -- can follow the centreline through it.
/// NapDraw::rdpSimplify() returns points, which loses that pairing.
std::vector<size_t> simplifyIndices(const std::vector<glm::vec2> & points,
                                    float epsilon, size_t first, size_t last) {
    if (last - first < 2) return { first, last };

    float maxDist = 0.0f;
    size_t maxIdx = first;
    for (size_t i = first + 1; i < last; i++) {
        const float dist = NapDraw::rdpPointLineDist(points[i], points[first], points[last]);
        if (dist > maxDist) {
            maxDist = dist;
            maxIdx = i;
        }
    }

    if (maxDist > epsilon) {
        std::vector<size_t> keep = simplifyIndices(points, epsilon, first, maxIdx);
        const std::vector<size_t> right = simplifyIndices(points, epsilon, maxIdx, last);
        keep.pop_back(); // maxIdx opens the right half
        keep.insert(keep.end(), right.begin(), right.end());
        return keep;
    }
    return { first, last };
}

/// Simplifies a centreline, carrying its radii along.
Stroke::ScreenPath simplifyPath(const Stroke::ScreenPath & path, float epsilon) {
    if (path.points.size() < 3) return path;

    Stroke::ScreenPath out;
    for (size_t i : simplifyIndices(path.points, epsilon, 0, path.points.size() - 1)) {
        out.points.push_back(path.points[i]);
        out.radii.push_back(path.radii[i]);
    }
    return out;
}

/// How far each quad has to reach past a corner to cover the wedge the next one
/// leaves there: radius * tan(half the turn), which is nothing on a straight run
/// and a whole radius at a right angle. Capped there, so a hairpin gets a blunt
/// corner instead of a spike.
std::vector<float> cornerReach(const std::vector<glm::vec2> & points,
                               const std::vector<float> & radii) {
    std::vector<float> reach(points.size(), 0.0f);

    for (size_t i = 1; i + 1 < points.size(); i++) {
        const glm::vec2 into = points[i] - points[i - 1];
        const glm::vec2 outOf = points[i + 1] - points[i];
        const float intoLen = glm::length(into);
        const float outLen = glm::length(outOf);
        if (intoLen < kMinStep || outLen < kMinStep) continue;

        const float dot = glm::dot(into, outOf) / (intoLen * outLen);
        const float cross = std::abs(into.x * outOf.y - into.y * outOf.x) / (intoLen * outLen);
        const float halfTurn = cross / std::max(1e-6f, 1.0f + dot); // tan(turn / 2)

        reach[i] = std::min(1.0f, halfTurn) * std::max(radii[i], kMinRadius);
    }
    return reach;
}

/// Whether any part of a polygon lies inside the 0..1 frame.
bool touchesFrame(const std::vector<glm::vec2> & poly) {
    if (poly.empty()) return false;

    glm::vec2 lo = poly.front();
    glm::vec2 hi = poly.front();
    for (const glm::vec2 & p : poly) {
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    }
    return lo.x <= 1.0f && hi.x >= 0.0f && lo.y <= 1.0f && hi.y >= 0.0f;
}

/// Pulls a polygon inside the frame, and far enough inside to still be there
/// once it has been through the encoder.
std::vector<glm::vec2> clampToFrame(const std::vector<glm::vec2> & poly) {
    std::vector<glm::vec2> out;
    out.reserve(poly.size());
    for (const glm::vec2 & p : poly) {
        out.push_back(glm::vec2(ofClamp(p.x, kFrameMargin, 1.0f - kFrameMargin),
                                ofClamp(p.y, kFrameMargin, 1.0f - kFrameMargin)));
    }
    return out;
}

} // namespace

//--------------------------------------------------------------
void Stroke::addPoint(const glm::vec3 & point) {
    points.push_back(point);
    meshDirty = true;
}

//--------------------------------------------------------------
void Stroke::splitStroke() {
    // Insert a midpoint between each existing pair. The index step of 2 keeps
    // the walk on the original points as the vector grows underneath it.
    for (size_t i = 1; i < points.size(); i += 2) {
        const glm::vec3 mid = (points[i] + points[i - 1]) * 0.5f;
        points.insert(points.begin() + i, mid);
    }
    meshDirty = true;
}

//--------------------------------------------------------------
void Stroke::smoothStroke() {
    if (points.size() < 3) return;

    // Weighted 3-tap average, heavily biased towards the centre point, applied
    // in place so each pass also feeds on the previous point's new value.
    const float weight = 18.0f;
    const float scale = 1.0f / (weight + 2.0f);
    const size_t nPointsMinusTwo = points.size() - 2;

    for (size_t i = 1; i < nPointsMinusTwo; i++) {
        const glm::vec3 & lower = points[i - 1];
        const glm::vec3 & upper = points[i + 1];
        glm::vec3 & center = points[i];

        center = (lower + weight * center + upper) * scale;
    }
    meshDirty = true;
}

//--------------------------------------------------------------
void Stroke::refine() {
    if (points.size() < 2) return;

    for (int i = 0; i < splitReps; i++) {
        splitStroke();
        smoothStroke();
    }
    for (int i = 0; i < smoothReps - splitReps; i++) {
        smoothStroke();
    }

    // The point count changed, so any pressures computed earlier no longer line
    // up; they're recomputed on the next mesh or outline build.
    pressures.clear();
    meshDirty = true;
}

//--------------------------------------------------------------
glm::vec3 Stroke::computeNormal() const {
    if (points.size() < 3) return glm::vec3(0.0f, 0.0f, 1.0f);

    // Newell's method: works for a non-planar polygon, which a hand-drawn
    // stroke always is.
    glm::vec3 normal(0.0f);
    for (size_t i = 0; i < points.size(); i++) {
        const glm::vec3 & curr = points[i];
        const glm::vec3 & next = points[(i + 1) % points.size()];

        normal.x += (curr.y - next.y) * (curr.z + next.z);
        normal.y += (curr.z - next.z) * (curr.x + next.x);
        normal.z += (curr.x - next.x) * (curr.y + next.y);
    }

    // A perfectly straight stroke has no plane; pick one rather than normalizing
    // a zero vector into NaN.
    if (glm::dot(normal, normal) < 0.001f) return glm::vec3(0.0f, 0.0f, 1.0f);

    return glm::normalize(normal);
}

//--------------------------------------------------------------
void Stroke::offsetAlongNormal(float amount) {
    if (points.size() < 3 || amount == 0.0f) return;

    const glm::vec3 normal = computeNormal();
    for (auto & point : points) {
        point += normal * amount;
    }
    meshDirty = true;
}

//--------------------------------------------------------------
void Stroke::computePressures() {
    pressures.clear();
    const size_t n = points.size();
    if (n == 0) return;

    pressures.reserve(n);
    for (size_t i = 0; i < n; i++) {
        const float t = (float)i / (float)std::max<size_t>(1, n - 1) * PI;
        pressures.push_back(std::sqrt((1.0f - std::cos(t)) * 0.5f));
    }
}

//--------------------------------------------------------------
void Stroke::buildEdges(std::vector<glm::vec3> & leftEdge,
                        std::vector<glm::vec3> & rightEdge) const {
    leftEdge.clear();
    rightEdge.clear();
    if (points.size() < 2) return;

    const glm::vec3 normal = computeNormal();
    const size_t nPoints = points.size();
    const size_t lastIndex = nPoints - 1;

    leftEdge.reserve(nPoints);
    rightEdge.reserve(nPoints);

    for (size_t i = 0; i < nPoints; i++) {
        const glm::vec3 & p = points[i];

        const float radius = radiusAt(i);

        glm::vec3 tangent;
        if (i == 0) {
            tangent = points[1] - p;
        } else if (i == lastIndex) {
            tangent = p - points[i - 1];
        } else {
            // Central difference, so the ribbon doesn't kink at each sample.
            tangent = points[i + 1] - points[i - 1];
        }

        const float tangentLength = glm::length(tangent);
        if (tangentLength < 0.0001f) {
            tangent = glm::vec3(1.0f, 0.0f, 0.0f);
        } else {
            tangent /= tangentLength;
        }

        // A stroke running along its own normal -- drawn straight at the camera,
        // say -- has no perpendicular there, so fall back to one across the
        // tangent rather than let the ribbon collapse to nothing.
        glm::vec3 perp = glm::cross(tangent, normal);
        if (glm::dot(perp, perp) < 1e-8f) {
            perp = glm::vec3(-tangent.y, tangent.x, 0.0f);
            if (glm::dot(perp, perp) < 1e-8f) perp = glm::vec3(0.0f, 1.0f, 0.0f);
        }
        perp = glm::normalize(perp);

        leftEdge.push_back(p + perp * radius);
        rightEdge.push_back(p - perp * radius);
    }
}

//--------------------------------------------------------------
const ofVboMesh & Stroke::getBrushMesh() {
    if (!meshDirty) return brushMesh;

    brushMesh.clear();
    brushMesh.setMode(OF_PRIMITIVE_TRIANGLES);
    meshDirty = false;

    if (points.size() < 2) return brushMesh;

    if (pressures.size() != points.size()) computePressures();

    std::vector<glm::vec3> leftEdge, rightEdge;
    buildEdges(leftEdge, rightEdge);
    if (leftEdge.size() < 2) return brushMesh;

    const size_t nPoints = leftEdge.size();

    // Left edge first, then the right, so an index into the right edge is just
    // nPoints + i.
    for (const auto & v : leftEdge) brushMesh.addVertex(v);
    for (const auto & v : rightEdge) brushMesh.addVertex(v);

    for (size_t i = 0; i + 1 < nPoints; i++) {
        const ofIndexType l0 = (ofIndexType)i;
        const ofIndexType l1 = (ofIndexType)(i + 1);
        const ofIndexType r0 = (ofIndexType)(nPoints + i);
        const ofIndexType r1 = (ofIndexType)(nPoints + i + 1);

        brushMesh.addIndex(l0); brushMesh.addIndex(r0); brushMesh.addIndex(l1);
        brushMesh.addIndex(l1); brushMesh.addIndex(r0); brushMesh.addIndex(r1);
    }

    return brushMesh;
}

//--------------------------------------------------------------
float Stroke::radiusAt(size_t i) const {
    if (points.size() < 2) return kTipRadius;

    const size_t lastIndex = points.size() - 1;
    if (i == 0 || i >= lastIndex) return kTipRadius;

    const float taper = std::pow((float)(lastIndex - i) / (float)std::max<size_t>(1, lastIndex), taperPower);

    // Fall back to a locally computed pressure when the cache is stale, so this
    // stays const and callers don't have to remember to prime it.
    const float pressure = (pressures.size() == points.size())
        ? pressures[i]
        : std::sqrt((1.0f - std::cos((float)i / (float)std::max<size_t>(1, lastIndex) * PI)) * 0.5f);

    return std::max(minThickness * thickness, taper * pressure * thickness);
}

//--------------------------------------------------------------
Stroke::ScreenPath Stroke::toScreenPath(const ProjectFn & project, const glm::vec3 & widthAxis) const {
    ScreenPath path;
    if (points.empty()) return path;

    const size_t lastIndex = points.size() - 1;

    for (size_t i = 0; i <= lastIndex; i++) {
        const glm::vec2 centre = project(points[i]);
        const glm::vec2 edge = project(points[i] + widthAxis * radiusAt(i));
        const float radius = glm::length(edge - centre);

        // A point the encoder can't tell from the last one costs four bytes and
        // says nothing -- but never drop the tip, or the stroke shortens, and
        // keep the widest radius of the points that fall together so a slow
        // passage doesn't come out thin.
        if (!path.points.empty() && i != lastIndex &&
            glm::length(centre - path.points.back()) < kMinStep) {
            path.radii.back() = std::max(path.radii.back(), radius);
            continue;
        }

        path.points.push_back(centre);
        path.radii.push_back(radius);
    }

    return path;
}

//--------------------------------------------------------------
std::vector<std::vector<glm::vec2>> Stroke::toBrushQuads(const ProjectFn & project,
                                                         const glm::vec3 & widthAxis,
                                                         float epsilon) const {
    std::vector<std::vector<glm::vec2>> quads;
    if (points.size() < 2) return quads;

    const ScreenPath path = simplifyPath(toScreenPath(project, widthAxis), epsilon);
    const std::vector<glm::vec2> & centre = path.points;
    const std::vector<float> & radii = path.radii;
    if (centre.empty()) return quads;

    const std::vector<float> reach = cornerReach(centre, radii);
    const size_t segments = centre.size() - 1;

    for (size_t i = 0; i < segments; i++) {
        const glm::vec2 & a = centre[i];
        const glm::vec2 & b = centre[i + 1];
        const float length = glm::length(b - a);
        if (length < kMinStep) continue;

        const glm::vec2 t = (b - a) / length;
        const glm::vec2 n = glm::vec2(-t.y, t.x);
        const float ra = std::max(radii[i], kMinRadius);
        const float rb = std::max(radii[i + 1], kMinRadius);

        // Reach into the neighbouring segments, but not past the stroke's own ends
        const glm::vec2 from = a - t * ((i > 0) ? reach[i] : 0.0f);
        const glm::vec2 to = b + t * ((i + 1 < segments) ? reach[i + 1] : 0.0f);

        const std::vector<glm::vec2> quad = {
            from + n * ra, to + n * rb, to - n * rb, from - n * ra
        };

        // The encoder silently drops a point outside the frame, and every point
        // after it in that polygon is a delta from the one dropped, so the rest
        // of the shape lands somewhere else entirely. Keep the quads that touch
        // the frame and clamp them into it; skip the rest.
        if (touchesFrame(quad)) quads.push_back(clampToFrame(quad));
    }

    // A stroke can land on a single spot -- drawn straight at the camera, or
    // held still. There was paint on it, so leave a dab rather than nothing.
    if (quads.empty()) {
        float r = kMinRadius;
        for (const float radius : radii) r = std::max(r, radius);

        const glm::vec2 c = centre.front();
        const std::vector<glm::vec2> dab = {
            glm::vec2(c.x - r, c.y - r), glm::vec2(c.x + r, c.y - r),
            glm::vec2(c.x + r, c.y + r), glm::vec2(c.x - r, c.y + r)
        };
        if (touchesFrame(dab)) quads.push_back(clampToFrame(dab));
    }

    return quads;
}

// ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~
// Frame
// ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~

//--------------------------------------------------------------
void Frame::setup(ofNode & _worldOrigin) {
    worldOrigin = &_worldOrigin;
    setParent(_worldOrigin);
}

//--------------------------------------------------------------
bool Frame::hasActiveStroke(const ControllerId & id) const {
    return activeStrokes.find(id) != activeStrokes.end();
}

//--------------------------------------------------------------
void Frame::beginStroke(const glm::vec3 & worldPosition, const ControllerId & id, const ofColor & color) {
    activeStrokes[id] = Stroke(color);
    tempStrokes[id] = Stroke(color);

    // Points are stored in the frame's own space, so the whole drawing moves
    // with the world node rather than each stroke needing to be transformed.
    rawPoints[id] = { NapDraw::worldToLocal(*this, worldPosition) };
}

//--------------------------------------------------------------
void Frame::continueStroke(const glm::vec3 & worldPosition, const ControllerId & id) {
    auto activeIt = activeStrokes.find(id);
    if (activeIt == activeStrokes.end()) return;

    std::vector<glm::vec3> & raw = rawPoints[id];
    raw.push_back(NapDraw::worldToLocal(*this, worldPosition));

    // Commit the point kPointsTrimEnd behind the newest one, and only once
    // we're past kPointsTrimStart. The tail sitting in the buffer is discarded
    // when the stroke ends, which is how both ends get trimmed.
    const long addIndex = (long)raw.size() - 1 - kPointsTrimEnd;
    if (addIndex >= kPointsTrimStart) {
        activeIt->second.addPoint(raw[addIndex]);
        rebuildTempStroke(id);
    }
}

//--------------------------------------------------------------
void Frame::rebuildTempStroke(const ControllerId & id) {
    auto activeIt = activeStrokes.find(id);
    if (activeIt == activeStrokes.end()) return;

    // The in-progress preview is the committed points as they stand, unrefined:
    // refining every frame would make the line crawl under the fingertip.
    Stroke & temp = tempStrokes[id];
    temp.color = activeIt->second.color;
    temp.points = activeIt->second.points;
    temp.pressures.clear();
    temp.setDirty();
}

//--------------------------------------------------------------
void Frame::endStroke(const ControllerId & id) {
    auto activeIt = activeStrokes.find(id);

    if (activeIt != activeStrokes.end() && activeIt->second.points.size() > 1) {
        Stroke finished = activeIt->second;

        finished.refine();
        finished.offsetAlongNormal(strokeCounter * kZOffsetPerStroke);
        strokeCounter++;

        strokes.push_back(std::move(finished));
    }

    activeStrokes.erase(id);
    tempStrokes.erase(id);
    rawPoints.erase(id);
}

//--------------------------------------------------------------
bool Frame::undo() {
    if (strokes.empty()) return false;
    strokes.pop_back();
    return true;
}

//--------------------------------------------------------------
bool Frame::undoWithFlicker() {
    if (strokes.empty()) return false;

    // Pull it out of the list immediately so a second undo can't take it again,
    // but keep drawing it until the blink is done.
    flickerStroke = strokes.back();
    strokes.pop_back();

    flickerMode = FlickerMode::Undo;
    flickerStart = ofGetElapsedTimeMillis();
    return true;
}

//--------------------------------------------------------------
void Frame::clearWithFlicker() {
    flickerMode = FlickerMode::Clear;
    flickerStart = ofGetElapsedTimeMillis();
    // The strokes stay put until the blink finishes -- update() clears them.
}

//--------------------------------------------------------------
void Frame::update() {
    if (flickerMode == FlickerMode::None) return;

    if (ofGetElapsedTimeMillis() - flickerStart < kFlickerDuration) return;

    if (flickerMode == FlickerMode::Clear) clear();

    flickerMode = FlickerMode::None;
    flickerStroke.points.clear();
}

//--------------------------------------------------------------
void Frame::clear() {
    strokes.clear();
    activeStrokes.clear();
    tempStrokes.clear();
    rawPoints.clear();
    strokeCounter = 0;

    if (worldOrigin != nullptr) {
        worldOrigin->setPosition(0.0f, 0.0f, 0.0f);
        worldOrigin->setOrientation(glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
        worldOrigin->setScale(1.0f);
    }
}

//--------------------------------------------------------------
void Frame::draw() {
    // Blink on a 50 ms square wave. In the JS this drove an empty line mesh, so
    // a clear flickered nothing visible; here it blinks the drawing itself,
    // which is what the shrinking-circle overlay is announcing.
    bool blinkOn = true;
    if (flickerMode != FlickerMode::None) {
        const uint64_t elapsed = ofGetElapsedTimeMillis() - flickerStart;
        blinkOn = ((elapsed / kFlickerInterval) % 2) == 0;
    }

    const bool hideAll = (flickerMode == FlickerMode::Clear) && !blinkOn;

    ofPushMatrix();
    ofMultMatrix(getGlobalTransformMatrix());

    if (!hideAll) {
        for (auto & stroke : strokes) {
            ofSetColor(stroke.color);
            stroke.getBrushMesh().draw();
        }

        for (auto & entry : tempStrokes) {
            Stroke & temp = entry.second;
            if (temp.points.size() < 2) continue;
            ofSetColor(temp.color);
            temp.getBrushMesh().draw();
        }
    }

    // The stroke an undo is currently eating blinks on its own.
    if (flickerMode == FlickerMode::Undo && blinkOn && flickerStroke.points.size() > 1) {
        ofSetColor(flickerStroke.color);
        flickerStroke.getBrushMesh().draw();
    }

    ofPopMatrix();
    ofSetColor(255);
}
