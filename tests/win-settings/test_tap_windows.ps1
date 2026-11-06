param([Parameter(Mandatory = $true)][string]$Bundle)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../../tools/win/tap-adapter.ps1')
$baseline = @(Get-NetAdapter -IncludeHidden | Select-Object InterfaceGuid, Name, Status)
if (Test-AtariTap ([Guid]::NewGuid())) { throw 'An unknown GUID was accepted as a TAP adapter.' }
Write-Host 'PASS registry scan skips protected non-adapter class subkeys'
$log = Get-AtariSecureLog 'adapter-setup'
Assert-AtariLogSecurity (Split-Path $log -Parent)
Start-Transcript -Path $log -Append | Out-Null
$owned = @()
try {
    foreach ($product in @('Shinogi', 'ARAnyM')) {
        $result = Install-AtariTap $product (Join-Path $Bundle 'network/tap')
        $state = Get-AtariTapState $product
        if (!$state -or !$state.DeviceId -or !$state.DeviceId.StartsWith('ROOT\AtariTAP\')) { throw 'Missing exact owned device identity.' }
        $owned += $state
        if ($result.RebootRequired -or !$result.Adapter) { throw 'A restart is required; kernel-open checks cannot be claimed on this runner.' }
        if ((Install-AtariTap $product (Join-Path $Bundle 'network/tap')).Created) { throw 'Repeat setup created another device.' }
        & (Join-Path $Bundle 'network/probe-tap.exe') $state.Guid
        if ($LASTEXITCODE -ne 0) { throw 'Live TAP driver probe failed.' }
        Write-Host "PASS Windows targeted TAP install and real kernel open/media/close: $product $($state.Guid)"
    }
    if ($owned[0].Guid -eq $owned[1].Guid) { throw 'Both products share one exclusive TAP handle.' }
    foreach ($old in $baseline) {
        $current = @(Get-NetAdapter -IncludeHidden | Where-Object { [Guid]$_.InterfaceGuid -eq [Guid]$old.InterfaceGuid })
        if ($current.Count -ne 1 -or $current[0].Name -ne $old.Name -or $current[0].Status -ne $old.Status) {
            throw 'An existing network adapter was changed.'
        }
    }
    Write-Host 'PASS existing host adapters preserved; physical LAN/Wi-Fi is not exercised by this test'
} finally {
    # These are exact fresh test identities, never a driver/hardware-ID-wide removal.
    foreach ($state in $owned) {
        & pnputil.exe /remove-device $state.DeviceId | Write-Host
    }
    Stop-Transcript | Out-Null
}
