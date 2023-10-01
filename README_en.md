Lang: [Русский](README.md) [English](README_en.md)

# VLC-FPV-OSD
Plugin for VLC media player

## How to build
### Windows

Install [MSYS2](https://www.msys2.org)

Download 7z-archive from [videolan.org](https://download.videolan.org/pub/videolan/vlc/) and unpack. `<VLC_DIR>` - unpacked folder.

Edit variables in `<VLC_DIR>\sdk\lib\pkgconfig\vlc-plugin.pc`:

```
prefix=<VLC_DIR>/sdk
pluginsdir=<VLC_DIR>/plugins
```

Open MSYS2 MinGW64 and run:

```bash
export PKG_CONFIG_PATH=<VLC_DIR>/sdk/lib/pkgconfig:$PKG_CONFIG_PATH
make
make install
```

## Install
Copy output file `libfpvosd_plugin.dll` (for Windows) to VLC install subdir `plugins/misc`.

## Reference
* https://github.com/fpv-wtf/msp-osd
* https://habr.com/ru/articles/475992/
* https://wiki.videolan.org/Documentation:Documentation/
* https://wiki.videolan.org/Hacker_Guide/Core/
* https://code.videolan.org/videolan/vlc/-/tree/master/doc
