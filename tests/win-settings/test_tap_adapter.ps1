$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
foreach ($name in @('tap-adapter.ps1', 'tap-setup.ps1', 'collect-wifi.ps1', 'lan-setup.ps1')) {
    $tokens = $null; $errors = $null
    [void][System.Management.Automation.Language.Parser]::ParseFile(
        (Join-Path $root "tools/win/$name"), [ref]$tokens, [ref]$errors)
    if ($errors.Count) { throw ($errors | Out-String) }
}
. (Join-Path $root 'tools/win/tap-adapter.ps1')

# Run the real ownership/selection state machine with a simulated Windows
# adapter inventory. No driver, registry or physical networking is changed.
function Reset-Fixture {
    $script:saved = @{}
    $script:adapters = @([PSCustomObject]@{ InterfaceGuid = [Guid]::NewGuid(); PnPDeviceID = 'OtherVPN'; Name = 'Other VPN'; Tap = $true })
    $script:installerCalls = 0
    $script:installCode = 0
    $script:newCount = 1
    $script:extraForeign = $false
    $script:renameFails = $false
    $script:boot = 1
    $script:bootFails = $false
    $script:compatible = $true
    $script:recoverId = ''
    $script:recoveryNodes = @()
    $script:pnpFails = $false
}
function Get-AtariTapState($Product) { return $script:saved[$Product] }
function Save-AtariTapState($Product, $Guid, $DeviceId, $RestartBoot, $RecoveryRequired) {
    $script:saved[$Product] = [PSCustomObject]@{ Guid = $Guid; DeviceId = $DeviceId; RestartBoot = $RestartBoot; RecoveryRequired = [bool]$RecoveryRequired }
}
function Clear-AtariTapRecovery($Product) { $script:saved.Remove($Product) }
function Get-PnpDevice { if ($script:pnpFails) { throw 'PnP query failed' }; return $script:recoveryNodes }
function Get-AtariBootId { if ($script:bootFails) { throw 'CIM unavailable' }; return [string]$script:boot }
function Assert-AtariTapCompatible { if (!$script:compatible) { throw 'Different existing driver version' } }
function Get-NetAdapter([string]$Name) {
    if ($Name) { return @($script:adapters | Where-Object { $_.Name -eq $Name }) }
    return $script:adapters
}
function Test-AtariTap($Guid) { return @($script:adapters | Where-Object { $_.InterfaceGuid -eq $Guid -and $_.Tap }).Count -eq 1 }
function Start-Sleep { }
function Invoke-AtariTapInstaller($DriverFolder) {
    $script:installerCalls++
    $id = 'ROOT\AtariTAP\' + [Guid]::NewGuid().ToString()
    $guid = [Guid]::NewGuid()
    if ($script:installCode -in @(0, 1)) {
        for ($i = 0; $i -lt $script:newCount; $i++) {
            $script:adapters += [PSCustomObject]@{ InterfaceGuid = $guid; PnPDeviceID = $id; Name = "New $i"; Tap = $true }
        }
        if ($script:extraForeign) {
            $script:adapters += [PSCustomObject]@{ InterfaceGuid = [Guid]::NewGuid(); PnPDeviceID = 'Concurrent unrelated VPN'; Name = 'Foreign'; Tap = $true }
        }
    } else { $id = $script:recoverId }
    return [PSCustomObject]@{ ExitCode = $script:installCode; deviceInstanceId = $id;
        guid = $(if ($script:newCount -and $script:installCode -in @(0, 1)) { '{' + $guid.ToString() + '}' } else { $null });
        rebootRequired = ($script:installCode -eq 1); Diagnostic = 'fixture'; progress = $false }
}
function Rename-NetAdapter {
    param([Parameter(ValueFromPipeline)]$InputObject, [string]$NewName)
    process { if ($script:renameFails) { throw 'simulated rename error' }; $InputObject.Name = $NewName }
}
function Expect-Failure([scriptblock]$Action) {
    $failed = $false
    try { & $Action | Out-Null } catch { $failed = $true }
    if (!$failed) { throw 'An unsafe operation unexpectedly succeeded.' }
}

Reset-Fixture
$unrelated = $script:adapters[0].InterfaceGuid
$script:extraForeign = $true
$first = Ensure-AtariTap Shinogi ignored
$second = Ensure-AtariTap ARAnyM ignored
if ($first.Adapter.InterfaceGuid -eq $second.Adapter.InterfaceGuid) { throw 'Both products selected the same adapter.' }
if ($first.Adapter.InterfaceGuid -eq $unrelated -or $second.Adapter.InterfaceGuid -eq $unrelated) { throw 'An unrelated TAP was adopted.' }
if ($first.Adapter.Name -ne 'Shinogi LAN' -or $second.Adapter.Name -ne 'ARAnyM LAN') { throw 'Wrong product adapter names.' }
$again = Ensure-AtariTap Shinogi ignored
if ($script:installerCalls -ne 2 -or $again.Created) { throw 'Repeated setup created another adapter.' }
Write-Host 'PASS exact new-device selection, concurrent unrelated VPN preserved, separate products, idempotent retry'

Reset-Fixture
$script:saved.Shinogi = [PSCustomObject]@{ Guid = [Guid]::NewGuid().ToString(); DeviceId = ''; RestartBoot = '' }
Expect-Failure { Ensure-AtariTap Shinogi ignored }
if ($script:installerCalls) { throw 'A stale owned adapter was silently replaced.' }
$script:saved.Shinogi.Guid = $script:adapters[0].InterfaceGuid.ToString()
$script:adapters[0].Tap = $false
Expect-Failure { Ensure-AtariTap Shinogi ignored }
Write-Host 'PASS stale/non-TAP ownership fails without installing or adopting an adapter'

Reset-Fixture
$script:newCount = 2
Expect-Failure { Ensure-AtariTap Shinogi ignored }
if (!$script:saved.Shinogi.DeviceId) { throw 'Ambiguous discovery lost the exact new device recovery identity.' }
Reset-Fixture
$script:installCode = 2
Expect-Failure { Ensure-AtariTap Shinogi ignored }
if ($script:saved.Count) { throw 'Clean rollback saved a nonexistent owned device.' }
$script:recoverId = 'ROOT\AtariTAP\recovery'
Expect-Failure { Ensure-AtariTap Shinogi ignored }
if ($script:saved.Shinogi.DeviceId -ne $script:recoverId) { throw 'Failed rollback lost recovery identity.' }
$calls = $script:installerCalls
$script:recoveryNodes = @([PSCustomObject]@{ InstanceId = $script:recoverId })
Expect-Failure { Ensure-AtariTap Shinogi ignored }
if ($script:installerCalls -ne $calls) { throw 'Recovery created another device while the old node still existed.' }
$script:recoveryNodes = @()
$script:pnpFails = $true
Expect-Failure { Ensure-AtariTap Shinogi ignored }
if ($script:installerCalls -ne $calls) { throw 'A failed PnP query was mistaken for completed removal.' }
$script:pnpFails = $false
$script:installCode = 0
[void](Ensure-AtariTap Shinogi ignored)
if ($script:installerCalls -ne $calls + 1) { throw 'Verified removal did not permit a clean retry.' }
Reset-Fixture
$script:installCode = 1
$script:newCount = 0
if (!(Ensure-AtariTap Shinogi ignored).RebootRequired) { throw 'Reboot request was lost.' }
if (!(Ensure-AtariTap Shinogi ignored).RebootRequired -or $script:installerCalls -ne 1) { throw 'Retry bypassed the pending restart.' }
$script:boot++
$script:adapters += [PSCustomObject]@{ InterfaceGuid = [Guid]::NewGuid(); PnPDeviceID = $script:saved.Shinogi.DeviceId; Name = 'After restart'; Tap = $true }
if ((Ensure-AtariTap Shinogi ignored).RebootRequired -or $script:installerCalls -ne 1) { throw 'A completed restart was ignored or a second node installed.' }
Write-Host 'PASS ambiguous discovery, clean/failed rollback, pending restart and post-restart recovery'

Reset-Fixture
$script:bootFails = $true
Expect-Failure { Ensure-AtariTap Shinogi ignored }
if ($script:installerCalls -or $script:saved.Count) { throw 'Missing boot identity allowed mutation.' }
Reset-Fixture
$script:compatible = $false
Expect-Failure { Ensure-AtariTap Shinogi ignored }
if ($script:installerCalls -or $script:saved.Count) { throw 'A different existing TAP driver was updated.' }
Reset-Fixture
$script:renameFails = $true
Expect-Failure { Ensure-AtariTap ARAnyM ignored }
if (!$script:saved.ARAnyM.DeviceId) { throw 'Rename failure orphaned the new adapter.' }
$script:renameFails = $false
if ((Ensure-AtariTap ARAnyM ignored).Created -or $script:installerCalls -ne 1) { throw 'Rename retry duplicated the adapter.' }
Write-Host 'PASS pre-install boot/driver checks and ownership preservation after rename failure'

# Test the bridge ownership classifier and membership guard from the real
# setup script, without evaluating any of its mutating top-level code.
$tokens = $null; $errors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $root 'tools/win/lan-setup.ps1'), [ref]$tokens, [ref]$errors)
foreach ($name in @('Select-CreatedBridge', 'Assert-ManagedBridgeMembers')) {
    $function = $ast.Find({ param($n)
        $n -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq $name
    }, $true)
    if (!$function) { throw "Missing bridge guard: $name" }
    Invoke-Expression $function.Extent.Text
}
$existing = [Guid]::NewGuid(); $new = [Guid]::NewGuid(); $other = [Guid]::NewGuid()
Expect-Failure { Select-CreatedBridge 1 @() @($existing) }
Expect-Failure { Select-CreatedBridge 0 @($existing) @($existing) }
Expect-Failure { Select-CreatedBridge 0 @() @($new, $other) }
if ((Select-CreatedBridge 0 @($existing) @($existing, $new)) -ne $new) { throw 'Wrong bridge was claimed.' }
$script:adapters = @([PSCustomObject]@{ Name = 'Host'; InterfaceGuid = $existing },
                     [PSCustomObject]@{ Name = 'Tap'; InterfaceGuid = $new },
                     [PSCustomObject]@{ Name = 'Foreign'; InterfaceGuid = $other })
$script:bindings = @([PSCustomObject]@{ Name = 'Host'; Enabled = $true }, [PSCustomObject]@{ Name = 'Tap'; Enabled = $true })
function Get-NetAdapterBinding { return $script:bindings }
Assert-ManagedBridgeMembers $existing $new
$script:bindings += [PSCustomObject]@{ Name = 'Foreign'; Enabled = $true }
Expect-Failure { Assert-ManagedBridgeMembers $existing $new }
Write-Host 'PASS failed/existing/ambiguous bridge creation never claims another bridge; added members prevent removal'

# Exercise the actual elevated log-path checks with synthetic ACLs.
$script:logAttributes = [IO.FileAttributes]::Directory
$script:logOwner = 'S-1-5-32-544'
$script:logRules = @([PSCustomObject]@{ AccessControlType = 'Allow'; IdentityReference = [PSCustomObject]@{ Value = 'S-1-5-32-545' }; FileSystemRights = [Security.AccessControl.FileSystemRights]::ReadAndExecute })
$script:logAcl = New-Object PSObject
$script:logAcl | Add-Member ScriptMethod GetOwner { param($Type) return [PSCustomObject]@{ Value = $script:logOwner } }
$script:logAcl | Add-Member ScriptMethod GetAccessRules { param($Explicit, $Inherited, $Type) return $script:logRules }
function Get-Item { return [PSCustomObject]@{ Attributes = $script:logAttributes } }
function Get-Acl { return $script:logAcl }
Assert-AtariLogSecurity ignored
$script:logOwner = 'S-1-5-21-1000'
Expect-Failure { Assert-AtariLogSecurity ignored }
$script:logOwner = 'S-1-5-18'
$script:logRules[0].FileSystemRights = [Security.AccessControl.FileSystemRights]::Write
Expect-Failure { Assert-AtariLogSecurity ignored }
$script:logRules = @()
$script:logAttributes = [IO.FileAttributes]::ReparsePoint
Expect-Failure { Assert-AtariLogSecurity ignored }
Write-Host 'PASS privileged log rejects user-owned, user-writable and junction/reparse paths'

# Keep the diagnostic collector read-only. In particular, netsh is invoked
# only with the fixed read/help commands, never user-controlled arguments.
$tokens = $null; $errors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $root 'tools/win/collect-wifi.ps1'), [ref]$tokens, [ref]$errors)
$forbidden = @('Set-NetIPInterface', 'Set-NetIPAddress', 'New-NetIPAddress', 'Remove-NetIPAddress',
               'Set-DnsClientServerAddress', 'Rename-NetAdapter', 'Set-NetAdapterBinding', 'Start-Process',
               'Set-ItemProperty', 'New-ItemProperty', 'Register-ScheduledTask')
foreach ($node in $ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.CommandAst] }, $true)) {
    if ($node.GetCommandName() -in $forbidden) { throw 'The read-only collector mutates host networking.' }
}
Write-Host 'PASS diagnostic collector has no networking/driver/registry mutation commands'
