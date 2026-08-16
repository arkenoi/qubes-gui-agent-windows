Add-Type @"
using System;using System.Runtime.InteropServices;
public class WW {
 [DllImport("user32.dll")] public static extern int GetWindowLong(IntPtr h,int i);
 [DllImport("user32.dll",SetLastError=true)] public static extern int SetWindowLong(IntPtr h,int i,int v);
 [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h,IntPtr a,int x,int y,int cx,int cy,uint f);
}
"@
$np = Get-Process notepad -EA SilentlyContinue | Select-Object -First 1
Write-Output "=== RESULT ==="
Write-Output ("whoami=" + [System.Security.Principal.WindowsIdentity]::GetCurrent().Name)
if ($np) {
    $h=$np.MainWindowHandle
    $before=[WW]::GetWindowLong($h,-16)
    [System.Runtime.InteropServices.Marshal]::SetLastWin32Error(0)
    $ret=[WW]::SetWindowLong($h,-16, ($before -band (-bnot 0x00C00000)) -bor 0x00080000)
    $err=[System.Runtime.InteropServices.Marshal]::GetLastWin32Error()
    [void][WW]::SetWindowPos($h,[IntPtr]::Zero,0,0,0,0,0x0020 -bor 0x0002 -bor 0x0001)
    Start-Sleep -Seconds 2
    $after=[WW]::GetWindowLong($h,-16)
    [pscustomobject]@{
        hwnd=("0x{0:X}" -f [int64]$h)
        style_before=("0x{0:X}" -f $before); style_after=("0x{0:X}" -f $after)
        setlong_ret=$ret; lasterror=$err
        stuck=(($after -band 0x00C00000) -ne 0x00C00000)
    } | ConvertTo-Json -Compress
}
