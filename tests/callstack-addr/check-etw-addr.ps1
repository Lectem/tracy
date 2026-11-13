# Checks the stored addresses of the frames of etwtest.exe against its disassembly (dumpbin /disasm), used by run-etw.bat.
#  - a frame that has a user callee must be stored at the last byte of a call instruction
#    of its function, that is the return address minus one;
#  - the last user frame of a stack (the interrupted instruction) must be stored at the start of an instruction of its function.
# Addresses are compared modulo 64K: the image is loaded at a multiple of 64K, and the disassembly is at the preferred base.
# usage: check-etw-addr.ps1 dis.txt stacks-addr.txt     (stacks-addr.txt: tracy-stackdump -a output)
param( [string]$Dis, [string]$Stacks )

$funcs = @{}            # function label -> list of @{ Start; End; IsCall }
$label = $null
foreach( $l in Get-Content $Dis )
{
    if( $l -match '^(\S.*):$' ) { $label = $Matches[1]; $funcs[$label] = New-Object System.Collections.ArrayList; continue }
    if( $label -and $l -match '^\s+([0-9A-F]{16}): ((?:[0-9A-F]{2} )+)\s*(\S+)' )
    {
        $start = [Convert]::ToUInt64( $Matches[1], 16 )
        $len = ( $Matches[2].Trim() -split ' ' ).Count
        [void]$funcs[$label].Add( @{ Start = $start; End = $start + $len; IsCall = ( $Matches[3] -eq 'call' ) } )
    }
}

$names = 'main', 'caller', 'work', 'tiny', 'sleeper'
$okCall = @{}; $badCall = @{}; $okLeaf = @{}; $badLeaf = @{}
foreach( $line in Get-Content $Stacks )
{
    if( $line -match '^#' ) { continue }
    $frames = ( $line -replace ' \d+$', '' ) -split ';'
    for( $i = 0; $i -lt $frames.Count; $i++ )
    {
        if( -not ( $frames[$i] -match '^0x([0-9a-f]+):(.+)$' ) ) { continue }   # note: -notmatch does not fill $Matches
        $addr = [Convert]::ToUInt64( $Matches[1], 16 ); $name = $Matches[2]
        if( $names -notcontains $name ) { continue }
        $label = if( $name -eq 'main' ) { 'main' } else { $funcs.Keys | Where-Object { $_ -match "^\?$name@@" } | Select-Object -First 1 }
        if( -not $label ) { continue }
        $key = "{0}:{1:x}" -f $name, ( $addr -band 0xFFFF )
        # a user frame followed by kernel frames is the interrupted instruction of the user part of the stack (a separate event)
        $isLeaf = ( $i -eq $frames.Count - 1 ) -or ( $frames[$i+1] -match '^0xffff[89a-f]' )
        if( -not $isLeaf )
        {
            $hit = $funcs[$label] | Where-Object { $_.IsCall -and ( ( $_.End - 1 ) -band 0xFFFF ) -eq ( $addr -band 0xFFFF ) }
            if( $hit ) { $okCall[$key] = 1 } else { $badCall[$key] = 1 }
        }
        else
        {
            $hit = $funcs[$label] | Where-Object { ( $_.Start -band 0xFFFF ) -eq ( $addr -band 0xFFFF ) }
            if( $hit ) { $okLeaf[$key] = 1 } else { $badLeaf[$key] = 1 }
        }
    }
}

$fail = 0
"caller frames stored at the last byte of a call instruction: {0} ok, {1} wrong" -f $okCall.Count, $badCall.Count
"leaf frames stored at the start of an instruction:           {0} ok, {1} wrong" -f $okLeaf.Count, $badLeaf.Count
if( $badCall.Count -gt 0 ) { "FAIL callers not at the last byte of a call: " + ( $badCall.Keys -join ' ' ); $fail = 1 }
if( $badLeaf.Count -gt 0 ) { "FAIL leaves not at an instruction start: " + ( $badLeaf.Keys -join ' ' ); $fail = 1 }
if( $okCall.Count -eq 0 ) { "FAIL no caller frame of etwtest was checked"; $fail = 1 }
if( $okLeaf.Count -eq 0 ) { "FAIL no leaf frame of etwtest was checked"; $fail = 1 }
if( -not $fail ) { "PASS stored addresses match the disassembly" }
exit $fail
