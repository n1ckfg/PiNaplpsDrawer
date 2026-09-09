#pragma once

#include "ofMain.h"

#include <cmath>
#include <vector>

// ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~
// The handful of Three.js Object3D operations the drawing code relies on that
// ofNode doesn't already provide.
//
// ofNode covers most of it: setParent(parent, true) is Three's attach(), and
// setGlobalPosition/setGlobalOrientation both divide out the parent transform
// the way Three's world-space setters do. What's missing is the scale half of
// that pair, and the two point-transform helpers.
// ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~

namespace NapDraw {

// Object3D.localToWorld()
inline glm::vec3 localToWorld(const ofNode & node, const glm::vec3 & p) {
    const glm::vec4 v = node.getGlobalTransformMatrix() * glm::vec4(p, 1.0f);
    return glm::vec3(v) / v.w;
}

// Object3D.worldToLocal()
inline glm::vec3 worldToLocal(const ofNode & node, const glm::vec3 & p) {
    const glm::vec4 v = glm::inverse(node.getGlobalTransformMatrix()) * glm::vec4(p, 1.0f);
    return glm::vec3(v) / v.w;
}

// The counterpart to ofNode::setGlobalPosition/setGlobalOrientation, which the
// core leaves out. A zero component in the parent's scale would divide by zero,
// so it's treated as 1 -- a degenerate parent scale is a bug elsewhere, and
// propagating NaN into the scene graph only hides where it came from.
inline void setGlobalScale(ofNode & node, const glm::vec3 & worldScale) {
    ofNode * parent = node.getParent();
    if (parent == nullptr) {
        node.setScale(worldScale);
        return;
    }

    glm::vec3 parentScale = parent->getGlobalScale();
    for (int i = 0; i < 3; i++) {
        if (std::abs(parentScale[i]) < 1e-9f) parentScale[i] = 1.0f;
    }
    node.setScale(worldScale / parentScale);
}

// worldscale.js's _setWorldTransform: place a node in world space regardless of
// where it currently sits in the hierarchy.
inline void setWorldTransform(ofNode & node,
                              const glm::vec3 & position,
                              const glm::quat & orientation,
                              const glm::vec3 & scale) {
    node.setGlobalPosition(position);
    node.setGlobalOrientation(orientation);
    setGlobalScale(node, scale);
}

// Turns a node to face a point, the way Three's Object3D.lookAt() does for the
// flat, camera-facing quads (palette swatches, controller rims).
inline void faceToward(ofNode & node, const glm::vec3 & target) {
    node.lookAt(target, glm::vec3(0.0f, 1.0f, 0.0f));
}

// ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~
// Ramer-Douglas-Peucker, ported from rdpSimplify() in index.html.
//
// The encoder writes every point it's given, so an unsimplified hand-drawn
// stroke -- one point per frame, for as long as the hand was moving -- blows
// past the ~30 KB a Tezos token can hold. This is what keeps a drawing mintable.
// ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~

inline float rdpPointLineDist(const glm::vec2 & p, const glm::vec2 & a, const glm::vec2 & b) {
    float dx = b.x - a.x;
    float dy = b.y - a.y;
    const float lenSq = dx * dx + dy * dy;

    if (lenSq == 0.0f) {
        dx = p.x - a.x;
        dy = p.y - a.y;
        return std::sqrt(dx * dx + dy * dy);
    }

    float t = ((p.x - a.x) * dx + (p.y - a.y) * dy) / lenSq;
    t = std::max(0.0f, std::min(1.0f, t));

    dx = p.x - (a.x + t * dx);
    dy = p.y - (a.y + t * dy);
    return std::sqrt(dx * dx + dy * dy);
}

inline std::vector<glm::vec2> rdpSimplify(const std::vector<glm::vec2> & points, float epsilon) {
    if (points.size() <= 2) return points;

    const glm::vec2 & start = points.front();
    const glm::vec2 & end = points.back();

    float maxDist = 0.0f;
    size_t maxIdx = 0;
    for (size_t i = 1; i + 1 < points.size(); i++) {
        const float dist = rdpPointLineDist(points[i], start, end);
        if (dist > maxDist) {
            maxDist = dist;
            maxIdx = i;
        }
    }

    if (maxDist > epsilon) {
        const std::vector<glm::vec2> leftIn(points.begin(), points.begin() + maxIdx + 1);
        const std::vector<glm::vec2> rightIn(points.begin() + maxIdx, points.end());

        std::vector<glm::vec2> left = rdpSimplify(leftIn, epsilon);
        const std::vector<glm::vec2> right = rdpSimplify(rightIn, epsilon);

        // left.slice(0, -1).concat(right) -- the split point is in both halves.
        left.pop_back();
        left.insert(left.end(), right.begin(), right.end());
        return left;
    }

    return { start, end };
}

} // namespace NapDraw
