# Performance Improvement Suggestions for PiNaplpsPlayer

Deploying openFrameworks applications on resource-constrained devices like the Raspberry Pi 3 requires careful optimization, particularly concerning the render loop and GPU usage. Based on the architecture and codebase of `PiNaplpsPlayer`, here are several actionable suggestions to improve performance.

## 1. Cache the FBO Rendering (Stop Redrawing Every Frame)
**Current State:** 
In `ofApp::draw()`, the `telidon` renderer draws all parsed NAPLPS commands into the `fbo` every single frame at 60 FPS, clearing the FBO first with `ofBackground(0)`.
**Why it's a problem:** 
For complex drawings with thousands of commands, issuing those OpenGL calls 60 times a second will heavily tax the Pi's GPU and CPU.
**Solution:** 
Only update the `fbo` when necessary. Since NAPLPS drawings are static once finished, you can check `telidon.isFinished()` and avoid clearing and re-rendering.
```cpp
// 1. Add a dirty flag in ofApp.h: bool bFboDirty = true;

// 2. In ofApp::draw():
void ofApp::draw() {
    // Only redraw the FBO if progressive draw is still running, or if a redraw was forced
    if (!telidon.isFinished() || bFboDirty) {
        fbo.begin();
        ofBackground(0);
        ofPushMatrix();
        ofTranslate(drawOffset.x, drawOffset.y);
        telidon.draw();
        ofPopMatrix();
        fbo.end();
        
        if (telidon.isFinished()) {
            bFboDirty = false; // Stop updating FBO now that it's complete
        }
    }
    
    // Always draw the cached FBO to the screen
    fbo.draw(0, 0, 720, 480);
    // ...
}
```
*Note: Make sure to set `bFboDirty = true;` whenever `telidon.reset()` is called, a new file is loaded, or the window is resized.*

## 2. Reduce the Frame Rate
**Current State:** 
`ofSetFrameRate(60);` is called in `setup()`.
**Why it's a problem:** 
A Raspberry Pi 3 may struggle to maintain 60 FPS while doing heavy vector rendering, causing thermal throttling or stuttering.
**Solution:** 
Vector graphics like NAPLPS, especially during progressive drawing, do not require 60 FPS for a smooth appearance. Change this to 30 FPS (`ofSetFrameRate(30);`) or even lower (15-24 FPS). This instantly halves the general workload on the processor.

## 3. Disable Alpha Blending if Unnecessary
**Current State:**
`ofEnableAlphaBlending();` is called in `setup()`.
**Why it's a problem:**
Alpha blending adds computational overhead to every pixel drawn.
**Solution:**
If the NAPLPS graphics are strictly opaque (which the standard typically is, unless this specific implementation adds transparency), you can disable alpha blending by commenting out `ofEnableAlphaBlending()` or explicitly calling `ofDisableAlphaBlending()`.

## 4. Optimize FBO Formats and Scaling
**Current State:**
- The FBO is allocated with an alpha channel: `fbo.allocate(720, 540, GL_RGBA);`
- The FBO is drawn with scaling: `fbo.draw(0, 0, 720, 480);`
**Why it's a problem:**
Using `GL_RGBA` uses 33% more memory and bandwidth than `GL_RGB`. Furthermore, scaling a 720x540 texture to 720x480 at runtime adds a texture sampling overhead on the GPU.
**Solution:**
- If alpha isn't needed, allocate using `GL_RGB` instead of `GL_RGBA`.
- If the target aspect ratio is 720x480, consider allocating the FBO at that exact size or ensuring `telidon` draws natively into the 720x480 coordinate space. This avoids the squish/scale step when drawing the FBO to the screen.

## 5. Cache Debug Text Generation
**Current State:**
When `showInfo` is true, a large `std::string` is constructed via multiple concatenations and `ofToString()` conversions every frame in the `draw()` loop.
**Why it's a problem:**
String allocations in C++ are surprisingly slow and can trigger heap fragmentation on low-memory devices.
**Solution:**
If the info overlay is left on frequently, cache the generated string in `update()` and only rebuild it if the state changes (e.g., if `telidon.isFinished()`, `connections`, or `received` changes).
