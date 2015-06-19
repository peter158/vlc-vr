
# VLC-VR 

This is a movie player based on libvlc that can render to the Oculus Rift on any platform supported by SDL2, OpenGL, and VLC (Linux, Windows, OSX).  It supports distortion rendering to planar, cylinder, and dome meshes and also renders side-by-side and over-under 3D formats.

# Usage
$ vlc-vr [options] video-path

# Options
* -f - Render to the rift on startup, otherwise use F2 or F9 to do this.
* -d[1-3] - Sets the initial screen distortion mode. (1=None,2=Dome,3=Cylindrical) 
* -s[1-3] - Sets the video source 3D stereo mode. (1=None,2=SBS,3=Over/Under)
* example to play an SBS w/ Dome projection:  ./vlc-vr -f -s2 -d2 file

## Settings
* F2 or F9 toggles the window to the Rift and back (ONLY for extended mode).
* SPACE: recenter the view to the current HMD orientation.
* Right/Left: Skip forward/backward in the video.
* Up/Down: Skip forward/backward fast.
* PgUp/PgDn: Skip forward/backward superfast.
* r: cycle through different modes 3D (None -> SBS -> Over/Under).
* w/s: increase/decrease size of screen projected screen.
* a/d: increase/decrease distance of screen from viewer.
* '1,2,3' change screen distortion modes (None -> Dome -> Cylinder).
* ESC: Quit the player.

### Compile from source:
* Dependencies: sdl2 glew git g++ cmake libvlc
 * apt-get install git libsdl2-dev libglew-dev cmake libvlc
 * For older debian (squeeze or wheezy) follow: http://backports.debian.org/Instructions/
   * apt-get -t wheezy-backports install "libsdl2-dev"
* Download the oculus rift sdk 0.4.4 (0.5 not supported yet), extract somewhere, and compile it:
 * https://developer.oculus.com/downloads/#version=pc-0.4.4-beta
* Download and compile vlc-vr:

 ```
sudo apt-get install git
git clone https://github.com/jdtaylor/vlc-vr.git
export OVR_ROOT="/path/to/your/compiled/oculus/ovr_sdk_linux_0.4.4"
export CXXFLAGS="-Ofast -fomit-frame-pointer -march=native" 
mkdir build && cd build
cmake ../vlc-vr
make 
./vlc-vr
 ```

* On Linux the Oculus SDK only supports extended mode.. whether using Xinerama or as separate displays via the DISPLAY environment variable.
* Please make sure your oculusd or ovrd service is running before running vlc-vr.
* Use F2 or F9 key to toggle to the rift and back.
* Sometimes if the rift is turned off and back on while Xorg is running, judder starts and I havn't found a way to make it go away without an X shutdown and restart w/ the Rift on.


