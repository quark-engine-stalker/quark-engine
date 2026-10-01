@echo off
setlocal EnableExtensions
pushd "%~dp0" || exit /b 1

set "DYNASM=%~dp0..\dynasm\dynasm.lua"
set "LJLIB=lib_base.c lib_math.c lib_bit.c lib_string.c lib_table.c lib_io.c lib_os.c lib_package.c lib_debug.c lib_jit.c lib_ffi.c"

if /I "%~1"=="x64" (
  set "DFLAGS=-D WIN -D JIT -D FFI -D P64"
) else (
  set "DFLAGS=-D WIN -D JIT -D FFI"
)

REM Always rebuild the host tools when this custom build runs. Reusing host
REM binaries from a previous LuaJIT revision can silently corrupt artifacts.
echo [LuaJIT] Building host\minilua.exe
cl /nologo /c /O2 /Ob3 /Oi /Ot /Oy /GT /W3 /fp:precise /MD /GF /GS- /Zi /D_CRT_SECURE_NO_DEPRECATE /Fominilua.obj host\minilua.c
if errorlevel 1 goto :fail
link /nologo /OPT:REF /OPT:ICF /out:host\minilua.exe minilua.obj
if errorlevel 1 goto :fail

echo [LuaJIT] Generating versioned luajit.h
host\minilua.exe host\genversion.lua luajit_rolling.h luajit_relver.txt luajit.h
if errorlevel 1 goto :fail

echo [LuaJIT] Generating host\buildvm_arch.h for x64 VM
host\minilua.exe "%DYNASM%" -LN %DFLAGS% -o host\buildvm_arch.h vm_x86.dasc
if errorlevel 1 goto :fail

echo [LuaJIT] Building host\buildvm.exe
cl /nologo /c /O2 /Ob3 /Oi /Ot /Oy /GT /W3 /fp:precise /MD /GF /GS- /Zi /D_CRT_SECURE_NO_DEPRECATE /I "." /I "..\dynasm" host\buildvm.c host\buildvm_peobj.c host\buildvm_lib.c host\buildvm_asm.c host\buildvm_fold.c
if errorlevel 1 goto :fail
link /nologo /OPT:REF /OPT:ICF /out:host\buildvm.exe buildvm.obj buildvm_peobj.obj buildvm_lib.obj buildvm_asm.obj buildvm_fold.obj
if errorlevel 1 goto :fail

echo [LuaJIT] Generating VM object and headers
host\buildvm.exe -m peobj -o lj_vm.obj
if errorlevel 1 goto :fail
host\buildvm.exe -m bcdef -o lj_bcdef.h %LJLIB%
if errorlevel 1 goto :fail
host\buildvm.exe -m ffdef -o lj_ffdef.h %LJLIB%
if errorlevel 1 goto :fail
host\buildvm.exe -m libdef -o lj_libdef.h %LJLIB%
if errorlevel 1 goto :fail
host\buildvm.exe -m recdef -o lj_recdef.h %LJLIB%
if errorlevel 1 goto :fail
host\buildvm.exe -m vmdef -o jit\vmdef.lua %LJLIB%
if errorlevel 1 goto :fail
host\buildvm.exe -m folddef -o lj_folddef.h lj_opt_fold.c
if errorlevel 1 goto :fail

echo [LuaJIT] Generation complete
popd
exit /b 0

:fail
set "BUILD_ERROR=%ERRORLEVEL%"
popd
exit /b %BUILD_ERROR%
