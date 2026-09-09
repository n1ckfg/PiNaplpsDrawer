#sudo apt-get update
#sudo apt-get install -y xvfb

DIR=$PWD

cd ../../../addons
#git clone https://github.com/n1ckfg/ofxCv
#git clone https://github.com/n1ckfg/ofxCvPiCam
git clone https://github.com/n1ckfg/ofxNaplps

# the websocket server the player listens on (ofxPoco ships with oF)
git clone https://github.com/n1ckfg/ofxHTTP
git clone https://github.com/n1ckfg/ofxIO
git clone https://github.com/n1ckfg/ofxMediaType
git clone https://github.com/n1ckfg/ofxNetworkUtils
git clone https://github.com/n1ckfg/ofxSSLManager
git clone https://github.com/n1ckfg/ofxJSON
git clone https://github.com/n1ckfg/ofxCrypto

cd $DIR
