[CmdletBinding()]
param(
    [ValidateSet('Both', 'Shinogi', 'ARAnyM')][string]$Product = 'Both',
    [switch]$Install
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'tap-adapter.ps1')
Add-Type -AssemblyName System.Windows.Forms

if ($Install) {
    $log = '(log not opened)'
    $transcribing = $false
    try {
        $log = Get-AtariSecureLog 'adapter-setup'
        Start-Transcript -Path $log -Append | Out-Null
        $transcribing = $true
        $products = @($Product)
        if ($Product -eq 'Both') { $products = @('Shinogi', 'ARAnyM') }
        $lines = @()
        foreach ($name in $products) {
            $result = Install-AtariTap $name (Join-Path $PSScriptRoot 'tap')
            if ($result.Adapter) { $lines += "$name`: $($result.Adapter.Name)" }
            else { $lines += "$name`: adapter is waiting for a Windows restart." }
            if ($result.RebootRequired) { $lines += 'Windows needs a restart before using this adapter.' }
        }
        [void][System.Windows.Forms.MessageBox]::Show(
            (($lines -join "`r`n") + "`r`n`r`nAdapters are installed. Your Wi-Fi, routes and DNS were not changed.`r`nLAN bridging is a separate setup step."),
            'Atari network adapters', 'OK', 'Information')
        exit 0
    } catch {
        [void][System.Windows.Forms.MessageBox]::Show(
            ($_.Exception.Message + "`r`nLog: $log"), 'Atari adapter setup', 'OK', 'Error')
        exit 1
    } finally { if ($transcribing) { Stop-Transcript | Out-Null } }
}

$form = New-Object System.Windows.Forms.Form
$form.Text = 'Atari network adapter setup'
$form.ClientSize = New-Object System.Drawing.Size(520, 240)
$form.StartPosition = 'CenterScreen'
$form.FormBorderStyle = 'FixedDialog'
$form.MaximizeBox = $false
$label = New-Object System.Windows.Forms.Label
$label.SetBounds(20, 18, 480, 80)
$label.Text = "Install the signed TAP adapter without installing OpenVPN.`r`nEach emulator gets its own adapter; the driver is shared.`r`n`r`nThis step does not change your Wi-Fi, bridge, addresses or DNS."
$form.Controls.Add($label)
$choice = New-Object System.Windows.Forms.ComboBox
$choice.SetBounds(20, 108, 230, 28)
$choice.DropDownStyle = 'DropDownList'
[void]$choice.Items.AddRange(@('Both', 'Shinogi', 'ARAnyM'))
$choice.SelectedItem = $Product
$form.Controls.Add($choice)
$button = New-Object System.Windows.Forms.Button
$button.SetBounds(270, 105, 220, 32)
$button.Text = 'Install / check adapters'
$button.Add_Click({
    try {
        $script = Join-Path $PSScriptRoot 'tap-setup.ps1'
        if ($script.Contains('"')) { throw 'Unsupported package path.' }
        $button.Enabled = $false
        $launchArguments = '-NoProfile -ExecutionPolicy Bypass -File "' + $script + '" -Install -Product ' + $choice.SelectedItem
        $process = Start-Process -FilePath (Join-Path $PSHOME 'powershell.exe') -Verb RunAs -ArgumentList $launchArguments -Wait -PassThru
        if ($process.ExitCode -ne 0) { throw 'Adapter setup was not completed. Existing networking was preserved.' }
    } catch { [void][System.Windows.Forms.MessageBox]::Show($_.Exception.Message, 'Adapter setup', 'OK', 'Error') }
    finally { $button.Enabled = $true }
})
$form.Controls.Add($button)
$note = New-Object System.Windows.Forms.Label
$note.SetBounds(20, 158, 480, 65)
$note.Text = "Internet access over Wi-Fi already works with SLIRP in either app.`r`nA separate Atari LAN address needs bridging. Wi-Fi compatibility`r`nmust be tested on your laptop before it can be enabled normally."
$form.Controls.Add($note)
[void]$form.ShowDialog()
