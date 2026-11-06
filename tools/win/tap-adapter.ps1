# Shared standalone TAP setup. Dot-source to load; this file has no setup side effects.
# Physical adapters, bridges, routes, IP addresses and DNS are never changed here.
function Get-AtariTapState([ValidateSet('Shinogi', 'ARAnyM')][string]$Product) {
    $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey('LocalMachine', 'Registry64')
    try {
        $key = $base.OpenSubKey("SOFTWARE\$Product\LAN")
        if (!$key) { return $null }
        try {
            $guid = $key.GetValue('TapGuid', '')
            $deviceId = $key.GetValue('TapDeviceId', '')
            if (!$guid -and !$deviceId) { return $null }
            return [PSCustomObject]@{ Guid = $guid; DeviceId = $deviceId; RestartBoot = $key.GetValue('TapRestartBoot', ''); RecoveryRequired = [bool]$key.GetValue('TapRecoveryRequired', 0) }
        } finally { $key.Dispose() }
    } finally { $base.Dispose() }
}

function Get-AtariBootId {
    return (Get-CimInstance Win32_OperatingSystem -ErrorAction Stop).LastBootUpTime.ToUniversalTime().ToString('o')
}

function Save-AtariTapState([ValidateSet('Shinogi', 'ARAnyM')][string]$Product, [string]$Guid,
                           [string]$DeviceId, [string]$RestartBoot, [bool]$RecoveryRequired = $false) {
    $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey('LocalMachine', 'Registry64')
    try {
        $key = $base.CreateSubKey("SOFTWARE\$Product\LAN")
        try {
            $key.SetValue('TapGuid', $Guid, 'String')
            $key.SetValue('TapDeviceId', $DeviceId, 'String')
            $key.SetValue('TapRestartBoot', $RestartBoot, 'String')
            $key.SetValue('TapRecoveryRequired', [int]$RecoveryRequired, 'DWord')
        }
        finally { $key.Dispose() }
    } finally { $base.Dispose() }
}

function Clear-AtariTapRecovery([ValidateSet('Shinogi', 'ARAnyM')][string]$Product) {
    $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey('LocalMachine', 'Registry64')
    try {
        $key = $base.OpenSubKey("SOFTWARE\$Product\LAN", $true)
        if (!$key) { return }
        try {
            foreach ($name in @('TapGuid', 'TapDeviceId', 'TapRestartBoot', 'TapRecoveryRequired')) { $key.DeleteValue($name, $false) }
        } finally { $key.Dispose() }
    } finally { $base.Dispose() }
}

function Test-AtariTap([Guid]$Guid) {
    $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey('LocalMachine', 'Registry64')
    try {
        $root = $base.OpenSubKey('SYSTEM\CurrentControlSet\Control\Class\{4D36E972-E325-11CE-BFC1-08002BE10318}')
        if (!$root) { return $false }
        try {
            foreach ($name in $root.GetSubKeyNames()) {
                try { $key = $root.OpenSubKey($name) }
                catch [System.UnauthorizedAccessException] { continue }
                catch [System.Security.SecurityException] { continue }
                if (!$key) { continue }
                try {
                    if ($key.GetValue('ComponentId', '') -ine 'tap0901') { continue }
                    $instance = [Guid]::Empty
                    if ([Guid]::TryParse([string]$key.GetValue('NetCfgInstanceId', ''), [ref]$instance) -and $instance -eq $Guid) {
                        return $true
                    }
                } finally { $key.Dispose() }
            }
        } finally { $root.Dispose() }
    } finally { $base.Dispose() }
    return $false
}

function Get-AtariTap([Guid]$Guid) {
    $found = @(Get-NetAdapter -IncludeHidden | Where-Object { [Guid]$_.InterfaceGuid -eq $Guid })
    if ($found.Count -ne 1 -or !(Test-AtariTap $Guid)) {
        throw 'The saved dedicated TAP adapter is missing or is a different driver. No other adapter was selected.'
    }
    return $found[0]
}

function Assert-AtariTapCompatible([string]$DriverFolder) {
    $service = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\tap0901' -ErrorAction SilentlyContinue
    if (!$service) {
        if (@(Get-NetAdapter -IncludeHidden | Where-Object { Test-AtariTap ([Guid]$_.InterfaceGuid) }).Count) {
            throw 'An existing TAP adapter has an unknown driver service. It has not been changed.'
        }
        return
    }
    $windows = [Environment]::GetFolderPath('Windows')
    $binary = [string]$service.ImagePath
    if ($binary.StartsWith('\SystemRoot\', [StringComparison]::OrdinalIgnoreCase)) {
        $binary = Join-Path $windows $binary.Substring(12)
    } elseif ($binary.StartsWith('System32\', [StringComparison]::OrdinalIgnoreCase)) {
        $binary = Join-Path $windows $binary
    } elseif ($binary.StartsWith('\??\')) { $binary = $binary.Substring(4) }
    if (!$binary -or !(Test-Path -LiteralPath $binary -PathType Leaf) -or
        (Get-FileHash -LiteralPath $binary -Algorithm SHA256).Hash -ne
        (Get-FileHash -LiteralPath (Join-Path $DriverFolder 'tap0901.sys') -Algorithm SHA256).Hash) {
        throw 'An existing VPN uses a different TAP driver version. It has not been upgraded, restarted or replaced. Keep Internet/SLIRP selected until this driver conflict is resolved.'
    }
}

function Assert-AtariLogSecurity([string]$Path) {
    $item = Get-Item -LiteralPath $Path -Force -ErrorAction Stop
    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'The setup log path is a link/junction. It has not been used.' }
    $acl = Get-Acl -LiteralPath $Path
    $trusted = @('S-1-5-18', 'S-1-5-32-544') # SYSTEM, Administrators
    if ($acl.GetOwner([Security.Principal.SecurityIdentifier]).Value -notin $trusted) {
        throw 'The setup log path is not owned by SYSTEM/Administrators. Rename the existing AtariLAN folder before retrying.'
    }
    $writes = [Security.AccessControl.FileSystemRights]::Write -bor
              [Security.AccessControl.FileSystemRights]::Delete -bor
              [Security.AccessControl.FileSystemRights]::DeleteSubdirectoriesAndFiles -bor
              [Security.AccessControl.FileSystemRights]::ChangePermissions -bor
              [Security.AccessControl.FileSystemRights]::TakeOwnership
    foreach ($rule in $acl.GetAccessRules($true, $true, [Security.Principal.SecurityIdentifier])) {
        if ($rule.AccessControlType -eq 'Allow' -and $rule.IdentityReference.Value -notin $trusted -and
            ($rule.FileSystemRights -band $writes)) { throw 'The setup log path is writable by other users. It has not been used.' }
    }
}

function Get-AtariSecureLog([ValidateSet('adapter-setup', 'Shinogi-lan-setup', 'ARAnyM-lan-setup')][string]$Name) {
    $folder = Join-Path ([Environment]::GetFolderPath('CommonApplicationData')) 'AtariLAN'
    $acl = New-Object Security.AccessControl.DirectorySecurity
    $admins = New-Object Security.Principal.SecurityIdentifier('S-1-5-32-544')
    $acl.SetOwner($admins)
    $acl.SetAccessRuleProtection($true, $false)
    foreach ($sid in @('S-1-5-18', 'S-1-5-32-544')) {
        $identity = New-Object Security.Principal.SecurityIdentifier($sid)
        $acl.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule(
            $identity, 'FullControl', 'ContainerInherit,ObjectInherit', 'None', 'Allow')))
    }
    $users = New-Object Security.Principal.SecurityIdentifier('S-1-5-32-545')
    $acl.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule(
        $users, 'ReadAndExecute', 'ContainerInherit,ObjectInherit', 'None', 'Allow')))
    # The Windows PowerShell 5.1 overload installs the ACL on creation.
    # A racing/preexisting directory is always validated before any append.
    [void][IO.Directory]::CreateDirectory($folder, $acl)
    Assert-AtariLogSecurity $folder
    $path = Join-Path $folder ($Name + '.log')
    if (Get-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue) { Assert-AtariLogSecurity $path }
    return $path
}

function Invoke-AtariTapInstaller([string]$DriverFolder) {
    foreach ($file in @('tap0901.cat', 'tap0901.sys', 'OemVista.inf')) {
        if (!(Test-Path -LiteralPath (Join-Path $DriverFolder $file) -PathType Leaf)) {
            throw "The standalone TAP package is incomplete: $file"
        }
    }
    if ((Get-AuthenticodeSignature -LiteralPath (Join-Path $DriverFolder 'tap0901.cat')).Status -ne 'Valid') {
        throw 'Windows could not verify the bundled TAP catalog signature.'
    }
    # Windows validates the SYS against the signed catalog during installation.
    $inf = Join-Path $DriverFolder 'OemVista.inf'
    if ($inf.Contains('"') -or $inf.Contains("`n") -or $inf.Contains("`r")) { throw 'Unsupported package path.' }
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = Join-Path (Split-Path $DriverFolder -Parent) 'install-tap.exe'
    $info.Arguments = '"' + $inf + '"'
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $info
    try {
        [void]$process.Start()
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        $timedOut = !$process.WaitForExit(90000)
        if ($timedOut) {
            $process.Kill()
            $process.WaitForExit()
        }
        Write-Verbose $stderr.Result
        $line = @($stdout.Result -split "`r?`n" | Where-Object { $_.Trim() }) | Select-Object -Last 1
        $result = if ($line) { $line | ConvertFrom-Json } else {
            [PSCustomObject]@{ deviceInstanceId = $null; guid = $null; rebootRequired = $false }
        }
        $code = $process.ExitCode
        $diagnostic = $stderr.Result
        if ($timedOut -or $result.progress) {
            $code = 258
            $diagnostic += ' Setup did not complete; its exact device identity is retained for recovery. Check Device Manager before retrying.'
        }
        $result | Add-Member NoteProperty ExitCode $code
        $result | Add-Member NoteProperty Diagnostic $diagnostic
        return $result
    } finally { $process.Dispose() }
}

function Ensure-AtariTap([ValidateSet('Shinogi', 'ARAnyM')][string]$Product, [string]$DriverFolder) {
    $saved = Get-AtariTapState $Product
    if ($saved) {
        $restart = $saved.RestartBoot -and $saved.RestartBoot -eq (Get-AtariBootId)
        if ($saved.RecoveryRequired) {
            if ($restart) { return [PSCustomObject]@{ Adapter = $null; RebootRequired = $true; Created = $false } }
            # Enumerate successfully before concluding the owned node is gone.
            # A query failure must never be mistaken for successful removal.
            $remaining = @(Get-PnpDevice -ErrorAction Stop | Where-Object { $_.InstanceId -ieq $saved.DeviceId })
            if ($remaining.Count) { throw "The previous setup device still needs recovery: $($saved.DeviceId). Check Device Manager; no second device was created." }
            Clear-AtariTapRecovery $Product
            return Ensure-AtariTap $Product $DriverFolder
        }
        if ($saved.Guid) {
            $adapter = Get-AtariTap ([Guid]$saved.Guid)
        } else {
            $found = @(Get-NetAdapter -IncludeHidden | Where-Object { $_.PnPDeviceID -ieq $saved.DeviceId })
            if (!$found.Count -and $restart) {
                return [PSCustomObject]@{ Adapter = $null; RebootRequired = $true; Created = $false }
            }
            if ($found.Count -ne 1) { throw 'The owned TAP device has no adapter. Restart Windows or check Device Manager; no replacement was created.' }
            $adapter = Get-AtariTap ([Guid]$found[0].InterfaceGuid)
            Save-AtariTapState $Product ('{' + ([Guid]$adapter.InterfaceGuid).ToString() + '}') $saved.DeviceId $saved.RestartBoot
        }
        if ($saved.DeviceId -and $adapter.PnPDeviceID -ine $saved.DeviceId) {
            throw 'The owned TAP device and adapter GUID disagree. Existing networking was preserved.'
        }
        return [PSCustomObject]@{ Adapter = $adapter; RebootRequired = [bool]$restart; Created = $false }
    }
    # Read the boot identity before any mutation; a transient CIM failure
    # cannot leave a permanently unknown restart marker after installation.
    $boot = Get-AtariBootId
    Assert-AtariTapCompatible $DriverFolder
    $result = Invoke-AtariTapInstaller $DriverFolder
    if ($result.ExitCode -notin @(0, 1)) {
        if ($result.deviceInstanceId) {
            $restart = if ($result.rebootRequired) { $boot } else { '' }
            Save-AtariTapState $Product '' $result.deviceInstanceId $restart $true
        }
        throw "Windows rejected TAP installation (code $($result.ExitCode)). $($result.Diagnostic)"
    }
    if (!$result.deviceInstanceId) { throw 'The TAP installer did not identify its own device. No adapter was adopted.' }
    $restart = ''
    if ($result.rebootRequired -or $result.ExitCode -eq 1) { $restart = $boot }
    # Save the exact new device identity before discovery/renaming. Never
    # infer ownership from a before/after list of possibly unrelated devices.
    Save-AtariTapState $Product ([string]$result.guid) $result.deviceInstanceId $restart
    $found = @()
    for ($i = 0; $i -lt 30; $i++) {
        $found = @(Get-NetAdapter -IncludeHidden | Where-Object {
            $_.PnPDeviceID -ieq $result.deviceInstanceId
        })
        if ($found.Count) { break }
        Start-Sleep -Milliseconds 500
    }
    if (!$found.Count -and $restart) {
        return [PSCustomObject]@{ Adapter = $null; RebootRequired = $true; Created = $true }
    }
    if ($found.Count -ne 1) { throw 'Windows did not expose exactly one adapter for the installed device. No other adapter was adopted.' }
    $adapter = Get-AtariTap ([Guid]$found[0].InterfaceGuid)
    if ($result.guid -and [Guid]$result.guid -ne [Guid]$adapter.InterfaceGuid) { throw 'The installed TAP device and GUID disagree.' }
    Save-AtariTapState $Product ('{' + ([Guid]$adapter.InterfaceGuid).ToString() + '}') $result.deviceInstanceId $restart
    $adapter | Rename-NetAdapter -NewName "$Product LAN" -ErrorAction Stop
    return [PSCustomObject]@{
        Adapter = (Get-AtariTap ([Guid]$adapter.InterfaceGuid))
        RebootRequired = [bool]$restart
        Created = $true
    }
}

function Install-AtariTap([ValidateSet('Shinogi', 'ARAnyM')][string]$Product, [string]$DriverFolder) {
    $admin = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    if (!$admin.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Approve the Windows administrator prompt to install the signed network adapter.'
    }
    $mutex = New-Object System.Threading.Mutex($false, 'Global\Atari-TAP-Setup')
    $locked = $false
    try {
        try { $locked = $mutex.WaitOne(1000) }
        catch [System.Threading.AbandonedMutexException] { $locked = $true }
        if (!$locked) { throw 'Another Atari network setup is running. Wait for it to finish.' }
        return Ensure-AtariTap $Product $DriverFolder
    } finally {
        if ($locked) { $mutex.ReleaseMutex() }
        $mutex.Dispose()
    }
}
