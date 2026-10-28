@echo off
rem A 5 player online test on this PC: 5 windows, no launcher lobby (the game starts once all 5 are in).
rem Window 1 hosts (player 1), windows 2 to 4 join as players 2 to 4, window 5 joins as player 5.
rem Windows 1 to 4 read the keyboard only while they are in front: click a window, then use the keyboard for that player.
rem Window 5 reads the gamepad. Windows 1 to 4 open in the screen's four quarters (CONKER_WINDOW_TILE),
rem window 5 on display 3 as Windows' Display settings number them (CONKER_WINDOW_DISPLAY; the displays are listed in conker_displays.txt).
setlocal
set CONKER_NET_PLAYERS=5
set CONKER_NET_DELAY=4
pushd "%~dp0host\build-win"
start "P1 (host)" cmd /c "set CONKER_NET_HOST=47000&& set CONKER_NET_INPUT=keyboard&& set CONKER_NET_LOG=netplay_p1.txt&& set CONKER_WINDOW_TILE=1&& start "" ConkerOnline.exe"
timeout /t 4 /nobreak >nul
start "P2" cmd /c "set CONKER_NET_JOIN=127.0.0.1:47000&& set CONKER_NET_SLOT=2&& set CONKER_NET_INPUT=keyboard&& set CONKER_NET_LOG=netplay_p2.txt&& set CONKER_WINDOW_TILE=2&& start "" ConkerOnline.exe"
timeout /t 2 /nobreak >nul
start "P3" cmd /c "set CONKER_NET_JOIN=127.0.0.1:47000&& set CONKER_NET_SLOT=3&& set CONKER_NET_INPUT=keyboard&& set CONKER_NET_LOG=netplay_p3.txt&& set CONKER_WINDOW_TILE=3&& start "" ConkerOnline.exe"
timeout /t 2 /nobreak >nul
start "P4" cmd /c "set CONKER_NET_JOIN=127.0.0.1:47000&& set CONKER_NET_SLOT=4&& set CONKER_NET_INPUT=keyboard&& set CONKER_NET_LOG=netplay_p4.txt&& set CONKER_WINDOW_TILE=4&& start "" ConkerOnline.exe"
timeout /t 2 /nobreak >nul
start "P5" cmd /c "set CONKER_NET_JOIN=127.0.0.1:47000&& set CONKER_NET_SLOT=5&& set CONKER_NET_INPUT=gamepad&& set CONKER_NET_LOG=netplay_p5.txt&& set CONKER_WINDOW_DISPLAY=3&& start "" ConkerOnline.exe"
popd
