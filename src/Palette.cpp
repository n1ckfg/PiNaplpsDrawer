#include "Palette.h"

#include "NodeUtils.h"

#include <cmath>

//--------------------------------------------------------------
void Palette::setup(float _radius, float _swatchSize) {
    radius = _radius;
    swatchSize = _swatchSize;

    // Twelve of the sixteen NAPLPS colours, arranged clockwise from noon. The
    // four greys left out are the ones hardest to tell apart at swatch size.
    const struct { const char * name; int hex; } table[] = {
        { "Black",       0x000000 }, // 12 o'clock
        { "Gray2",       0x404040 }, //  1
        { "Gray4",       0x808080 }, //  2
        { "Blue",        0x0000FF }, //  3
        { "BlueMagenta", 0xB400FC }, //  4
        { "PinkishRed",  0xFC0090 }, //  5
        { "OrangeRed",   0xFC4800 }, //  6
        { "Yellow",      0xFFFF00 }, //  7
        { "YellowGreen", 0x48FC00 }, //  8
        { "Greenish",    0x00FC90 }, //  9
        { "BlueGreen",   0x00B4FC }, // 10
        { "White",       0xFFFFFF }  // 11
    };

    swatches.clear();
    swatches.reserve(12);

    for (int i = 0; i < 12; i++) {
        // Start at the top and sweep round; -PI/2 puts index 0 at 12 o'clock.
        const float angle = ((float)i / 12.0f) * TWO_PI - HALF_PI;

        Swatch swatch;
        swatch.name = table[i].name;
        swatch.color = ofColor::fromHex(table[i].hex);
        swatch.position = glm::vec3(std::cos(angle) * radius, std::sin(angle) * radius, 0.0f);
        swatches.push_back(swatch);
    }

    selectedIndex = 0;
}

//--------------------------------------------------------------
void Palette::select(int index) {
    selectedIndex = std::max(0, std::min((int)swatches.size() - 1, index));
}

//--------------------------------------------------------------
bool Palette::hitTest(const glm::vec3 & worldPosition, float threshold) {
    glm::vec3 localPos = NapDraw::worldToLocal(*this, worldPosition);
    localPos.z = 0.0f; // flatten onto the palette plane

    for (size_t i = 0; i < swatches.size(); i++) {
        const float dist = glm::distance(localPos, swatches[i].position);
        if (dist < swatchSize + threshold) {
            select((int)i);
            return true;
        }
    }
    return false;
}

//--------------------------------------------------------------
void Palette::draw() {
    if (!visible) return;

    ofPushMatrix();
    ofMultMatrix(getGlobalTransformMatrix());

    // The palette has to stay readable against the drawing behind it, so it
    // ignores depth entirely rather than being clipped by strokes it overlaps.
    // DrawingMode keeps depth testing on for the scene, so it goes back on
    // below.
    ofPushStyle();
    ofDisableDepthTest();

    for (size_t i = 0; i < swatches.size(); i++) {
        if (soloIndex >= 0 && (int)i != soloIndex) continue;

        const Swatch & swatch = swatches[i];

        // Black ring outside, white ring inside: together they keep a swatch
        // visible whether the background behind it is light or dark.
        ofFill();
        ofSetColor(0);
        ofDrawCircle(swatch.position, swatchSize * 1.2f);
        ofSetColor(255);
        ofDrawCircle(swatch.position, swatchSize * 1.1f);

        ofSetColor(swatch.color);
        ofDrawCircle(swatch.position, swatchSize);
    }

    if (showSelectionRing && soloIndex < 0 && selectedIndex < (int)swatches.size()) {
        ofNoFill();
        ofSetLineWidth(2.0f);
        ofSetColor(255);
        ofDrawCircle(swatches[selectedIndex].position, swatchSize * 1.3f);
        ofFill();
    }

    ofEnableDepthTest();
    ofPopStyle();
    ofPopMatrix();
    ofSetColor(255);
}
