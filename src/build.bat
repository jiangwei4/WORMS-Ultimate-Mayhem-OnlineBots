@echo off
rem WUM-Mod : version finale (sans log) et version de diagnostic (avec log).
rem MinGW 32 bits requis.
set GCC=gcc
%GCC% -std=gnu99 -O2 -Wall -shared -s -DWUM_NOLOG -o WUMMod.asi wummod.c -static-libgcc
%GCC% -std=gnu99 -O2 -Wall -shared -s -o WUMMod_log.asi wummod.c -static-libgcc
