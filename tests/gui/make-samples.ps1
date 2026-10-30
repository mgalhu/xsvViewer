# Writes the synthetic files the GUI tests open. Everything here is generated: no real data.
param([Parameter(Mandatory)] [string] $Out)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $Out | Out-Null
$utf8 = New-Object System.Text.UTF8Encoding($false)
function Save($name, $text) { [IO.File]::WriteAllText((Join-Path $Out $name), $text, $utf8) }

$rand = New-Object System.Random 42

# Typical measurement table: text, integers, floats, NA, negative values, a long free-text column.
$sb = New-Object Text.StringBuilder
[void]$sb.Append("sample_id`tgroup`tcount`tlog2fc`tp_value`tscore`tdate`tnote`n")
$groups = 'control', 'treated', "Länge-Äpfel", 'high dose'
for ($i = 1; $i -le 120; $i++) {
    $lfc = if ($i % 11 -eq 0) { 'NA' } else { ($rand.NextDouble() * 8 - 4).ToString('0.000000', [Globalization.CultureInfo]::InvariantCulture) }
    $p = ([Math]::Pow(10, -$rand.NextDouble() * 12)).ToString('0.######e+00', [Globalization.CultureInfo]::InvariantCulture)
    $score = ($rand.NextDouble() * 1000).ToString('0.00', [Globalization.CultureInfo]::InvariantCulture)
    $note = if ($i % 7 -eq 0) { 'a considerably longer free text note that will not fit into the default column width at all' } else { "note $i" }
    [void]$sb.Append("s$i`t$($groups[$i % 4])`t$($rand.Next(0, 200000))`t$lfc`t$p`t$score`t2026-$('{0:00}' -f (1 + $i % 12))-15`t$note`n")
}
Save 'sample.tsv' $sb.ToString()

# R write.table with row names: the header is one field short.
Save 'r_rownames.tsv' "geneA`tgeneB`tgeneC`ndonor1`t1.5`tNA`t-2.25`ndonor2`t0.5`t3.75`t1.0`ndonor3`t2.25`t0.125`t-1.5`n"

# German spreadsheet export: semicolons and decimal commas.
Save 'german.csv' "Name;Größe;Wert`nMüller;1,75;1.234,50`nHuber;1,80;2.345,75`nÖztürk;1,65;987,25`n"

# VCF-like preamble with a #-prefixed header line.
Save 'variants.vcf.tsv' "##fileformat=VCFv4.2`n##source=synthetic`n#CHROM`tPOS`tID`tREF`tALT`n1`t101`trs1`tA`tG`n2`t202`trs2`tC`tT`n"

# Quoting: delimiters, doubled quotes and line breaks inside cells; ragged rows.
Save 'quoted.csv' "id,comment,value`r`n1,""contains, comma"",10`r`n2,""she said """"hi"""""",20`r`n3,""two`r`nlines"",30`r`n4,short`r`n5,""too"",many,fields`r`n"

# A wide table such as a gene expression matrix.
$sb = New-Object Text.StringBuilder
[void]$sb.Append('gene')
for ($c = 1; $c -le 3000; $c++) { [void]$sb.Append("`tcell$c") }
[void]$sb.Append("`n")
for ($r = 1; $r -le 200; $r++) {
    [void]$sb.Append("g$r")
    for ($c = 1; $c -le 3000; $c++) { [void]$sb.Append("`t$(($r * $c) % 97)") }
    [void]$sb.Append("`n")
}
Save 'wide.tsv' $sb.ToString()

# Windows-1252 rather than UTF-8, as older Excel exports are.
[IO.File]::WriteAllBytes((Join-Path $Out 'ansi.csv'), [Text.Encoding]::GetEncoding(1252).GetBytes("Name;Ort`r`nÄnne;Wien`r`nBernd;Graz`r`nÖmer;Linz`r`n"))

# Edge cases.
Save 'empty.csv' ''
Save 'one_column.csv' "only`nsingle`ncolumn`n"
Save 'header_only.csv' "a,b,c`n"
Save 'plain.txt' "This is a plain text file.`nIt must stay ordinary text.`n"
Save 'garbage.csv' "`"unterminated`n,,,,`n`t`t`n;;;`n|||`n"

Get-ChildItem $Out | ForEach-Object { '{0,-20} {1,10}' -f $_.Name, $_.Length }
