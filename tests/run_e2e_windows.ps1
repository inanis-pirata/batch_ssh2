# run_e2e_windows.ps1 - End-to-end tests for batchssh2 on Windows
# Usage: powershell -ExecutionPolicy Bypass -File tests\run_e2e_windows.ps1 -Binary <path> -User <user> -Password <password>
# Requires: OpenSSH server (sshd) running on localhost:22

param(
    [Parameter(Mandatory=$true)][string]$Binary,
    [Parameter(Mandatory=$true)][string]$User,
    [Parameter(Mandatory=$true)][string]$Password,
    [int]$Port = 22
)

$ErrorActionPreference = "Stop"
$pass = 0
$fail = 0

function CK($name, $expected, $actual) {
    if ($expected -eq $actual) {
        $script:pass++
        Write-Host "PASS: $name"
    } else {
        $script:fail++
        Write-Host "FAIL: $name (expected [$expected] got [$actual])"
    }
}

function Run-Batch($args) {
    $output = & $Binary @args 2>$null
    return $LASTEXITCODE
}

Write-Host "== 1. basic exec =="
$out = & $Binary -H localhost -u $User -p $Password -c "echo hello-world" 2>$null
$found = ($out | Select-String "hello-world" -Quiet)
CK "exec stdout" $true $found

Write-Host "== 2. script mode with CRLF =="
$crlfScript = "$env:TEMP\bs2_crlf.sh"
"echo CRLF_OK`r`necho second-line`r`n" | Out-File -Encoding ascii $crlfScript
$out = & $Binary -H localhost -u $User -p $Password -c "@$crlfScript" 2>$null
$found = ($out | Select-String "CRLF_OK" -Quiet) -and ($out | Select-String "second-line" -Quiet)
CK "crlf script" $true $found
Remove-Item $crlfScript -Force

Write-Host "== 3. json output =="
$out = & $Binary -H localhost -u $User -p $Password -c "true" --json 2>$null
try {
    $json = $out | ConvertFrom-Json
    CK "json status" "success" $json[0].status
    CK "json exit_code" 0 $json[0].exit_code
} catch {
    CK "json parse" "valid" "invalid: $_"
}

Write-Host "== 4. csv output =="
$out = & $Binary -H localhost -u $User -p $Password -c "echo a,b" --csv 2>$null
$csvFound = ($out | Select-String "Host,Status,ExitCode" -Quiet)
CK "csv header" $true $csvFound

Write-Host "== 5. upload file =="
$uploadFile = "$env:TEMP\bs2_up.txt"
"uploaded-content" | Out-File -Encoding ascii $uploadFile
$rc = & $Binary -H localhost -u $User -p $Password --upload "${uploadFile}:C:/Windows/Temp/bs2_up.txt" 2>$null
CK "upload rc" 0 $rc
Start-Sleep -Seconds 1
$verify = & $Binary -H localhost -u $User -p $Password -c "type C:\Windows\Temp\bs2_up.txt" 2>$null
$uploadFound = ($verify | Select-String "uploaded-content" -Quiet)
CK "uploaded content" $true $uploadFound

Write-Host "== 6. download with per-host subdirectory =="
$dlDir = "$env:TEMP\bs2_dl"
Remove-Item $dlDir -Recurse -Force -ErrorAction SilentlyContinue
$rc = & $Binary -H localhost -u $User -p $Password --download "C:/Windows/Temp/bs2_up.txt:${dlDir}/" 2>$null
CK "download rc" 0 $rc
$dirs = Get-ChildItem $dlDir -Directory
CK "per-host dirs" 1 $dirs.Count

Write-Host "== 7. timeout detection =="
$rc = & $Binary -H localhost -u $User -p $Password -T 2 -c "ping -n 30 127.0.0.1" 2>$null
CK "timeout exit code" 2 $rc

Write-Host "== 8. wrong password =="
$rc = & $Binary -H localhost -u $User -p "definitely-wrong" -c "true" 2>$null
CK "auth failure exit code" 2 $rc

Write-Host "== 9. mixed success/failure =="
$rc = & $Binary -H localhost,no-such-host.invalid -u $User -p $Password -T 5 -c "true" 2>$null
CK "partial failure exit code" 1 $rc

Write-Host "== 10. password from environment =="
$env:BATCHSSH2_PWD = $Password
$out = & $Binary -H localhost -u $User -c "echo env-ok" 2>$null
Remove-Item Env:BATCHSSH2_PWD
$envFound = ($out | Select-String "env-ok" -Quiet)
CK "env password" $true $envFound

Write-Host ""
Write-Host "RESULT: $pass passed, $fail failed"
if ($fail -eq 0) { exit 0 } else { exit 1 }
