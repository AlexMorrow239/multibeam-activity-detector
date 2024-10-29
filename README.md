# Setup
## Do the following in an admin powershell instance:
usbipd list (to find bus ID)

usbipd bind --busid [busid] --force

usbipd attach --wsl --busid [busid]
