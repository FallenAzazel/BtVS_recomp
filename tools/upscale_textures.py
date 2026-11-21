#!/usr/bin/env python3
"""
Local HD Texture Upscaler for BtVS Windows Recomp.

Reads dumped Xbox textures from recomp/dump_textures/ and generates crisp
4x HD replacement PNGs in recomp/hd_textures/ (auto-loaded by d3d9_bridge.cpp
and hot-reloadable in-game via F9).

Backends (auto-selected in priority order):
  1. Real-ESRGAN (realesrgan-ncnn-vulkan.exe on PATH or in tools/) -> True GPU 4x AI upscaling
  2. Pillow (PIL) if installed -> 4x Lanczos + UnsharpMask
  3. Built-in Windows GDI+ (zero dependencies!) -> Alpha-safe HighQualityBicubic 4x + crisp edge bleed

Usage:
  python tools/upscale_textures.py
  python tools/upscale_textures.py --scale 4 --force
  python tools/upscale_textures.py --realesrgan path/to/realesrgan-ncnn-vulkan.exe
"""

import argparse
import ctypes
import os
import shutil
import subprocess
import sys
import time
from ctypes import wintypes

PNG_ENCODER_CLSID = (ctypes.c_ubyte * 16)(
    0x06, 0xF4, 0x7C, 0x55, 0x04, 0x1A, 0xD3, 0x11,
    0x9A, 0x73, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E,
)


class GdiplusStartupInput(ctypes.Structure):
    _fields_ = [
        ("GdiplusVersion", ctypes.c_uint32),
        ("DebugEventCallback", ctypes.c_void_p),
        ("SuppressBackgroundThread", ctypes.c_int32),
        ("SuppressExternalCodecs", ctypes.c_int32),
    ]


class GpRect(ctypes.Structure):
    _fields_ = [
        ("X", ctypes.c_int32),
        ("Y", ctypes.c_int32),
        ("Width", ctypes.c_int32),
        ("Height", ctypes.c_int32),
    ]


class BitmapData(ctypes.Structure):
    _fields_ = [
        ("Width", ctypes.c_uint32),
        ("Height", ctypes.c_uint32),
        ("Stride", ctypes.c_int32),
        ("PixelFormat", ctypes.c_int32),
        ("Scan0", ctypes.c_void_p),
        ("Reserved", ctypes.c_void_p),
    ]


class GdiPlusUpscaler:
    """Zero-dependency Windows GDI+ upscaler with alpha-safe RGB edge bleed."""

    PIXEL_FORMAT_32BPP_ARGB = 0x0026200A
    INTERPOLATION_HQ_BICUBIC = 7
    PIXEL_OFFSET_HALF = 4
    SMOOTHING_HIGH_QUALITY = 2
    WRAP_MODE_TILE_FLIP_XY = 3

    def __init__(self):
        self.gdip = ctypes.windll.gdiplus
        self.token = ctypes.c_void_p()
        si = GdiplusStartupInput(1, None, 0, 0)
        status = self.gdip.GdiplusStartup(ctypes.byref(self.token), ctypes.byref(si), None)
        if status != 0:
            raise RuntimeError(f"GdiplusStartup failed with status {status}")

        # Configure argtypes for 64-bit pointer safety
        self.gdip.GdipLoadImageFromFile.argtypes = [ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_void_p)]
        self.gdip.GdipGetImageWidth.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32)]
        self.gdip.GdipGetImageHeight.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32)]
        self.gdip.GdipCreateBitmapFromScan0.argtypes = [
            ctypes.c_int32, ctypes.c_int32, ctypes.c_int32, ctypes.c_int32,
            ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p),
        ]
        self.gdip.GdipBitmapLockBits.argtypes = [
            ctypes.c_void_p, ctypes.POINTER(GpRect), ctypes.c_uint32,
            ctypes.c_int32, ctypes.POINTER(BitmapData),
        ]
        self.gdip.GdipBitmapUnlockBits.argtypes = [ctypes.c_void_p, ctypes.POINTER(BitmapData)]
        self.gdip.GdipGetImageGraphicsContext.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
        self.gdip.GdipSetInterpolationMode.argtypes = [ctypes.c_void_p, ctypes.c_int32]
        self.gdip.GdipSetPixelOffsetMode.argtypes = [ctypes.c_void_p, ctypes.c_int32]
        self.gdip.GdipSetSmoothingMode.argtypes = [ctypes.c_void_p, ctypes.c_int32]
        self.gdip.GdipCreateImageAttributes.argtypes = [ctypes.POINTER(ctypes.c_void_p)]
        self.gdip.GdipSetImageAttributesWrapMode.argtypes = [ctypes.c_void_p, ctypes.c_int32, ctypes.c_uint32, ctypes.c_int32]
        self.gdip.GdipDrawImageRectRectI.argtypes = [
            ctypes.c_void_p, ctypes.c_void_p,
            ctypes.c_int32, ctypes.c_int32, ctypes.c_int32, ctypes.c_int32,
            ctypes.c_int32, ctypes.c_int32, ctypes.c_int32, ctypes.c_int32,
            ctypes.c_int32, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
        ]
        self.gdip.GdipDisposeImageAttributes.argtypes = [ctypes.c_void_p]
        self.gdip.GdipSaveImageToFile.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_void_p, ctypes.c_void_p]
        self.gdip.GdipDeleteGraphics.argtypes = [ctypes.c_void_p]
        self.gdip.GdipDisposeImage.argtypes = [ctypes.c_void_p]
        self.gdip.GdiplusShutdown.argtypes = [ctypes.c_void_p]

    def close(self):
        if self.token:
            self.gdip.GdiplusShutdown(self.token)
            self.token = None

    @staticmethod
    def _bleed_transparent_rgb(buf: bytearray, w: int, h: int, stride: int) -> bool:
        """Copy RGB from nearest opaque neighbors into alpha==0 pixels to prevent dark UI halos."""
        has_trans = False
        for y in range(h):
            row = y * stride
            for x in range(w):
                if buf[row + x * 4 + 3] == 0:
                    has_trans = True
                    break
            if has_trans:
                break
        if not has_trans:
            return False

        # One fast 4-neighbor dilation pass for edge pixels adjacent to opaque UI art/text
        src = bytes(buf)
        for y in range(h):
            row = y * stride
            for x in range(w):
                off = row + x * 4
                if src[off + 3] != 0:
                    continue
                rb = gb = bb = cnt = 0
                for dy, dx in ((-1, 0), (1, 0), (0, -1), (0, 1)):
                    ny, nx = y + dy, x + dx
                    if 0 <= ny < h and 0 <= nx < w:
                        noff = ny * stride + nx * 4
                        if src[noff + 3] > 16:
                            bb += src[noff]
                            gb += src[noff + 1]
                            rb += src[noff + 2]
                            cnt += 1
                if cnt > 0:
                    buf[off] = bb // cnt
                    buf[off + 1] = gb // cnt
                    buf[off + 2] = rb // cnt
        return True

    def upscale(self, src_path: str, dst_path: str, scale: int) -> tuple[int, int, int, int]:
        img = ctypes.c_void_p()
        if self.gdip.GdipLoadImageFromFile(src_path, ctypes.byref(img)) != 0 or not img:
            raise RuntimeError(f"Failed to load {src_path}")
        try:
            w = ctypes.c_uint32()
            h = ctypes.c_uint32()
            self.gdip.GdipGetImageWidth(img, ctypes.byref(w))
            self.gdip.GdipGetImageHeight(img, ctypes.byref(h))
            sw, sh = int(w.value), int(h.value)
            dw, dh = sw * scale, sh * scale

            # Alpha-safe RGB bleed on source bitmap before scaling
            rect = GpRect(0, 0, sw, sh)
            bd = BitmapData()
            if self.gdip.GdipBitmapLockBits(img, ctypes.byref(rect), 3, self.PIXEL_FORMAT_32BPP_ARGB, ctypes.byref(bd)) == 0:
                try:
                    nbytes = abs(bd.Stride) * sh
                    raw = bytearray((ctypes.c_ubyte * nbytes).from_address(bd.Scan0))
                    if self._bleed_transparent_rgb(raw, sw, sh, abs(bd.Stride)):
                        ctypes.memmove(bd.Scan0, (ctypes.c_ubyte * nbytes).from_buffer(raw), nbytes)
                finally:
                    self.gdip.GdipBitmapUnlockBits(img, ctypes.byref(bd))

            dst_bmp = ctypes.c_void_p()
            if self.gdip.GdipCreateBitmapFromScan0(dw, dh, 0, self.PIXEL_FORMAT_32BPP_ARGB, None, ctypes.byref(dst_bmp)) != 0:
                raise RuntimeError("Failed to create target bitmap")
            try:
                gfx = ctypes.c_void_p()
                self.gdip.GdipGetImageGraphicsContext(dst_bmp, ctypes.byref(gfx))
                try:
                    self.gdip.GdipSetInterpolationMode(gfx, self.INTERPOLATION_HQ_BICUBIC)
                    self.gdip.GdipSetPixelOffsetMode(gfx, self.PIXEL_OFFSET_HALF)
                    self.gdip.GdipSetSmoothingMode(gfx, self.SMOOTHING_HIGH_QUALITY)

                    attr = ctypes.c_void_p()
                    self.gdip.GdipCreateImageAttributes(ctypes.byref(attr))
                    try:
                        # TileFlipXY prevents border fade/black edges when upscaling textures!
                        self.gdip.GdipSetImageAttributesWrapMode(attr, self.WRAP_MODE_TILE_FLIP_XY, 0, 0)
                        self.gdip.GdipDrawImageRectRectI(
                            gfx, img,
                            0, 0, dw, dh,
                            0, 0, sw, sh,
                            2,  # UnitPixel
                            attr, None, None,
                        )
                    finally:
                        if attr:
                            self.gdip.GdipDisposeImageAttributes(attr)
                finally:
                    if gfx:
                        self.gdip.GdipDeleteGraphics(gfx)

                st_save = self.gdip.GdipSaveImageToFile(dst_bmp, dst_path, ctypes.byref(PNG_ENCODER_CLSID), None)
                if st_save != 0:
                    raise RuntimeError(f"Failed to save {dst_path} (GDI+ status={st_save})")
            finally:
                self.gdip.GdipDisposeImage(dst_bmp)
            return sw, sh, dw, dh
        finally:
            self.gdip.GdipDisposeImage(img)


def find_realesrgan(explicit_path: str | None) -> str | None:
    if explicit_path and os.path.isfile(explicit_path):
        return explicit_path
    script_dir = os.path.dirname(os.path.abspath(__file__))
    candidates = [
        os.path.join(script_dir, "realesrgan-ncnn-vulkan.exe"),
        os.path.join(script_dir, "realesrgan", "realesrgan-ncnn-vulkan.exe"),
        shutil.which("realesrgan-ncnn-vulkan"),
        shutil.which("realesrgan-ncnn-vulkan.exe"),
    ]
    for c in candidates:
        if c and os.path.isfile(c):
            return c
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description="Upscale dumped BtVS Xbox textures into recomp/hd_textures/")
    parser.add_argument("--input", default=os.path.join("recomp", "dump_textures"), help="Input dumped textures folder")
    parser.add_argument("--output", default=os.path.join("recomp", "hd_textures"), help="Output HD textures folder")
    parser.add_argument("--scale", type=int, default=4, choices=[2, 4], help="Upscale factor (2 or 4)")
    parser.add_argument("--force", action="store_true", help="Overwrite existing HD textures")
    parser.add_argument("--realesrgan", default=None, help="Optional path to realesrgan-ncnn-vulkan.exe")
    parser.add_argument("--model", default="realesrgan-x4plus", help="Real-ESRGAN model name")
    args = parser.parse_args()

    if not os.path.isdir(args.input):
        print(f"[Upscale] Input folder not found: {args.input}")
        print("[Upscale] Tip: Press F8 in-game (or enable Dump Visible Textures in Config -> Textures) first.")
        return 1

    os.makedirs(args.output, exist_ok=True)
    # Track existing 16-hex-char texture hashes in output so we never duplicate custom HD packs
    existing_hashes = {
        f[:16].lower()
        for f in os.listdir(args.output)
        if len(f) >= 16 and f.lower().endswith((".png", ".jpg", ".jpeg", ".bmp", ".tga"))
    }
    files = sorted(f for f in os.listdir(args.input) if f.lower().endswith(".png"))
    if not files:
        print(f"[Upscale] No PNG files found in {args.input}")
        return 0

    realesrgan_bin = find_realesrgan(args.realesrgan)
    use_pil = False
    if not realesrgan_bin:
        try:
            from PIL import Image, ImageFilter  # noqa: F401
            use_pil = True
        except ImportError:
            use_pil = False

    backend_name = (
        f"Real-ESRGAN ({os.path.basename(realesrgan_bin)}, model={args.model})"
        if realesrgan_bin else
        ("Pillow Lanczos + UnsharpMask" if use_pil else "Windows GDI+ HighQualityBicubic (alpha-safe)")
    )
    print(f"[Upscale] Found {len(files)} dumped texture(s) in '{args.input}'")
    print(f"[Upscale] Backend: {backend_name} | Scale: {args.scale}x | Output: '{args.output}'")

    t0 = time.time()
    done = skipped = failed = 0

    gdip = None
    if not realesrgan_bin and not use_pil:
        gdip = GdiPlusUpscaler()

    try:
        for idx, name in enumerate(files, 1):
            src_path = os.path.abspath(os.path.join(args.input, name))
            dst_path = os.path.abspath(os.path.join(args.output, name))
            tex_hash = name[:16].lower()
            if (os.path.exists(dst_path) or tex_hash in existing_hashes) and not args.force:
                skipped += 1
                continue
            try:
                if realesrgan_bin:
                    res = subprocess.run(
                        [realesrgan_bin, "-i", src_path, "-o", dst_path, "-s", str(args.scale), "-n", args.model, "-f", "png"],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False,
                    )
                    if res.returncode != 0 or not os.path.isfile(dst_path):
                        raise RuntimeError(f"realesrgan exited with code {res.returncode}")
                elif use_pil:
                    from PIL import Image, ImageFilter
                    with Image.open(src_path) as im:
                        im = im.convert("RGBA")
                        nw, nh = im.width * args.scale, im.height * args.scale
                        up = im.resize((nw, nh), Image.Resampling.LANCZOS)
                        r, g, b, a = up.split()
                        rgb = Image.merge("RGB", (r, g, b)).filter(ImageFilter.UnsharpMask(radius=1.2, percent=120, threshold=2))
                        r2, g2, b2 = rgb.split()
                        Image.merge("RGBA", (r2, g2, b2, a)).save(dst_path, "PNG")
                else:
                    gdip.upscale(src_path, dst_path, args.scale)
                done += 1
                if done % 25 == 0 or idx == len(files):
                    print(f"[Upscale] Progress: {idx}/{len(files)} ({done} generated, {skipped} skipped)")
            except Exception as exc:
                failed += 1
                print(f"[Upscale] WARN: {name} failed: {exc}")
    finally:
        if gdip:
            gdip.close()

    elapsed = time.time() - t0
    print(f"[Upscale] Complete in {elapsed:.1f}s -> {done} generated, {skipped} skipped, {failed} failed.")
    print("[Upscale] Press F9 in-game to hot-reload your new HD textures!")
    return 0


if __name__ == "__main__":
    sys.exit(main())
