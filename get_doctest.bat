@echo off
set DEST=tests\doctest.h
set URL=https://raw.githubusercontent.com/doctest/doctest/master/doctest/doctest.h
echo Downloading doctest to %DEST% ...
powershell -Command "Invoke-WebRequest -Uri '%URL%' -OutFile '%DEST%'"
echo Done. Re-run cmake to pick up the test target.
