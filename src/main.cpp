#include "ofMain.h"
#include "ofApp.h"

//========================================================================
int main() {

	int w = 720;
	int h = 480;
	
	// setup the GL context
#ifdef TARGET_OPENGLES
	ofGLESWindowSettings settings;
	settings.glesVersion = 2;
	//settings.glesVersion = 3;
	//settings.setSize(w, h);
	settings.windowMode = OF_FULLSCREEN;
	ofCreateWindow(settings);
#else
	ofGLFWWindowSettings settings;
	//settings.setGLVersion(2, 1);
	//settings.setGLVersion(3, 2);
	settings.setGLVersion(3, 1);
	settings.numSamples = 0;
	//settings.setSize(w, h);
	settings.windowMode = OF_FULLSCREEN;
	ofCreateWindow(settings);
#endif

	// this kicks off the running of my app
	// can be OF_WINDOW or OF_FULLSCREEN
	// pass in width and height too:
	ofRunApp(new ofApp());

}
