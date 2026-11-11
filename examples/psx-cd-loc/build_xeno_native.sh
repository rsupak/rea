#!/bin/sh
set -eu
cd "$(dirname "$0")"
clang++ -std=c++20 -Wall -Wextra -Wpedantic -ObjC++ -fobjc-arc \
  -I include \
  src/psx_cd_loc.cpp src/psx_cd_image.cpp src/psx_disc.cpp src/psx_catalog.cpp \
  src/psx_audio.cpp src/XenoNative.mm \
  -framework Cocoa -framework AudioToolbox \
  -o XenoNative
rm -rf XenoNative.app
mkdir -p XenoNative.app/Contents/MacOS
mv XenoNative XenoNative.app/Contents/MacOS/XenoNative
cat > XenoNative.app/Contents/Info.plist << 'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleExecutable</key>
  <string>XenoNative</string>
  <key>CFBundleIdentifier</key>
  <string>local.xeno.native</string>
  <key>CFBundleName</key>
  <string>XenoNative</string>
  <key>CFBundlePackageType</key>
  <string>APPL</string>
  <key>CFBundleShortVersionString</key>
  <string>0.1</string>
  <key>LSMinimumSystemVersion</key>
  <string>11.0</string>
</dict>
</plist>
EOF
codesign -s - XenoNative.app
echo "built $(pwd)/XenoNative.app"
