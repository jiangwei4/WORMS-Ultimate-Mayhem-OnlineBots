@echo off
rem WUM-Mod : WUMMod.asi = version finale (sans journal), WUMMod_log.asi = version de diagnostic (journal wummod_log.txt).
cd /d "%~dp0"
set GCC=C:\MinGW\bin\gcc
set F=-std=gnu99 -O2 -Wall -Wno-unused-parameter -Wno-unused-function -shared -s -static-libgcc
%GCC% %F% -DWUM_NOLOG -o WUMMod.asi wummod.c
%GCC% %F% -o WUMMod_log.asi wummod.c
