param(
  [Parameter(Mandatory = $true)][string]$Source,
  [Parameter(Mandatory = $true)][string]$Output
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$sourceBitmap = [System.Drawing.Bitmap]::new($Source)
$bitmap = [System.Drawing.Bitmap]::new(256, 256, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$stream = [System.IO.MemoryStream]::new()
try {
  $graphics.Clear([System.Drawing.Color]::Transparent)
  $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
  $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
  $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
  $graphics.DrawImage($sourceBitmap, 0, 0, 256, 256)
  $bitmap.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
  $png = $stream.ToArray()
  $directory = [System.IO.Path]::GetDirectoryName($Output)
  [System.IO.Directory]::CreateDirectory($directory) | Out-Null
  $file = [System.IO.File]::Create($Output)
  $writer = [System.IO.BinaryWriter]::new($file)
  try {
    $writer.Write([UInt16]0)
    $writer.Write([UInt16]1)
    $writer.Write([UInt16]1)
    $writer.Write([Byte]0)
    $writer.Write([Byte]0)
    $writer.Write([Byte]0)
    $writer.Write([Byte]0)
    $writer.Write([UInt16]1)
    $writer.Write([UInt16]32)
    $writer.Write([UInt32]$png.Length)
    $writer.Write([UInt32]22)
    $writer.Write($png)
    $writer.Flush()
  } finally {
    $writer.Dispose()
  }
} finally {
  $stream.Dispose()
  $graphics.Dispose()
  $bitmap.Dispose()
  $sourceBitmap.Dispose()
}
