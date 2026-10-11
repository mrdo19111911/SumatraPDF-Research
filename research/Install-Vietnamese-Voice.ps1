param([Parameter(Mandatory=$true)][string]$Cache)
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
New-Item -ItemType Directory -Path $Cache -Force | Out-Null
$piperPath = Join-Path $Cache 'piper/piper.exe'
if (!(Test-Path -LiteralPath $piperPath)) {
    $archive = Join-Path $Cache 'piper-windows.zip'
    Invoke-WebRequest -UseBasicParsing -Uri 'https://github.com/rhasspy/piper/releases/download/2023.11.14-2/piper_windows_amd64.zip' -OutFile $archive
    Expand-Archive -LiteralPath $archive -DestinationPath $Cache -Force
    Remove-Item -LiteralPath $archive
}
foreach ($name in @('vi_VN-vivos-x_low.onnx', 'vi_VN-vivos-x_low.onnx.json', 'MODEL_CARD')) {
    $target = Join-Path $Cache $name
    if (!(Test-Path -LiteralPath $target)) {
        $temporary = "$target.download"
        Invoke-WebRequest -UseBasicParsing -Uri "https://huggingface.co/rhasspy/piper-voices/resolve/main/vi/vi_VN/vivos/x_low/$name" -OutFile $temporary
        Move-Item -LiteralPath $temporary -Destination $target -Force
    }
}
if (!(Test-Path -LiteralPath $piperPath)) { throw 'Piper did not install.' }
Write-Output "READY $Cache"
