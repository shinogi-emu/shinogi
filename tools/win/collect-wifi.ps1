# Read-only Windows/Wi-Fi bridge diagnostics. No drivers or networking are changed.
[CmdletBinding()]
param([string]$OutputFile)
$ErrorActionPreference = 'Stop'
if (!$OutputFile) {
    Add-Type -AssemblyName System.Windows.Forms
    $save = New-Object System.Windows.Forms.SaveFileDialog
    $save.Title = 'Save the private Wi-Fi LAN diagnostic report'
    $save.Filter = 'Text report (*.txt)|*.txt'
    $save.FileName = 'Atari-WiFi-LAN-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.txt'
    $save.InitialDirectory = [Environment]::GetFolderPath('Desktop')
    if ($save.ShowDialog() -ne 'OK') { exit 0 }
    $OutputFile = $save.FileName
}
if (Test-Path -LiteralPath $OutputFile) { throw 'A report already exists at this path. Choose a new filename.' }
$report = New-Object System.Collections.Generic.List[string]
$report.Add('PRIVATE Atari Wi-Fi LAN preflight report')
$report.Add('Contains local adapter names, addresses, DNS and routes. Do not publish publicly.')
$report.Add('Read-only collection: no network, driver, bridge or registry changes.')
$report.Add('Collected: ' + (Get-Date -Format o))
function Add-ReportSection([string]$Name, [scriptblock]$Read) {
    $report.Add("`r`n=== $Name ===")
    try { $report.Add((& $Read | Out-String -Width 240).TrimEnd()) }
    catch { $report.Add('Unavailable: ' + $_.Exception.Message) }
}
Add-ReportSection 'Windows build' {
    Get-CimInstance Win32_OperatingSystem | Select-Object Caption, Version, BuildNumber, OSArchitecture
}
Add-ReportSection 'Physical and TAP adapters' {
    Get-NetAdapter -IncludeHidden | Select-Object Name, InterfaceDescription, InterfaceGuid, ifIndex, InterfaceType, Status, HardwareInterface, DriverVersion
}
Add-ReportSection 'Physical network driver versions' {
    Get-CimInstance Win32_PnPSignedDriver -Filter "DeviceClass='NET'" |
        Select-Object DeviceName, DriverProviderName, DriverVersion, DriverDate, IsSigned
}
Add-ReportSection 'DHCP and interface settings' {
    Get-NetIPInterface | Select-Object InterfaceAlias, InterfaceIndex, AddressFamily, Dhcp, ConnectionState, InterfaceMetric
}
Add-ReportSection 'IP addresses' {
    Get-NetIPAddress | Select-Object InterfaceAlias, AddressFamily, IPAddress, PrefixLength, PrefixOrigin, AddressState, SkipAsSource
}
Add-ReportSection 'Default routes' {
    Get-NetRoute | Where-Object { $_.DestinationPrefix -in @('0.0.0.0/0', '::/0') } |
        Select-Object InterfaceAlias, InterfaceIndex, DestinationPrefix, NextHop, RouteMetric
}
Add-ReportSection 'DNS configuration' { Get-DnsClientServerAddress | Select-Object InterfaceAlias, AddressFamily, ServerAddresses }
Add-ReportSection 'Bridge bindings' {
    Get-NetAdapterBinding -Name '*' -ComponentID ms_bridge -IncludeHidden |
        Select-Object Name, InterfaceDescription, ComponentID, Enabled
}
$netsh = Join-Path $env:SystemRoot 'System32\netsh.exe'
foreach ($arguments in @(@('bridge', '?'), @('bridge', 'create', '?'), @('bridge', 'destroy', '?'),
                          @('bridge', 'set', 'adapter', '?'), @('bridge', 'list'), @('bridge', 'show', 'adapter'), @('bridge', 'dump'))) {
    $report.Add("`r`n=== netsh $($arguments -join ' ') ===")
    try {
        $report.Add((& $netsh @arguments 2>&1 | Out-String -Width 240).TrimEnd())
        $report.Add('Exit code: ' + $LASTEXITCODE)
    } catch { $report.Add('Unavailable: ' + $_.Exception.Message) }
}
# CreateNew prevents overwriting an existing diagnostic or user file.
$stream = New-Object System.IO.FileStream($OutputFile, [System.IO.FileMode]::CreateNew)
try {
    $text = [Text.Encoding]::UTF8.GetBytes(($report -join "`r`n") + "`r`n")
    $stream.Write($text, 0, $text.Length)
} finally { $stream.Dispose() }
Write-Host "Saved read-only report: $OutputFile"
