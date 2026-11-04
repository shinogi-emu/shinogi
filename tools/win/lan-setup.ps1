# Configure only Shinogi's dedicated TAP and bridge. This script runs with
# elevation only after the user chooses Set up LAN or Remove LAN in Settings.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][Guid]$EthernetGuid,
    [Guid]$TapGuid = [Guid]::Empty,
    [switch]$CheckOnly,
    [switch]$Remove
)

$ErrorActionPreference = 'Stop'
$stateKey = 'HKLM:\SOFTWARE\shinogi\LAN'
$adapterKey = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4D36E972-E325-11CE-BFC1-08002BE10318}'
$netsh = Join-Path $env:SystemRoot 'System32\netsh.exe'
$createdBridge = $null
$transcribing = $false

function Invoke-Native([string]$Program, [string[]]$Arguments) {
    # All arguments are fixed tokens, validated GUIDs, or local paths/names.
    if ($Arguments | Where-Object { $_.Contains('"') -or $_.Contains("`n") }) {
        throw 'An adapter name or path contains unsupported characters.'
    }
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $Program
    $info.Arguments = (($Arguments | ForEach-Object { '"' + $_ + '"' }) -join ' ')
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $info
    [void]$process.Start()
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    if (!$process.WaitForExit(90000)) {
        $process.Kill()
        throw "Network setup command timed out: $Program"
    }
    $text = $stdout.Result + $stderr.Result
    $code = $process.ExitCode
    $process.Dispose()
    Write-Host "$Program $($Arguments -join ' ')`n$text"
    return [PSCustomObject]@{ Code = $code; Text = $text }
}

function Get-Adapter([Guid]$Guid) {
    $found = @(Get-NetAdapter -IncludeHidden | Where-Object { [Guid]$_.InterfaceGuid -eq $Guid })
    if ($found.Count -ne 1) { throw "Network adapter $Guid is unavailable." }
    return $found[0]
}

function Test-Tap([Guid]$Guid) {
    $found = @(Get-ChildItem $adapterKey | ForEach-Object {
        Get-ItemProperty $_.PSPath -ErrorAction SilentlyContinue
    } | Where-Object { $_.NetCfgInstanceId -and [Guid]$_.NetCfgInstanceId -eq $Guid -and $_.ComponentId -eq 'tap0901' })
    return $found.Count -eq 1
}

function Get-Bridges {
    $result = Invoke-Native $netsh @('bridge', 'list')
    if ($result.Code -ne 0) {
        throw 'This Windows build does not provide automatic bridge setup. Internet networking remains available.'
    }
    return @([regex]::Matches($result.Text, '\{[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}\}') |
        ForEach-Object { [Guid]$_.Value } | Select-Object -Unique)
}

function Assert-Ready($State) {
    if (!$State -or !$State.BridgeGuid -or [Guid]$State.EthernetGuid -ne $EthernetGuid) {
        throw 'LAN is not set up for the selected Ethernet adapter. Choose Set up LAN in Settings.'
    }
    if ($TapGuid -ne [Guid]::Empty -and [Guid]$State.TapGuid -ne $TapGuid) {
        throw 'The saved TAP adapter does not match the managed LAN setup.'
    }
    $ethernet = Get-Adapter ([Guid]$State.EthernetGuid)
    $tap = Get-Adapter ([Guid]$State.TapGuid)
    [void](Get-Adapter ([Guid]$State.BridgeGuid))
    if (!$ethernet.HardwareInterface -or $ethernet.InterfaceType -ne 6 -or !(Test-Tap ([Guid]$State.TapGuid))) {
        throw 'The selected Ethernet or TAP adapter is not supported.'
    }
    if (!$CheckOnly -and !(@(Get-Bridges) -contains [Guid]$State.BridgeGuid)) {
        throw 'The managed network bridge is missing.'
    }
    foreach ($adapter in @($ethernet, $tap)) {
        $binding = Get-NetAdapterBinding -Name $adapter.Name -ComponentID 'ms_bridge' -ErrorAction Stop
        if (!$binding.Enabled) { throw "The managed bridge is no longer connected to $($adapter.Name)." }
    }
}

function Get-BridgeId([string]$Output, $Adapter) {
    # Bridge IDs come from netsh, not from IP interface indices. Match the
    # complete adapter name/description and reject ambiguous localized output.
    $ids = @()
    foreach ($line in ($Output -split "`r?`n")) {
        if ($line -notmatch '^\s*(\d+)\s+(.+?)\s{2,}(\S+)\s*$') { continue }
        $id = [int]$Matches[1]
        $label = $Matches[2].TrimEnd()
        foreach ($name in @($Adapter.Name, $Adapter.InterfaceDescription)) {
            if ($name -and $label -eq $name) {
                $ids += $id
                break
            }
        }
    }
    $ids = @($ids | Select-Object -Unique)
    if ($ids.Count -ne 1) { throw "Windows did not identify an unambiguous bridge port for $($Adapter.Name)." }
    return $ids[0]
}

function Restore-Ethernet($State) {
    $adapter = Get-Adapter ([Guid]$State.EthernetGuid)
    Enable-NetAdapterBinding -Name $adapter.Name -ComponentID 'ms_tcpip' | Out-Null
    Set-NetIPInterface -InterfaceIndex $adapter.ifIndex -AddressFamily IPv4 -Dhcp Enabled
    if ($State.OriginalDNS) {
        Set-DnsClientServerAddress -InterfaceIndex $adapter.ifIndex -ServerAddresses ($State.OriginalDNS -split '[,\s]+' | Where-Object { $_ })
    } else {
        Set-DnsClientServerAddress -InterfaceIndex $adapter.ifIndex -ResetServerAddresses
    }
}

try {
    $state = Get-ItemProperty $stateKey -ErrorAction SilentlyContinue
    if ($CheckOnly) {
        Assert-Ready $state
        exit 0
    }
    $admin = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    if (!$admin.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Administrator permission is required for LAN setup.'
    }
    $logFolder = Join-Path $env:ProgramData 'shinogi'
    New-Item -ItemType Directory -Path $logFolder -Force | Out-Null
    Start-Transcript -Path (Join-Path $logFolder 'lan-setup.log') -Append | Out-Null
    $transcribing = $true
    New-Item -Path $stateKey -Force | Out-Null
    Set-ItemProperty $stateKey 'LastError' ''
    $bridges = @(Get-Bridges)
    if ($Remove) {
        if ($state.BridgeGuid -and $bridges -contains [Guid]$state.BridgeGuid) {
            $result = Invoke-Native $netsh @('bridge', 'destroy', "bridge=$($state.BridgeGuid)")
            if ($result.Code -ne 0) { throw 'Windows could not remove the managed bridge.' }
            Restore-Ethernet $state
        }
        Remove-ItemProperty $stateKey -Name 'BridgeGuid', 'EthernetGuid' -ErrorAction SilentlyContinue
        Write-Host 'LAN bridge removed. The shared TAP driver and dedicated adapter are kept for reuse.'
        exit 0
    }
    if ($state.BridgeGuid) {
        if ([Guid]$state.EthernetGuid -ne $EthernetGuid) {
            throw 'A different Ethernet adapter is already configured. Choose Remove LAN first, then set up the new adapter.'
        }
        Assert-Ready $state
        exit 0
    }
    if ($bridges.Count) { throw 'An existing Windows bridge is present. Shinogi will not modify a bridge it did not create.' }
    $ethernet = Get-Adapter $EthernetGuid
    if (!$ethernet.HardwareInterface -or $ethernet.InterfaceType -ne 6) {
        throw 'Automatic LAN setup supports physical Ethernet. Wi-Fi bridging is not supported in this version.'
    }
    if ($ethernet.Status -ne 'Up') { throw 'Connect the selected Ethernet adapter before setting up LAN.' }
    $ipInterface = Get-NetIPInterface -InterfaceIndex $ethernet.ifIndex -AddressFamily IPv4
    if ($ipInterface.Dhcp -ne 'Enabled') {
        throw 'Automatic LAN setup requires Windows Ethernet addressing set to automatic (DHCP). Static host addressing is not changed.'
    }
    $manualIPv6 = @(Get-NetIPAddress -InterfaceIndex $ethernet.ifIndex -AddressFamily IPv6 |
        Where-Object { $_.PrefixOrigin -eq 'Manual' -and $_.IPAddress -notlike 'fe80:*' })
    if ($manualIPv6.Count) { throw 'Static Windows IPv6 settings require a separately configured bridge. They have not been changed.' }
    $ipv6DNSKey = "HKLM:\SYSTEM\CurrentControlSet\Services\Tcpip6\Parameters\Interfaces\{$EthernetGuid}"
    if ((Get-ItemProperty $ipv6DNSKey -ErrorAction SilentlyContinue).NameServer) {
        throw 'Static Windows IPv6 DNS settings require a separately configured bridge. They have not been changed.'
    }
    $hostIPv4 = @(Get-NetIPAddress -InterfaceIndex $ethernet.ifIndex -AddressFamily IPv4 |
        Where-Object { $_.IPAddress -notlike '169.254.*' })
    if (!$hostIPv4.Count) { throw 'Windows Ethernet must have a working IPv4 address before LAN setup.' }
    $hadRouter = @(Get-NetRoute -InterfaceIndex $ethernet.ifIndex -DestinationPrefix '0.0.0.0/0' -ErrorAction SilentlyContinue).Count -gt 0
    $dnsKey = "HKLM:\SYSTEM\CurrentControlSet\Services\Tcpip\Parameters\Interfaces\{$EthernetGuid}"
    $manualDNS = (Get-ItemProperty $dnsKey -ErrorAction SilentlyContinue).NameServer
    Set-ItemProperty $stateKey 'OriginalDNS' ([string]$manualDNS)

    if ($state.TapGuid) {
        $tap = Get-Adapter ([Guid]$state.TapGuid)
        if (!(Test-Tap ([Guid]$state.TapGuid))) { throw 'The saved managed TAP adapter is not a TAP-Windows6 adapter.' }
    } else {
        $driver = Join-Path $PSScriptRoot 'tap'
        foreach ($name in @('tap0901.cat', 'tap0901.sys', 'devcon.exe')) {
            $signature = Get-AuthenticodeSignature (Join-Path $driver $name)
            # SYS may be catalog-signed; Windows checks its catalog during
            # installation. CAT and the installer executable must verify here.
            if ($name -ne 'tap0901.sys' -and $signature.Status -ne 'Valid') {
                throw "The bundled TAP package signature is not trusted: $name"
            }
        }
        $before = @(Get-NetAdapter -IncludeHidden | ForEach-Object { [Guid]$_.InterfaceGuid })
        $result = Invoke-Native (Join-Path $driver 'devcon.exe') @('install', (Join-Path $driver 'OemVista.inf'), 'tap0901')
        if ($result.Code -gt 1) { throw 'Windows could not install the signed TAP-Windows6 adapter.' }
        $new = @()
        for ($i = 0; $i -lt 30; $i++) {
            $new = @(Get-NetAdapter -IncludeHidden | Where-Object {
                $before -notcontains [Guid]$_.InterfaceGuid -and (Test-Tap ([Guid]$_.InterfaceGuid))
            })
            if ($new.Count) { break }
            Start-Sleep -Milliseconds 500
        }
        if ($new.Count -ne 1) { throw 'Windows did not expose exactly one new TAP adapter. See the LAN setup log.' }
        $tap = $new[0]
        Set-ItemProperty $stateKey 'TapGuid' ('{' + ([Guid]$tap.InterfaceGuid).ToString() + '}')
        $tap | Rename-NetAdapter -NewName 'Shinogi LAN'
        $tap = Get-Adapter ([Guid]$tap.InterfaceGuid)
        if ($result.Code -eq 1) { throw 'The network driver needs a Windows restart. Restart, then choose Set up LAN again.' }
    }
    $ports = Invoke-Native $netsh @('bridge', 'show', 'adapter')
    if ($ports.Code -ne 0) { throw 'Windows could not enumerate bridge-capable adapters.' }
    $ethernetId = Get-BridgeId $ports.Text $ethernet
    $tapId = Get-BridgeId $ports.Text $tap
    if ($ethernetId -eq $tapId) { throw 'Windows returned the same bridge port for Ethernet and TAP. Nothing was changed.' }
    # Preserve a diagnostic snapshot before any change to physical networking.
    $snapshot = Invoke-Native $netsh @('interface', 'ipv4', 'dump')
    if ($snapshot.Code -ne 0) { throw 'Could not save the original Windows IPv4 configuration.' }
    $snapshot.Text | Set-Content (Join-Path $logFolder 'before-lan-ipv4.txt')
    $result = Invoke-Native $netsh @('bridge', 'create', "adapter=$ethernetId", "adapter=$tapId")
    $after = @(Get-Bridges)
    if ($after.Count -eq 1) {
        $createdBridge = $after[0]
        # Record ownership immediately. If rollback is interrupted or fails,
        # Remove LAN and uninstall can still identify the bridge we created.
        Set-ItemProperty $stateKey 'EthernetGuid' ('{' + $EthernetGuid.ToString() + '}')
        Set-ItemProperty $stateKey 'BridgeGuid' ('{' + $createdBridge.ToString() + '}')
    }
    if ($result.Code -ne 0 -or !$createdBridge) { throw 'Windows could not create the dedicated network bridge.' }
    $bridge = $null
    for ($i = 0; $i -lt 20; $i++) {
        try { $bridge = Get-Adapter $createdBridge; break } catch { Start-Sleep -Milliseconds 500 }
    }
    if (!$bridge) { throw 'Windows did not expose the new network bridge adapter.' }
    Set-NetIPInterface -InterfaceIndex $bridge.ifIndex -AddressFamily IPv4 -Dhcp Enabled
    if ($manualDNS) {
        Set-DnsClientServerAddress -InterfaceIndex $bridge.ifIndex -ServerAddresses ($manualDNS -split '[,\s]+' | Where-Object { $_ })
    } else {
        Set-DnsClientServerAddress -InterfaceIndex $bridge.ifIndex -ResetServerAddresses
    }
    $hostReady = $false
    for ($i = 0; $i -lt 45; $i++) {
        $addresses = @(Get-NetIPAddress -InterfaceIndex $bridge.ifIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue |
            Where-Object { $_.IPAddress -notlike '169.254.*' -and $_.AddressState -eq 'Preferred' })
        $routes = @(Get-NetRoute -InterfaceIndex $bridge.ifIndex -DestinationPrefix '0.0.0.0/0' -ErrorAction SilentlyContinue)
        if ($addresses.Count -and (!$hadRouter -or $routes.Count)) { $hostReady = $true; break }
        Start-Sleep -Seconds 1
    }
    if (!$hostReady) { throw 'Windows did not regain its IPv4 connection on the bridge. Restoring Ethernet.' }
    Set-ItemProperty $stateKey 'EthernetGuid' ('{' + $EthernetGuid.ToString() + '}')
    Set-ItemProperty $stateKey 'BridgeGuid' ('{' + $createdBridge.ToString() + '}')
    $state = Get-ItemProperty $stateKey
    Assert-Ready $state
    $createdBridge = $null
    Write-Host 'Shinogi LAN is ready. Save the Atari IPv4 settings and restart Shinogi.'
    exit 0
} catch {
    $message = $_.Exception.Message
    if ($createdBridge) {
        try {
            $result = Invoke-Native $netsh @('bridge', 'destroy', "bridge={$createdBridge}")
            if ($result.Code -ne 0) { throw 'Windows refused to remove the incomplete bridge.' }
            Restore-Ethernet ([PSCustomObject]@{ EthernetGuid = $EthernetGuid; OriginalDNS = $manualDNS })
            Remove-ItemProperty $stateKey -Name 'BridgeGuid', 'EthernetGuid' -ErrorAction SilentlyContinue
        } catch {
            $message += ' Automatic rollback also failed. See %PROGRAMDATA%\shinogi\lan-setup.log.'
        }
    }
    Write-Host $message
    if (!$CheckOnly) {
        try { Set-ItemProperty $stateKey 'LastError' $message } catch {}
    }
    exit 1
} finally {
    if ($transcribing) { Stop-Transcript | Out-Null }
}
