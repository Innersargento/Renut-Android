param(
    [string]$Source = (Join-Path (Split-Path $PSScriptRoot -Parent) 'android/app/src/main/res/drawable-nodpi/ic_renut.png'),
    [string]$Destination = (Join-Path (Split-Path $PSScriptRoot -Parent) 'icon/app.ico')
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$sourceImage = [System.Drawing.Image]::FromFile($Source)
$stream = [System.IO.File]::Create($Destination)
$writer = [System.IO.BinaryWriter]::new($stream)
try {
    $sizes = @(16, 24, 32, 48, 64, 128, 256)
    $images = [System.Collections.Generic.List[byte[]]]::new()
    foreach ($size in $sizes) {
        $bitmap = [System.Drawing.Bitmap]::new($size, $size)
        try {
            $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
            try {
                $graphics.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
                $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
                $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
                $graphics.DrawImage($sourceImage, 0, 0, $size, $size)
            } finally {
                $graphics.Dispose()
            }
            $png = [System.IO.MemoryStream]::new()
            try {
                $bitmap.Save($png, [System.Drawing.Imaging.ImageFormat]::Png)
                $images.Add($png.ToArray())
            } finally {
                $png.Dispose()
            }
        } finally {
            $bitmap.Dispose()
        }
    }

    $writer.Write([uint16]0)
    $writer.Write([uint16]1)
    $writer.Write([uint16]$sizes.Count)
    $offset = 6 + 16 * $sizes.Count
    for ($index = 0; $index -lt $sizes.Count; $index++) {
        $size = $sizes[$index]
        $writer.Write([byte]($size % 256))
        $writer.Write([byte]($size % 256))
        $writer.Write([byte]0)
        $writer.Write([byte]0)
        $writer.Write([uint16]1)
        $writer.Write([uint16]32)
        $writer.Write([uint32]$images[$index].Length)
        $writer.Write([uint32]$offset)
        $offset += $images[$index].Length
    }
    foreach ($pngBytes in $images) {
        $writer.Write([byte[]]$pngBytes)
    }
} finally {
    $writer.Dispose()
    $sourceImage.Dispose()
}
