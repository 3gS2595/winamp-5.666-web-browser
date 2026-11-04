<#
.SYNOPSIS
    Draws exodium_open.exe's icon, exodium_open\exodium_open.ico.

.DESCRIPTION
    A dark tile with a steel E on it, and the ironwork from the corner of a browser window
    (exodium\res\corner_ornament_plain.png) across its top and down its right side. Only
    the 256-pixel icon has the ironwork's own picture: shrunk any further it turns into a
    grey smudge, so from 32 to 64 pixels it is a bar and a plate drawn on whole pixels,
    and below 32 there is the tile and the E alone.

    Each size is drawn several times bigger and averaged down, rather than one picture
    being shrunk to every size. -Preview also writes two sheets, every size as it is, on
    dark and light, and the small ones enlarged, to look at before building it in.

.EXAMPLE
    tools\make-icon.ps1 -Preview $env:TEMP\exodium-icon.png
#>
param(
    [string]$Out = (Join-Path $PSScriptRoot "..\exodium_open\exodium_open.ico"),
    [string]$Ornament = (Join-Path $PSScriptRoot "..\exodium\res\corner_ornament_plain.png"),
    [string]$Preview
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

Add-Type -ReferencedAssemblies System.Drawing @'
using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;

public static class ExodiumIcon {
    static Color Hex(int rgb) { return Color.FromArgb(255, (rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255); }

    // The E, in a box 72 wide and 100 high: a stem, three arms, and spurs on the ends of
    // the top and bottom arms. The big one has spikes on the stem's ends as well, like the
    // ones on the ironwork, which go outside the box.
    static readonly PointF[] SmallLetter = {
        new PointF(0, 0), new PointF(70, 0), new PointF(70, 30), new PointF(56, 17), new PointF(24, 17),
        new PointF(24, 42), new PointF(52, 42), new PointF(61, 50), new PointF(52, 58), new PointF(24, 58),
        new PointF(24, 83), new PointF(58, 83), new PointF(72, 70), new PointF(72, 100), new PointF(0, 100),
    };
    static readonly PointF[] BigLetter = {
        new PointF(-9, -9), new PointF(12, 0), new PointF(72, 0), new PointF(72, 36), new PointF(55, 17),
        new PointF(26, 17), new PointF(26, 42), new PointF(50, 42), new PointF(64, 50), new PointF(50, 58),
        new PointF(26, 58), new PointF(26, 83), new PointF(55, 83), new PointF(72, 64), new PointF(72, 100),
        new PointF(12, 100), new PointF(-9, 109), new PointF(0, 88), new PointF(0, 12),
    };

    static GraphicsPath RoundedRect(RectangleF r, float radius) {
        var path = new GraphicsPath();
        float d = radius * 2;
        path.AddArc(r.X, r.Y, d, d, 180, 90);
        path.AddArc(r.Right - d, r.Y, d, d, 270, 90);
        path.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
        path.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
        path.CloseFigure();
        return path;
    }

    // An outline in a box, in pixels of the finished size times k, with its corners on
    // whole pixels if `snap`.
    static PointF[] Fit(PointF[] shape, RectangleF box, float k, bool snap) {
        var points = new PointF[shape.Length];
        for (int i = 0; i < shape.Length; i++) {
            float x = box.X + shape[i].X / 72f * box.Width, y = box.Y + shape[i].Y / 100f * box.Height;
            if (snap) { x = (float)Math.Round(x); y = (float)Math.Round(y); }
            points[i] = new PointF(x * k, y * k);
        }
        return points;
    }

    static void DrawTile(Graphics g, int size, float k, bool small) {
        float inset = small ? 0 : (float)Math.Round(size * 0.06);
        float radius = small ? Math.Max(2, size / 6) : size * 0.16f;
        var outer = new RectangleF(inset * k, inset * k, (size - 2 * inset) * k, (size - 2 * inset) * k);
        float rim = Math.Max(1, (float)Math.Round(size / 32f)) * k;
        using (var path = RoundedRect(outer, radius * k))
        using (var brush = new LinearGradientBrush(outer, Hex(0x9a9aa2), Hex(0x2a2a30), LinearGradientMode.Vertical))
            g.FillPath(brush, path);
        var inner = RectangleF.Inflate(outer, -rim, -rim);
        using (var path = RoundedRect(inner, Math.Max(0.5f, radius * k - rim)))
        using (var brush = new LinearGradientBrush(inner, Hex(0x2c2c33), Hex(0x050507), LinearGradientMode.Vertical))
            g.FillPath(brush, path);
    }

    // Under 30 pixels high the E is the plain one, light and nearly flat, which is what
    // reads there; above that it has its spikes, and is polished steel.
    static void DrawLetter(Graphics g, RectangleF box, float k, bool snap) {
        bool plain = box.Height < 30;
        var bounds = new RectangleF(box.X * k, box.Y * k, box.Width * k, box.Height * k);
        using (var path = new GraphicsPath()) {
            path.AddPolygon(Fit(plain ? SmallLetter : BigLetter, box, k, snap));
            if (plain) {
                using (var brush = new LinearGradientBrush(bounds, Hex(0xf0f0f4), Hex(0xa8a8b0), LinearGradientMode.Vertical))
                    g.FillPath(brush, path);
                return;
            }
            // Polished steel: light above, a dark band across the middle, light again below it.
            using (var brush = new LinearGradientBrush(bounds, Color.White, Color.Black, LinearGradientMode.Vertical)) {
                var blend = new ColorBlend();
                blend.Colors = new[] { Hex(0xfafafc), Hex(0xc4c4cc), Hex(0x6a6a74), Hex(0xb8b8c0), Hex(0xe4e4ea), Hex(0x5c5c66) };
                blend.Positions = new[] { 0f, 0.38f, 0.5f, 0.62f, 0.8f, 1f };
                brush.InterpolationColors = blend;
                g.FillPath(brush, path);
            }
            using (var pen = new Pen(Color.FromArgb(220, 0, 0, 0), Math.Max(1, box.Height / 60f) * k)) {
                pen.LineJoin = LineJoin.Miter;
                g.DrawPath(pen, path);
            }
        }
    }

    // A shape with a dark edge round it, a pixel wide, outside the shape.
    static void FillOutlined(Graphics g, GraphicsPath path, Brush brush, float k) {
        using (var pen = new Pen(Color.FromArgb(230, 0, 0, 0), 2 * k)) {
            pen.LineJoin = LineJoin.Round;
            g.DrawPath(pen, path);
        }
        g.FillPath(brush, path);
    }

    static Brush Steel(RectangleF r, LinearGradientMode mode) {
        var brush = new LinearGradientBrush(RectangleF.Inflate(r, 0.5f, 0.5f), Color.White, Color.Black, mode);
        var blend = new ColorBlend();
        blend.Colors = new[] { Hex(0xf2f2f6), Hex(0xb0b0b8), Hex(0x6c6c76) };
        blend.Positions = new[] { 0f, 0.5f, 1f };
        brush.InterpolationColors = blend;
        return brush;
    }

    // The ironwork as a few shapes, which is what is left of the picture at these sizes: a
    // bar across the top and one down the right with pointed ends, and the plate where
    // they meet. Returns where the bars leave room for the E.
    static RectangleF DrawIronwork(Graphics g, int size, float k, float inset) {
        float t = Math.Max(2, (float)Math.Round(size / 16f));
        float y0 = (float)Math.Round(size * 0.08), y1 = y0 + t;
        float x1 = size - (float)Math.Round(size * 0.08), x0 = x1 - t;
        float left = (float)Math.Round(size * 0.14), bottom = size - (float)Math.Round(size * 0.12);
        // Bars 2 pixels thick have plain ends: a point on them would be its dark edge alone.
        float tip = t > 2 ? (float)Math.Round(t * 1.5f) : 0;
        using (var path = new GraphicsPath()) {
            path.AddPolygon(new[] {
                new PointF((left - tip) * k, (y0 + y1) / 2 * k), new PointF(left * k, y0 * k), new PointF(x1 * k, y0 * k),
                new PointF(x1 * k, bottom * k), new PointF((x0 + x1) / 2 * k, (bottom + tip) * k), new PointF(x0 * k, bottom * k),
                new PointF(x0 * k, y1 * k), new PointF(left * k, y1 * k),
            });
            using (Brush brush = Steel(new RectangleF(0, y0 * k, size * k, (bottom + tip - y0) * k), LinearGradientMode.ForwardDiagonal))
                FillOutlined(g, path, brush, k);
        }
        float pw = (float)Math.Round(size * 0.36), ph = Math.Max(4, (float)Math.Round(size * 0.15));
        var plate = new RectangleF((x1 + 1 - pw) * k, ((y0 + y1) / 2 - ph / 2) * k, pw * k, ph * k);
        plate.Y = Math.Max(k, plate.Y);
        using (var path = RoundedRect(plate, plate.Height / 2))
        using (var brush = new LinearGradientBrush(plate, Color.White, Color.Black, LinearGradientMode.Vertical)) {
            var blend = new ColorBlend();
            blend.Colors = new[] { Hex(0xffffff), Hex(0xa4a4ae), Hex(0xe8e8ee) };
            blend.Positions = new[] { 0f, 0.55f, 1f };
            brush.InterpolationColors = blend;
            FillOutlined(g, path, brush, k);
        }
        return new RectangleF(inset, y1 + 1, x0 - inset, size - inset - y1 - 1);
    }

    // The ironwork's own picture, with its top right corner on the icon's: the spikes there
    // stand out past the tile, as the ornaments do past a browser window's frame.
    static void DrawOrnament(Graphics g, int size, float k, Bitmap ornament) {
        float width = size * 0.97f;
        float height = width * ornament.Height / ornament.Width;
        g.DrawImage(ornament, new RectangleF((size - width) * k, 0, width * k, height * k));
    }

    // A box for the E of this height, in the middle of the room it has, on whole pixels if `snap`.
    static RectangleF LetterBox(RectangleF room, float height, bool snap) {
        float w = height * 0.72f;
        float x = room.X + (room.Width - w) / 2, y = room.Y + (room.Height - height) / 2;
        if (snap) { w = (float)Math.Round(w); x = (float)Math.Round(x); y = (float)Math.Round(y); }
        return new RectangleF(x, y, w, height);
    }

    // The icon at one size. Under 32 pixels it is the tile and the E alone; up to 64 the
    // ironwork is drawn in a few shapes, on whole pixels; above that it is the picture.
    public static Bitmap Draw(int size, Bitmap ornament) {
        bool small = size < 32, picture = size > 64;
        int k = picture ? 4 : 8;
        using (var big = new Bitmap(size * k, size * k, PixelFormat.Format32bppPArgb)) {
            using (var g = Graphics.FromImage(big)) {
                g.SmoothingMode = SmoothingMode.AntiAlias;
                g.InterpolationMode = InterpolationMode.HighQualityBicubic;
                g.PixelOffsetMode = PixelOffsetMode.HighQuality;
                g.CompositingQuality = CompositingQuality.HighQuality;
                g.Clear(Color.Transparent);
                float inset = small ? 0 : (float)Math.Round(size * 0.06);
                DrawTile(g, size, k, small);
                if (small) {
                    // About 5/8 of the tile high, so the stem and arms are 2 or 3 pixels wide.
                    float h = (float)Math.Round(size * 0.62), w = (float)Math.Round(h * 0.75);
                    var box = new RectangleF((float)Math.Floor((size - w) / 2), (float)Math.Floor((size - h) / 2), w, h);
                    DrawLetter(g, box, k, true);
                } else if (picture) {
                    DrawOrnament(g, size, k, ornament);
                    var room = new RectangleF(inset, size * 0.12f, size * 0.84f - inset, size * 0.88f - inset);
                    DrawLetter(g, LetterBox(room, size * 0.54f, false), k, false);
                } else {
                    RectangleF room = DrawIronwork(g, size, k, inset);
                    DrawLetter(g, LetterBox(room, (float)Math.Round(size * 0.5), true), k, true);
                }
            }
            return Shrink(big, k);
        }
    }

    // Averages each k by k block into a pixel.
    static Bitmap Shrink(Bitmap big, int k) {
        int size = big.Width / k;
        var result = new Bitmap(size, size, PixelFormat.Format32bppArgb);
        var from = big.LockBits(new Rectangle(0, 0, big.Width, big.Height), ImageLockMode.ReadOnly, PixelFormat.Format32bppPArgb);
        var to = result.LockBits(new Rectangle(0, 0, size, size), ImageLockMode.WriteOnly, PixelFormat.Format32bppArgb);
        var src = new byte[from.Stride * big.Height];
        Marshal.Copy(from.Scan0, src, 0, src.Length);
        var dst = new byte[to.Stride * size];
        for (int y = 0; y < size; y++) {
            for (int x = 0; x < size; x++) {
                double b = 0, gr = 0, r = 0, a = 0;
                for (int j = 0; j < k; j++) {
                    int row = (y * k + j) * from.Stride;
                    for (int i = 0; i < k; i++) {
                        int at = row + (x * k + i) * 4;
                        b += src[at]; gr += src[at + 1]; r += src[at + 2]; a += src[at + 3];
                    }
                }
                int o = y * to.Stride + x * 4;
                if (a <= 0) continue;
                // Premultiplied in, straight out.
                dst[o] = (byte)Math.Min(255, Math.Round(b * 255 / a));
                dst[o + 1] = (byte)Math.Min(255, Math.Round(gr * 255 / a));
                dst[o + 2] = (byte)Math.Min(255, Math.Round(r * 255 / a));
                dst[o + 3] = (byte)Math.Round(a / (k * k));
            }
        }
        Marshal.Copy(dst, 0, to.Scan0, dst.Length);
        big.UnlockBits(from);
        result.UnlockBits(to);
        return result;
    }

    // An .ico with each picture in it: PNG for 256, which is what Windows expects there, and
    // 32-bit bitmaps below that, which everything reads.
    public static void WriteIco(string path, Bitmap[] images) {
        var parts = new byte[images.Length][];
        for (int n = 0; n < images.Length; n++) {
            Bitmap image = images[n];
            if (image.Width >= 256) {
                using (var png = new MemoryStream()) { image.Save(png, ImageFormat.Png); parts[n] = png.ToArray(); }
                continue;
            }
            int size = image.Width, maskStride = ((size + 31) / 32) * 4;
            using (var dib = new MemoryStream())
            using (var w = new BinaryWriter(dib)) {
                w.Write(40); w.Write(size); w.Write(size * 2); w.Write((short)1); w.Write((short)32);
                w.Write(0); w.Write(size * size * 4 + maskStride * size); w.Write(0); w.Write(0); w.Write(0); w.Write(0);
                var mask = new byte[maskStride * size];
                for (int y = size - 1; y >= 0; y--) { // bottom row first
                    for (int x = 0; x < size; x++) {
                        Color c = image.GetPixel(x, y);
                        w.Write(c.B); w.Write(c.G); w.Write(c.R); w.Write(c.A);
                        if (c.A == 0) mask[(size - 1 - y) * maskStride + x / 8] |= (byte)(0x80 >> (x % 8));
                    }
                }
                w.Write(mask);
                w.Flush();
                parts[n] = dib.ToArray();
            }
        }
        using (var file = new FileStream(path, FileMode.Create))
        using (var w = new BinaryWriter(file)) {
            w.Write((short)0); w.Write((short)1); w.Write((short)images.Length);
            int offset = 6 + 16 * images.Length;
            for (int n = 0; n < images.Length; n++) {
                int size = images[n].Width;
                w.Write((byte)(size >= 256 ? 0 : size)); w.Write((byte)(size >= 256 ? 0 : size));
                w.Write((byte)0); w.Write((byte)0); w.Write((short)1); w.Write((short)32);
                w.Write(parts[n].Length); w.Write(offset);
                offset += parts[n].Length;
            }
            foreach (byte[] part in parts) w.Write(part);
        }
    }
}
'@

$sizes = 256, 64, 48, 40, 32, 24, 20, 16
$art = [System.Drawing.Bitmap]::FromFile((Resolve-Path $Ornament).Path)
try {
    $images = foreach ($size in $sizes) { [ExodiumIcon]::Draw($size, $art) }
} finally { $art.Dispose() }

[ExodiumIcon]::WriteIco([IO.Path]::GetFullPath($Out), $images)
"Wrote $([IO.Path]::GetFullPath($Out)): $($sizes -join ', ') pixels"

if ($Preview) {
    # Two sheets: every size at its own size, on the dark of a dark taskbar and the light
    # of a light one; and the sizes up to 64 enlarged without smoothing, to see their pixels.
    $actual = New-Object System.Drawing.Bitmap 760, 560
    $g = [System.Drawing.Graphics]::FromImage($actual)
    foreach ($half in 0, 1) {
        $back = if ($half) { [System.Drawing.Color]::FromArgb(238, 238, 238) } else { [System.Drawing.Color]::FromArgb(32, 32, 32) }
        $g.FillRectangle((New-Object System.Drawing.SolidBrush $back), 0, $half * 280, 760, 280)
        $x = 12
        foreach ($image in $images) {
            $g.DrawImage($image, $x, $half * 280 + 12, $image.Width, $image.Height)
            $x += $image.Width + 16
        }
    }
    $g.Dispose()
    $actual.Save([IO.Path]::GetFullPath($Preview), [System.Drawing.Imaging.ImageFormat]::Png)
    $actual.Dispose()

    $zoomed = New-Object System.Drawing.Bitmap 1000, 560
    $g = [System.Drawing.Graphics]::FromImage($zoomed)
    $g.InterpolationMode = "NearestNeighbor"
    $g.PixelOffsetMode = "Half"
    $g.Clear([System.Drawing.Color]::FromArgb(32, 32, 32))
    $x = 8; $y = 8; $rowHeight = 0
    foreach ($image in ($images | Where-Object { $_.Width -le 64 })) {
        $zoom = if ($image.Width -le 32) { 8 } else { 4 }
        $w = $image.Width * $zoom
        if ($x + $w -gt 1000) { $x = 8; $y += $rowHeight + 8; $rowHeight = 0 }
        $g.DrawImage($image, $x, $y, $w, $w)
        $x += $w + 8; $rowHeight = [Math]::Max($rowHeight, $w)
    }
    $g.Dispose()
    $zoomedPath = [IO.Path]::ChangeExtension([IO.Path]::GetFullPath($Preview), ".zoomed.png")
    $zoomed.Save($zoomedPath, [System.Drawing.Imaging.ImageFormat]::Png)
    $zoomed.Dispose()
    "Wrote $([IO.Path]::GetFullPath($Preview)) and $zoomedPath"
}
foreach ($image in $images) { $image.Dispose() }
