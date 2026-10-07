Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
$f = New-Object System.Windows.Forms.Form
$f.Text = "WHITE_TEST"
$f.BackColor = [System.Drawing.Color]::White
$f.StartPosition = [System.Windows.Forms.FormStartPosition]::Manual
$f.Location = New-Object System.Drawing.Point(700, 300)
$f.Size = New-Object System.Drawing.Size(900, 700)
$f.TopMost = $false
$f.ShowInTaskbar = $false
$t = New-Object System.Windows.Forms.Timer
$t.Interval = 30000
$t.Add_Tick({ $f.Close() })
$t.Start()
[System.Windows.Forms.Application]::Run($f)
