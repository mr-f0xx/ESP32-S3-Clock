$ErrorActionPreference = "Stop"
$previewRoot = [IO.Path]::GetFullPath((Split-Path -Parent $MyInvocation.MyCommand.Path))
$listener = [Net.HttpListener]::new()
$listener.Prefixes.Add("http://127.0.0.1:8765/")
$listener.Start()
Write-Host "Weather UI preview: http://127.0.0.1:8765/"
try {
    while ($listener.IsListening) {
        $context = $listener.GetContext()
        $relative = [Uri]::UnescapeDataString($context.Request.Url.AbsolutePath.TrimStart('/'))
        if ([string]::IsNullOrWhiteSpace($relative)) { $relative = "index.html" }
        $target = [IO.Path]::GetFullPath((Join-Path $previewRoot $relative))
        if (-not $target.StartsWith($previewRoot, [StringComparison]::OrdinalIgnoreCase) -or
            -not (Test-Path -LiteralPath $target -PathType Leaf)) {
            $context.Response.StatusCode = 404
            $context.Response.Close()
            continue
        }
        $extension = [IO.Path]::GetExtension($target).ToLowerInvariant()
        $context.Response.ContentType = switch ($extension) {
            ".html" { "text/html; charset=utf-8" }
            ".ttf"  { "font/ttf" }
            ".css"  { "text/css; charset=utf-8" }
            ".js"   { "text/javascript; charset=utf-8" }
            default { "application/octet-stream" }
        }
        $bytes = [IO.File]::ReadAllBytes($target)
        $context.Response.ContentLength64 = $bytes.Length
        $context.Response.OutputStream.Write($bytes, 0, $bytes.Length)
        $context.Response.Close()
    }
} finally {
    $listener.Stop()
    $listener.Close()
}
