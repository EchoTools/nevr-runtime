function Ensure-FirewallRule {
    param(
        [string]$FirewallRuleName,
        [string]$FirewallPortRange
    )
    if (-not (Get-NetFirewallRule -DisplayName $FirewallRuleName -ErrorAction SilentlyContinue)) {
        Write-Verbose "Firewall rule '$FirewallRuleName' not found. Attempting to create it."
        $firewallScriptContent = @"
        Write-Host 'Creating firewall rule for UDP ports $FirewallPortRange...'
        New-NetFirewallRule -DisplayName '$FirewallRuleName' `
            -Direction Inbound `
            -Action Allow `
            -Protocol UDP `
            -LocalPort '$FirewallPortRange' `
            -Profile Any
        Write-Host "Firewall rule '$FirewallRuleName' created successfully."
        Write-Host "You can close this window."
        pause
"@
        try {
            Start-Process powershell -ArgumentList "-NoProfile -ExecutionPolicy Bypass -Command `"$firewallScriptContent`"" -Verb RunAs -ErrorAction Stop
            Write-Verbose "Launched firewall configuration process. Please approve the UAC prompt if it appears."
        } catch {
            Write-Warning "Failed to create firewall rule. The server may not be accessible from other computers."
        }
    } else {
        Write-Verbose "Firewall rule '$FirewallRuleName' already exists."
    }
}
