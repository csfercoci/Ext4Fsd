# Testing

## Unit tests (host)

`tests/build_test.bat` — 17/17 header-level tests (sb, dir, extents, image). Run from VS 2026 dev cmd prompt.

## VM testing

VMware Workstation VM at `C:\dev\XfsFsd\vm\Win11-XfsFsd-Test\Win11-XfsFsd-Test.vmx` (encrypted, `PASS_TPM` env var for `-vp`).

### vmrun access

```
$vm='C:\dev\XfsFsd\vm\Win11-XfsFsd-Test\Win11-XfsFsd-Test.vmx'
$vp=$env:PASS_TPM
vmrun -T ws -vp $vp start $vm gui
vmrun -T ws -vp $vp -gu csfercoci -gp cristian CopyFileFromHostToGuest $vm hostpath guestpath
vmrun -T ws -vp $vp -gu csfercoci -gp cristian runProgramInGuest $vm program args
vmrun -T ws -vp $vp reset $vm hard
```

Guest credentials: `csfercoci` / `cristian`. Auto-login configured via `vm_set_autologin.ps1`.

### Test scripts (repo root)

- `vm_ext4_install_test.ps1` — install driver + basic IO test
- `vm_ext4_persist_test.ps1` — write phase / verify phase (pass `write` or `verify`)
- `vm_persist_write.ps1` — simpler persist write

### Safe VM test via scheduled task

Filesystem drivers run as SYSTEM. `vmrun` executes as the guest user, which may lack privileges. Use scheduled tasks to run test scripts as SYSTEM:

```powershell
# Copy script to guest
vmrun -T ws -vp $vp -gu csfercoci -gp cristian CopyFileFromHostToGuest $vm 'C:\Temp\script.ps1' 'C:\Temp\script.ps1'

# Create + run scheduled task as SYSTEM
vmrun -T ws -vp $vp -gu csfercoci -gp cristian runProgramInGuest $vm 'C:\Windows\System32\schtasks.exe' '/Create' '/TN' 'TestTask' '/SC' 'ONCE' '/ST' '23:59' '/RU' 'SYSTEM' '/RL' 'HIGHEST' '/F' '/TR' 'powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\Temp\script.ps1'
vmrun -T ws -vp $vp -gu csfercoci -gp cristian runProgramInGuest $vm 'C:\Windows\System32\schtasks.exe' '/Run' '/TN' 'TestTask'

# Wait, then copy log back
Start-Sleep -Seconds 60
vmrun -T ws -vp $vp -gu csfercoci -gp cristian CopyFileFromGuestToHost $vm 'C:\Temp\log.txt' 'C:\Temp\log_from_guest.txt'
```

### Important: reboot before installing new builds

Filesystem drivers cannot be unloaded while volumes are mounted (`sc stop` fails with 1052). **Always reboot the VM before installing a new driver build.** After reboot, the service is STOPPED and `C:\Windows\System32\drivers\Ext2Fsd.sys` is unlocked.

If you try to `Copy-Item` a new `.sys` over a running driver, the copy will silently fail (file locked). The service will continue running the old image.
