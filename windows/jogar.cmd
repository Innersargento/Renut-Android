@echo off
rem Abre o reNut (Plume) com o renut.toml e o save de out\win-plume-data.
rem Ajuste GAME para a pasta do jogo (a que tem default.xex e Bundle).
set GAME=D:\ROMS\Banjo-Kazooie - Nuts ^& Bolts (Europe) (En,Ja,Fr,De,Es,It,Nl,Sv,No,Zh,Ko,Pl,Ru,Cs)
"%~dp0..\out\build\win-plume\renut_plume.exe" --game "%GAME%" --data "%~dp0..\out\win-plume-data"
