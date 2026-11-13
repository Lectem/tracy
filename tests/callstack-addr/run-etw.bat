@echo off
REM Checks the ETW stacks of the callstack address contract (Windows). Run from an ELEVATED prompt.
REM Builds etwtest.cpp (x64, /O2) with the client of this tree, samples it for 10 s with tracy-capture, dumps the stacks,
REM then checks the names, and the stored addresses against the disassembly of etwtest.exe (check-etw-addr.ps1).
REM Stacks of a thread blocked in Sleep (wait stacks) are stored with the samples on Windows, there is no separate context switch list.
setlocal
net session >nul 2>&1 || (echo Run this from an elevated prompt, ETW sampling needs admin. & exit /b 2)
set "HERE=%~dp0"
set "ROOT=%HERE%..\.."
set "OUT=%TEMP%\tracy-etw-test"
set "DUMP=%HERE%stackdump\build\Release\tracy-stackdump.exe"
if not exist "%OUT%" mkdir "%OUT%"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 2
cmake -S "%ROOT%\capture" -B "%ROOT%\capture\build" >nul && cmake --build "%ROOT%\capture\build" --config Release >nul || (echo capture build failed & exit /b 2)
cmake -S "%HERE%stackdump" -B "%HERE%stackdump\build" >nul && cmake --build "%HERE%stackdump\build" --config Release >nul || (echo stackdump build failed & exit /b 2)
pushd "%OUT%"
cl /nologo /Zi /O2 /EHsc /std:c++20 /DTRACY_ENABLE /I"%ROOT%\public" /I"%ROOT%\public\tracy" "%HERE%etwtest.cpp" "%ROOT%\public\TracyClient.cpp" /link /DEBUG /INCREMENTAL:NO /OUT:etwtest.exe dbghelp.lib ws2_32.lib advapi32.lib user32.lib >nul || (echo test build failed & popd & exit /b 2)
dumpbin /disasm /nologo etwtest.exe > dis.txt
start "" /b etwtest.exe
"%ROOT%\capture\build\Release\tracy-capture.exe" -f -s 10 -o etw.tracy >nul
taskkill /f /im etwtest.exe >nul 2>&1
"%DUMP%" -n 100000 etw.tracy > stacks.txt
"%DUMP%" -a -n 100000 etw.tracy > stacks-addr.txt
set FAIL=0
findstr /r /c:";main;caller;work [0-9]*$" stacks.txt >nul && echo PASS main;caller;work is sampled || (echo FAIL no sample with stack main;caller;work & set FAIL=1)
findstr /r /c:";tiny [0-9]*$" stacks.txt >nul && echo PASS tiny is sampled || (echo FAIL tiny never sampled as a leaf & set FAIL=1)
findstr /r /c:";neighbor [0-9]*$" /c:"\[unknown\] [0-9]*$" stacks.txt >nul && (echo FAIL neighbor or [unknown] is a sampled leaf & set FAIL=1) || echo PASS no sample attributed to neighbor or [unknown]
REM stacks of the blocked thread: user frames above the kernel frames of the wait
findstr /r /c:";sleeper;SleepEx;NtDelayExecution" stacks.txt >nul && echo PASS wait stacks resolve sleeper;SleepEx;NtDelayExecution || (echo FAIL no wait stack with sleeper;SleepEx;NtDelayExecution & set FAIL=1)
findstr /c:"sleeper_next" stacks.txt >nul && (echo FAIL a frame is attributed to the never called sleeper_next & set FAIL=1) || echo PASS no frame attributed to the never called sleeper_next
findstr /r /c:"sleeper.*\[unknown\]" stacks.txt >nul && (echo FAIL [unknown] frame in a wait stack & set FAIL=1) || echo PASS no [unknown] frame in the wait stacks
REM stored addresses against the disassembly: callers at the last byte of their call, leaves at an instruction start
powershell -nop -ep bypass -File "%HERE%check-etw-addr.ps1" dis.txt stacks-addr.txt || set FAIL=1
echo.
echo Files: %OUT% (stacks.txt, stacks-addr.txt, dis.txt)
popd
exit /b %FAIL%
