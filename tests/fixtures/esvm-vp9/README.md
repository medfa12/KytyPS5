# Clear VP9 packet fixture

Purpose-generated, unencrypted 64x48 YUV420P grayscale frames, Y = 16, 96, 176;
Cb = Cr = 128. This is test material, not a substitute for any application's media.

Generator: `ffmpeg 8.1.1-full_build-www.gyan.dev`, libvpx-vp9, all-intra lossless:

```text
ffmpeg -hide_banner -f lavfi -i "nullsrc=s=64x48:r=3,geq=lum='16+80*N':cb=128:cr=128" -frames:v 3 -an -c:v libvpx-vp9 -lossless 1 -g 1 -lag-in-frames 0 -auto-alt-ref 0 -threads 1 -pix_fmt yuv420p -color_range tv -colorspace smpte170m -color_primaries smpte170m -color_trc smpte170m -map_metadata -1 -fflags +bitexact -flags:v +bitexact -f ivf tests/fixtures/esvm-vp9/gray.ivf
```

The standalone test's adjacent `packets.h` contains the compressed packets only:
the IVF 32-byte container header and 12-byte per-frame headers are removed during
fixture preparation, never by the production decoder. No demux library is used
by the packet module or its tests. Expected limited-range grayscale RGBA levels
are approximately 0, 93, 186, with alpha 255; tests allow conversion rounding.

Fixture hashes and packet extraction details are recorded in `packets.h`.
No project test/build/emulator execution was performed during fixture generation.
