#!/bin/sh
# Cross-compile LiteWall for Windows x64 from Linux using llvm-mingw.
set -e
cd "$(dirname "$0")"
TC=${TC:-third_party/llvm-mingw/bin}
MPV=${MPV:-third_party/mpv}
mkdir -p build
$TC/x86_64-w64-mingw32-windres src/litewall.rc -O coff -o build/res.o
$TC/x86_64-w64-mingw32-clang -O2 -Wall -municode -mwindows -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 \
  -I$MPV/include src/litewall.c build/res.o -o build/LiteWall.exe \
  -L$MPV -lmpv -lcomdlg32 -lshell32 -lwtsapi32 -lpsapi -lcomctl32 -lole32 -lwindowscodecs -ldwmapi -luxtheme -luuid -luser32 -lgdi32 -ladvapi32 -s
rm -rf build/LiteWall && mkdir -p build/LiteWall dist
cp build/LiteWall.exe $MPV/libmpv-2.dll README.md LICENSE build/LiteWall/
(cd build && rm -f ../dist/LiteWall-v0.2.0-win-x64.zip && python3 -c "import shutil;shutil.make_archive('../dist/LiteWall-v0.2.0-win-x64','zip','.','LiteWall')")
ls -la build/LiteWall dist
