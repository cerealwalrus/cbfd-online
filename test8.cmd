@echo off
rem An 8 player online test on this PC: 8 windows, no launcher lobby (the game starts once all 8 are in).
rem Window 1 hosts (player 1), windows 2 to 8 join as players 2 to 8.
rem Windows 1 to 7 read the keyboard only while they are in front: click a window, then use the keyboard for that player.
rem Window 8 reads the gamepad. Windows 1 to 4 open in the four quarters of display 2 (CONKER_WINDOW_TILE), windows 5 to 7 in
rem three quarters of display 3, window 8 in the middle of display 1 (CONKER_WINDOW_DISPLAY: displays as Windows' Display settings
rem number them; they are listed in conker_displays.txt).
setlocal
set CONKER_NET_PLAYERS=8
set CONKER_NET_DELAY=4
pushd "%~dp0host\build-win"
start "P1 (host)" cmd /c "set CONKER_NET_HOST=47000&& set CONKER_NET_INPUT=keyboard&& set CONKER_NET_LOG=netplay_p1.txt&& set CONKER_WINDOW_DISPLAY=2&& set CONKER_WINDOW_TILE=1&& start "" ConkerOnline.exe"
timeout /t 4 /nobreak >nul
start "P2" cmd /c "set CONKER_NET_JOIN=127.0.0.1:47000&& set CONKER_NET_SLOT=2&& set CONKER_NET_INPUT=keyboard&& set CONKER_NET_LOG=netplay_p2.txt&& set CONKER_WINDOW_DISPLAY=2&& set CONKER_WINDOW_TILE=2&& start "" ConkerOnline.exe"
timeout /t 2 /nobreak >nul
start "P3" cmd /c "set CONKER_NET_JOIN=127.0.0.1:47000&& set CONKER_NET_SLOT=3&& set CONKER_NET_INPUT=keyboard&& set CONKER_NET_LOG=netplay_p3.txt&& set CONKER_WINDOW_DISPLAY=2&& set CONKER_WINDOW_TILE=3&& start "" ConkerOnline.exe"
timeout /t 2 /nobreak >nul
start "P4" cmd /c "set CONKER_NET_JOIN=127.0.0.1:47000&& set CONKER_NET_SLOT=4&& set CONKER_NET_INPUT=keyboard&& set CONKER_NET_LOG=netplay_p4.txt&& set CONKER_WINDOW_DISPLAY=2&& set CONKER_WINDOW_TILE=4&& start "" ConkerOnline.exe"
timeout /t 2 /nobreak >nul
start "P5" cmd /c "set CONKER_NET_JOIN=127.0.0.1:47000&& set CONKER_NET_SLOT=5&& set CONKER_NET_INPUT=keyboard&& set CONKER_NET_LOG=netplay_p5.txt&& set CONKER_WINDOW_DISPLAY=3&& set CONKER_WINDOW_TILE=1&& start "" ConkerOnline.exe"
timeout /t 2 /nobreak >nul
start "P6" cmd /c "set CONKER_NET_JOIN=127.0.0.1:47000&& set CONKER_NET_SLOT=6&& set CONKER_NET_INPUT=keyboard&& set CONKER_NET_LOG=netplay_p6.txt&& set CONKER_WINDOW_DISPLAY=3&& set CONKER_WINDOW_TILE=2&& start "" ConkerOnline.exe"
timeout /t 2 /nobreak >nul
start "P7" cmd /c "set CONKER_NET_JOIN=127.0.0.1:47000&& set CONKER_NET_SLOT=7&& set CONKER_NET_INPUT=keyboard&& set CONKER_NET_LOG=netplay_p7.txt&& set CONKER_WINDOW_DISPLAY=3&& set CONKER_WINDOW_TILE=3&& start "" ConkerOnline.exe"
timeout /t 2 /nobreak >nul
start "P8" cmd /c "set CONKER_NET_JOIN=127.0.0.1:47000&& set CONKER_NET_SLOT=8&& set CONKER_NET_INPUT=gamepad&& set CONKER_NET_LOG=netplay_p8.txt&& set CONKER_WINDOW_DISPLAY=1&& start "" ConkerOnline.exe"
popd
