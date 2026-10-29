# 运行桌面版并截下主窗口（类名 MiniBIMWin），用来核对界面改动与各语言的显示。
#   pwsh -NoProfile -ExecutionPolicy Bypass -File .agents/skills/ui-check/scripts/shot.ps1 [-Lang zh_CN] [-Run view.plan] [-Keys "%f"] [-Name 文件名] [-Wait 毫秒] [-Release]
# -Lang：--lang 启动参数（只对这一次有效）；-Run：--run 启动参数，打开示例房屋后执行的界面命令（不用抢焦点，可给多个，逗号分隔）；-Keys：截图前按的键（SendKeys 语法，"%f" = Alt+F 打开文件菜单，"{DOWN}{ENTER}" …）
# 输出 out/tmp/shots/<Name>.png；程序截完即关闭。用户设置 settings.json 先备份、结束后还原（按键可能切换了语言）
# 退出码：0 成功；1 没等到窗口；2 给了 -Keys 但窗口没能置到前台、没有按键
[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$Lang = '',
    [string]$Keys = '',
    [string[]]$Run = @(),
    [string]$Name = '',
    [int]$Wait = 3000,
    [switch]$Release
)
$ErrorActionPreference = 'Stop'
$root   = (Resolve-Path (Join-Path $PSScriptRoot '../../../..')).Path
$preset = if ($Release) { 'release' } else { 'debug' }
$exe    = Join-Path $root "out/$preset/MiniBIMWin/MiniBIMWin.exe"
if (-not (Test-Path $exe)) { Write-Output "找不到 $exe，先编译 MiniBIMWin（skill test）"; exit 1 }
$outDir = Join-Path $root 'out/tmp/shots'
New-Item -ItemType Directory -Force $outDir | Out-Null
if (-not $Name) { $Name = if ($Lang) { "main-$Lang" } else { 'main' } }
$out = Join-Path $outDir "$Name.png"

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System; using System.Text; using System.Runtime.InteropServices;
public static class MiniBimShot {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder sb, int n);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    // FindWindow 在这台机器的工具环境里找不到窗口，按进程枚举顶层窗口
    public static IntPtr Find(uint pid, string cls) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); var c = new StringBuilder(256); GetClassName(h, c, 256);
                                if (p == pid && c.ToString() == cls) { found = h; return false; } return true; }, IntPtr.Zero);
        return found;
    }
}
"@
# 屏幕有缩放时，不感知 DPI 的进程拿到的窗口矩形是缩小过的，PrintWindow 却按实际像素画，只截到左上一块
[MiniBimShot]::SetProcessDPIAware() | Out-Null

$settings = Join-Path $env:LOCALAPPDATA 'MiniBIM/settings.json'
$backup   = Join-Path $outDir 'settings.backup.json'
$hadSettings = Test-Path $settings
if ($hadSettings) { Copy-Item $settings $backup -Force }

$argsList = @()
if ($Lang) { $argsList += @('--lang', $Lang) }
foreach ($id in ($Run -join ',') -split ',' | Where-Object { $_ }) { $argsList += @('--run', $id.Trim()) }
$p = Start-Process -FilePath $exe -ArgumentList $argsList -WorkingDirectory (Split-Path $exe) -PassThru
try {
    $deadline = (Get-Date).AddSeconds(30)
    $h = [IntPtr]::Zero
    while ($h -eq [IntPtr]::Zero -and -not $p.HasExited -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 200; $h = [MiniBimShot]::Find([uint32]$p.Id, 'MiniBIMWin') }
    if ($h -eq [IntPtr]::Zero) { Write-Output "没有等到主窗口（进程$(if ($p.HasExited) { "已退出，退出码 $($p.ExitCode)" } else { '还在运行' })）"; exit 1 }
    Start-Sleep -Milliseconds $Wait
    $keysSent = $false
    if ($Keys) {
        # 按键发给前台窗口：只有确认 MiniBIM 在前台才按，否则按键会落到用户的其他程序上
        for ($i = 0; $i -lt 5 -and [MiniBimShot]::GetForegroundWindow() -ne $h; $i++) {
            [MiniBimShot]::SetForegroundWindow($h) | Out-Null
            Start-Sleep -Milliseconds 300
        }
        if ([MiniBimShot]::GetForegroundWindow() -eq $h) {
            [System.Windows.Forms.SendKeys]::SendWait($Keys)
            Start-Sleep -Milliseconds 800
            $keysSent = $true
        } else {
            Write-Output "MiniBIM 的窗口没能置到前台（系统不让后台进程抢焦点），没有按键；截图里没有按键的效果"
        }
    }

    # 截图只取这个窗口：
    #   - 平时用 PrintWindow（PW_RENDERFULLCONTENT，D3D 的内容也截得到），不受其他窗口遮挡
    #   - 按过键时 PrintWindow 拿到的是 D3D 交换链上一次呈现之前的画面（菜单还没展开），只能从屏幕上取；
    #     只在确认 MiniBIM 仍在前台时取，且只取它的窗口矩形，免得截到用户的其他窗口和桌面
    $r = New-Object MiniBimShot+RECT
    [MiniBimShot]::GetWindowRect($h, [ref]$r) | Out-Null
    $w = $r.R - $r.L; $hh = $r.B - $r.T
    $bmp = New-Object System.Drawing.Bitmap $w, $hh
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    if ($keysSent -and [MiniBimShot]::GetForegroundWindow() -eq $h) {
        $g.CopyFromScreen($r.L, $r.T, 0, 0, (New-Object System.Drawing.Size $w, $hh))
    } else {
        if ($keysSent) { Write-Output 'MiniBIM 已不在前台，改用 PrintWindow：截图里可能没有按键的效果' }
        $hdc = $g.GetHdc()
        [MiniBimShot]::PrintWindow($h, $hdc, 2) | Out-Null
        $g.ReleaseHdc($hdc)
    }
    $bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    [MiniBimShot]::PostMessage($h, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null   # WM_CLOSE
    if (-not $p.WaitForExit(10000)) { $p.Kill(); Write-Output '程序 10 秒内没有退出，已结束进程' }
    Write-Output "截图：$out（$w×$hh），退出码 $($p.ExitCode)"
    if ($Keys -and -not $keysSent) { exit 2 }
} finally {
    if (-not $p.HasExited) { $p.Kill() }
    if ($hadSettings) { Copy-Item $backup $settings -Force; (Get-Item $settings).LastWriteTime = Get-Date }
    elseif (Test-Path $settings) { Remove-Item $settings }
}
