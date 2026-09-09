#pragma once

#include "ofMain.h"

#include <string>
#include <vector>

// ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~
// Ported from public/js/drawing/palette.js
//
// Twelve swatches on a clock face, spawned at the hand that summoned them. The
// colours are a subset of the NAPLPS palette (see public/docs/palette.txt), so
// nothing chosen here has to be approximated at encode time.
// ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~

class Palette : public ofNode {

    public:

        struct Swatch {
            std::string name;
            ofColor color;
            glm::vec3 position; // in palette-local space
        };

        void setup(float radius = 0.6f, float swatchSize = 0.08f);

        void draw();

        /// Selects the swatch nearest `worldPosition`, if one is within reach.
        /// Returns true when the hand actually landed on a colour.
        bool hitTest(const glm::vec3 & worldPosition, float threshold = 0.1f);

        void select(int index);
        int getSelectedIndex() const { return selectedIndex; }
        ofColor getSelectedColor() const { return swatches[selectedIndex].color; }

        const std::vector<Swatch> & getSwatches() const { return swatches; }

        bool visible = false;

        /// While a choice is being confirmed, every swatch but the chosen one is
        /// hidden and that one blinks. -1 means "show them all".
        int soloIndex = -1;
        bool showSelectionRing = true;

    private:

        std::vector<Swatch> swatches;
        int selectedIndex = 0;
        float radius = 0.6f;
        float swatchSize = 0.08f;

};
