$entry = Get-Content -Raw "d:\Project\MimiClaw\ESP32\build\compile_commands.json" |
    ConvertFrom-Json |
    Where-Object { $_.file -eq "D:\Project\MimiClaw\ESP32\main\main.c" } |
    Select-Object -First 1

if(-not $entry) {
    throw "compile command for main.c not found"
}

$extraIncludes = @(
    "-I D:/Project/MimiClaw/ESP32/components",
    "-I D:/Project/MimiClaw/ESP32/components/lvgl",
    "-I D:/Project/MimiClaw/ESP32/components/lvgl/src",
    "-I C:/esp/v5.5.3/esp-idf/components/esp_timer/include",
    "-D LV_CONF_INCLUDE_SIMPLE"
) -join " "

$cmd = "$($entry.command) $extraIncludes -fsyntax-only"

Write-Host $cmd
Invoke-Expression $cmd
