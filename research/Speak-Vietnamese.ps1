param(
    [Parameter(Mandatory=$true)][string]$Cache,
    [Parameter(Mandatory=$true)][string]$Text,
    [Parameter(Mandatory=$true)][string]$Output,
    [string]$Ffmpeg = '',
    [int]$Speaker = 0
)
$ErrorActionPreference = 'Stop'
if (Test-Path -LiteralPath $Output) { throw 'Output already exists. Choose a new MP3 name.' }
$piperPath = Join-Path $Cache 'piper/piper.exe'
$model = Join-Path $Cache 'vi_VN-vivos-x_low.onnx'
if (!(Test-Path -LiteralPath $piperPath) -or !(Test-Path -LiteralPath $model)) { throw 'Install the Vietnamese voice first.' }
if (!$Ffmpeg) {
    $command = Get-Command ffmpeg.exe -ErrorAction SilentlyContinue
    if ($command) { $Ffmpeg = $command.Source }
}
if (!(Test-Path -LiteralPath $Ffmpeg)) { throw 'FFmpeg was not found. Browse to ffmpeg.exe in the application.' }
$content = [IO.File]::ReadAllText($Text, [Text.Encoding]::UTF8)
if (!$content.Trim()) { throw 'The narration text is empty.' }
if ($content.Length -gt 200000) { throw 'Split the narration into chapters shorter than 200,000 characters.' }
if ($content -match '\$|\\\(|\\\[|\\frac|\\begin|\\end') { throw 'Remove mathematical notation from the narration before exporting.' }
if ($Speaker -lt 0 -or $Speaker -ge 65) { throw 'Speaker must be between 0 and 64.' }

function Quote-CrtArgument([string]$Value) {
    '"' + [regex]::Replace([regex]::Replace($Value, '(\\*)"', '$1$1\"'), '(\\+)$', '$1$1') + '"'
}
$renderId = [Guid]::NewGuid().ToString('N')
$wave = "$Output.$renderId.rendering.wav"
$partial = "$Output.$renderId.rendering.mp3"
try {
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = $piperPath
    $info.Arguments = '--model ' + (Quote-CrtArgument $model) + ' --espeak_data ' + (Quote-CrtArgument (Join-Path $Cache 'piper/espeak-ng-data')) + ' --speaker ' + $Speaker + ' --output_file ' + (Quote-CrtArgument $wave)
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardInput = $true
    $process = [Diagnostics.Process]::Start($info)
    try {
        # Windows PowerShell 5.1 lacks ProcessStartInfo.StandardInputEncoding.
        $speechText = ($content -replace '[\r\n]+', ' ') + "`n"
        $utf8Input = [Text.Encoding]::UTF8.GetBytes($speechText)
        $process.StandardInput.BaseStream.Write($utf8Input, 0, $utf8Input.Length)
        $process.StandardInput.BaseStream.Flush()
        $process.StandardInput.Close()
        $process.WaitForExit()
        if ($process.ExitCode -ne 0) { throw 'Vietnamese speech generation failed.' }
    } finally { $process.Dispose() }
    & $Ffmpeg '-hide_banner' '-loglevel' 'error' '-nostdin' '-n' '-i' $wave '-codec:a' 'libmp3lame' '-b:a' '64k' $partial
    if ($LASTEXITCODE -ne 0 -or !(Test-Path -LiteralPath $partial)) { throw 'MP3 encoding failed.' }
    Move-Item -LiteralPath $partial -Destination $Output
    Write-Output "SAVED $Output"
} finally {
    if (Test-Path -LiteralPath $wave) { Remove-Item -LiteralPath $wave }
    if (Test-Path -LiteralPath $partial) { Remove-Item -LiteralPath $partial }
}
