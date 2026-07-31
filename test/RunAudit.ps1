# RunAudit.ps1 - High-risk API audit script (P2 deliverable)
# Exit code: 0 = hard rules pass; non-zero = violation (CI gateable)

$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $PSScriptRoot
$Dirs = @(
    (Join-Path $Root "game"),
    (Join-Path $Root "flow"),
    (Join-Path $Root "ui"),
    (Join-Path $Root "config"),
    (Join-Path $Root "input")
)
$Files = @(
    (Join-Path $Root "TabletReforgeAutomation.cpp"),
    (Get-ChildItem -Recurse -Path $Dirs -Include *.h,*.cpp -ErrorAction SilentlyContinue).FullName
) | Where-Object { $_ -and (Test-Path $_) } | Select-Object -Unique

$fail = 0

function Write-Hdr($t)  { Write-Host ("`n=== " + $t + " ===") -ForegroundColor Cyan }
function Write-Pass($t) { Write-Host ("[PASS] " + $t) -ForegroundColor Green }
function Write-Fail($t) { Write-Host ("[FAIL] " + $t) -ForegroundColor Red; $script:fail++ }
function Write-Note($t) { Write-Host ("[NOTE] " + $t) -ForegroundColor Yellow }

# Rule 1: ReadItem (no Mods suffix) and GetFullItem -> MUST BE 0 in business code
Write-Hdr "Rule 1: ReadItem / GetFullItem zero-hit"
$hits1 = @()
foreach ($f in $Files) {
    $lines = Get-Content -LiteralPath $f -ErrorAction SilentlyContinue
    if (-not $lines) { continue }
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match '\bReadItem\b' -or $lines[$i] -match '\bGetFullItem\b') {
            $hits1 += ("  " + $f + ":" + ($i+1) + ": " + $lines[$i].Trim())
        }
    }
}
if ($hits1.Count -eq 0) { Write-Pass "Rule 1: 0 hits (ReadItem/GetFullItem)" }
else { Write-Fail ($hits1.Count.ToString() + " ReadItem/GetFullItem hit(s) - FORBIDDEN per CONSTITUTION"); $hits1 | ForEach-Object { Write-Host $_ } }

# Rule 2: Affix-text fields (soft rule, list for manual review)
Write-Hdr "Rule 2: Affix-text field references"
$hits2 = @()
$affixFields = @('ImplicitMods','ExplicitMods','EnchantMods','HellscapeMods','CrucibleMods')
foreach ($f in $Files) {
    $lines = Get-Content -LiteralPath $f -ErrorAction SilentlyContinue
    if (-not $lines) { continue }
    for ($i = 0; $i -lt $lines.Count; $i++) {
        foreach ($k in $affixFields) {
            if ($lines[$i] -match ("\b" + $k + "\b")) {
                $hits2 += ("  " + $f + ":" + ($i+1) + ": " + $lines[$i].Trim())
                break
            }
        }
    }
}
Write-Note ($hits2.Count.ToString() + " affix-text ref(s) - verify each inside needMods=true only:")
$hits2 | ForEach-Object { Write-Host $_ }

# Rule 3: ReadItemMods call sites (manual review)
Write-Hdr "Rule 3: ReadItemMods call sites"
$hits3 = @()
foreach ($f in $Files) {
    $lines = Get-Content -LiteralPath $f -ErrorAction SilentlyContinue
    if (-not $lines) { continue }
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match '\bReadItemMods\b') {
            $hits3 += ("  " + $f + ":" + ($i+1) + ": " + $lines[$i].Trim())
        }
    }
}
Write-Note ($hits3.Count.ToString() + " ReadItemMods call(s). ALLOWED summary fields: Rarity/IsIdentified/ItemLevel/IsCorrupted/IsMirrored/IsSplit/IsRelic/IsSynthesised/CraftedModCount/Valid")
$hits3 | ForEach-Object { Write-Host $_ }

# Rule 4: settings/calib json refs - writes must go AtomicWriteText
Write-Hdr "Rule 4: settings.json / calib.json references"
$hits4 = @()
foreach ($f in $Files) {
    $lines = Get-Content -LiteralPath $f -ErrorAction SilentlyContinue
    if (-not $lines) { continue }
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match 'settings\.json' -or $lines[$i] -match 'calib\.json') {
            $hits4 += ("  " + $f + ":" + ($i+1) + ": " + $lines[$i].Trim())
        }
    }
}
Write-Note ($hits4.Count.ToString() + " ref(s) - writes MUST use AtomicWriteText, direct ofstream(target) forbidden:")
$hits4 | ForEach-Object { Write-Host $_ }

# Rule 5: DumpTimeoutSnapshot ordering (ASCII-only regex to avoid encoding issues)
Write-Hdr "Rule 5: DumpTimeoutSnapshot ordering vs state-timeout Abort"
$smFile = Join-Path $Root "flow\StateMachine.h"
if (Test-Path $smFile) {
    $lines = Get-Content -LiteralPath $smFile
    $vicinityOk = $false
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match 'DumpTimeoutSnapshot') {
            $endJ = [Math]::Min($lines.Count, $i + 5)
            for ($j = $i; $j -lt $endJ; $j++) {
                if ($lines[$j] -match 'Abort') {
                    $vicinityOk = $true
                    break
                }
            }
        }
    }
    if ($vicinityOk) { Write-Pass "Rule 5: DumpTimeoutSnapshot precedes nearby Abort" }
    else { Write-Note "Rule 5: Manual check recommended - inspect StateMachine.h near line 180" }
}

Write-Hdr "SUMMARY"
if ($fail -eq 0) {
    Write-Pass "All HARD rules passed. Soft rules 2/3/4 need manual sign-off."
    exit 0
} else {
    Write-Fail ($fail.ToString() + " HARD rule violation(s).")
    exit $fail
}
