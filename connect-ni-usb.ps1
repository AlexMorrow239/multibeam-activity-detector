# Requires -RunAsAdministrator

# Function to write colored output
function Write-ColorOutput {
    param([string]$Message, [string]$Color = "White")
    Write-Host $Message -ForegroundColor $Color
}

# Function to check if usbipd is installed
function Test-UsbIpdInstalled {
    try {
        $null = Get-Command usbipd -ErrorAction Stop
        return $true
    }
    catch {
        Write-ColorOutput "Error: usbipd-win is not installed or not in PATH" "Red"
        Write-ColorOutput "Please install from: https://github.com/dorssel/usbipd-win/releases" "Yellow"
        return $false
    }
}

# Function to find NI USB-6501 device
function Get-NIUSBDevice {
    $devices = usbipd list
    $niDevice = $devices | Select-String -Pattern "National Instruments.*USB-6501"
    
    if ($niDevice) {
        $busid = ($niDevice -split '\s+')[0]
        Write-ColorOutput "Found NI USB-6501 device on bus ID: $busid" "Green"
        return $busid
    }
    else {
        Write-ColorOutput "No NI USB-6501 device found. Please check connection." "Red"
        return $null
    }
}

# Function to attach device to WSL
function Connect-DeviceToWSL {
    param([string]$BusId)
    
    try {
        # Check current state
        $deviceStatus = usbipd list | Select-String -Pattern $BusId
        
        # Attempt to unbind first if needed
        Write-ColorOutput "Attempting to unbind device first..." "Yellow"
        $null = usbipd unbind --busid $BusId --force 2>&1

        # Bind the device
        Write-ColorOutput "Binding device..." "Yellow"
        $bindResult = usbipd bind --busid $BusId --force 2>&1
        
        # Attach to WSL
        Write-ColorOutput "Attaching device to WSL..." "Yellow"
        $attachResult = usbipd attach --wsl --busid $BusId --force 2>&1

        Write-ColorOutput "Device successfully connected to WSL!" "Green"
        return $true
    }
    catch {
        Write-ColorOutput "Error during device connection: $_" "Red"
        return $false
    }
}

# Main execution
Write-ColorOutput "Starting NI USB-6501 WSL Connection Script..." "Cyan"

# Check if running as admin
if (-not ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-ColorOutput "This script must be run as Administrator" "Red"
    Exit 1
}

# Check for usbipd installation
if (-not (Test-UsbIpdInstalled)) {
    Exit 1
}

# Find device
$busId = Get-NIUSBDevice
if (-not $busId) {
    Exit 1
}

# Connect device
if (Connect-DeviceToWSL -BusId $busId) {
    Write-ColorOutput "`nDevice connection complete! You can now use the device in WSL" "Green"
} else {
    Write-ColorOutput "`nFailed to complete device connection" "Red"
    Exit 1
}

# Verify connection
Write-ColorOutput "`nTo verify connection in WSL, run: 'lsusb | grep National'" "Cyan"