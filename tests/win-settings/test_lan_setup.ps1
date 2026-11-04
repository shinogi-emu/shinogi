param([string]$Source = (Join-Path $PSScriptRoot '../../tools/win/lan-setup.ps1'))
$ErrorActionPreference = 'Stop'
$tokens = $null
$parseErrors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile(
    (Resolve-Path $Source), [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) {
    $parseErrors | ForEach-Object { Write-Host $_.ToString() }
    exit 1
}
# Evaluate only the pure adapter-ID parser. No setup, elevation, external
# processes, registry writes or host network changes are run by this test.
$function = $ast.Find({ param($node)
    $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Get-BridgeId'
}, $true)
if (!$function) { throw 'The bridge-ID parser was not found.' }
Invoke-Expression $function.Extent.Text
$adapter = [PSCustomObject]@{ Name = 'Ethernet 2'; InterfaceDescription = 'USB Ethernet Adapter' }
$table = @'
 ID  Name                          Compatibility
  7  Ethernet 2                    disabled
 21  Shinogi LAN                   disabled
'@
if ((Get-BridgeId $table $adapter) -ne 7) { throw 'Wrong bridge adapter ID.' }
$adapter.Name = 'Shinogi LAN'
if ((Get-BridgeId $table $adapter) -ne 21) { throw 'Wrong managed TAP ID.' }
$adapter.Name = 'Ethernet'
$failed = $false
try { Get-BridgeId $table $adapter | Out-Null } catch { $failed = $true }
if (!$failed) { throw 'Partial adapter-name match was accepted.' }
$failed = $false
try { Get-BridgeId " 7  Ethernet  2                    disabled" $adapter | Out-Null } catch { $failed = $true }
if (!$failed) { throw 'Adapter-name whitespace was mistaken for a column boundary.' }
$adapter.Name = 'Shinogi LAN'
$failed = $false
try { Get-BridgeId ($table + "`n 22  Shinogi LAN  disabled") $adapter | Out-Null } catch { $failed = $true }
if (!$failed) { throw 'Ambiguous adapter-name match was accepted.' }
Write-Host 'LAN setup: PowerShell syntax and unambiguous adapter-ID matching passed; host networking unchanged'
