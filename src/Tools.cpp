#include "Tools.h"

#include "NodeUtils.h"

#include <algorithm>
#include <cmath>

// ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~
// Stroke
// ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~

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

    // Fall back to a locally computed taper when the cache is stale, so this
    // stays const and callers don't have to remember to prime it.
    std::vector<float> scratchPressures;
    if (pressures.size() != nPoints) {
        scratchPressures.reserve(nPoints);
        for (size_t i = 0; i < nPoints; i++) {
            const float t = (float)i / (float)std::max<size_t>(1, nPoints - 1) * PI;
            scratchPressures.push_back(std::sqrt((1.0f - std::cos(t)) * 0.5f));
        }
    }
    const std::vector<float> & pressureRef = scratchPressures.empty() ? pressures : scratchPressures;

    leftEdge.reserve(nPoints);
    rightEdge.reserve(nPoints);

    for (size_t i = 0; i < nPoints; i++) {
        const glm::vec3 & p = points[i];

        float radius;
        if (i == 0 || i == lastIndex) {
            // Pin the ends narrow, or the taper flares out into a spade shape.
            radius = 0.01f;
        } else {
            const float taper = std::pow((float)(lastIndex - i) / (float)std::max<size_t>(1, lastIndex), taperPower);
            const float pressure = (i < pressureRef.size()) ? pressureRef[i] : 1.0f;
            radius = std::max(minThickness * thickness, taper * pressure * thickness);
        }

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

        const glm::vec3 perp = glm::normalize(glm::cross(tangent, normal));

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
std::vector<glm::vec3> Stroke::toBrushOutline() const {
    std::vector<glm::vec3> outline;
    if (points.size() < 2) return outline;

    std::vector<glm::vec3> leftEdge, rightEdge;
    buildEdges(leftEdge, rightEdge);

    outline.reserve(leftEdge.size() + rightEdge.size());
    outline.insert(outline.end(), leftEdge.begin(), leftEdge.end());
    outline.insert(outline.end(), rightEdge.rbegin(), rightEdge.rend());
    return outline;
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
