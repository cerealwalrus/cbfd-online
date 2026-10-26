Menu skin: your own pictures for the launcher (kept on your computer, not in the repository).

Put PNG files here (with transparent edges where the picture isn't a rectangle) and restart the program. Anything missing is
skipped; with none of them the launcher keeps its flat wooden look.

  plank.png           behind each launcher option (with the background picture it's only used for the planks the picture lacks; it should include
                      the plank's dark outline and shadow, a few pixels outside the plank). It is stretched to the option's box, so make it wide and short
                      (about 1000 x 170 pixels works well).
  plank_selected.png  the same, for the option that is selected (the mouse is over it, or the controller is on it).
  arrow.png           a small arrow shown at the left of the selected option (about 80 x 80 pixels).
  title.png           over the top of the screen, centred, about 46% of the screen's width, keeping its shape.
  plates/             with background.mp4: plate_<name>.png and plate_<name>_selected.png for each option, in the launcher's
                      order (singleplayer, online, controls, settings, mods, exit), and plate_plain for Version and Add ROM: in place
                      of the planks, the lettering right of the plate's icon.
  logo.png            with background.mp4: the logo at the top (the background picture's, cut out of it), in place of the title.
  background.mp4      a video for the whole screen behind everything, played without its sound and looped (Windows: its own video
                      decoder plays it). With it background.png isn't used, and the options keep their planks (plank.png).
  chrome/             the title bar's and status bar's pieces (from the War-Room launcher kit): star.png, minimize.png and
                      minimize_hover.png, maximize(_hover).png and restore(_hover).png, close.png and close_hover.png (each 46 x 34 at 3x), lamp_green/yellow/red.png (the status
                      lamp with its glow), hazard.png (the yellow and black stripes over the status bar, 7680 x 18),
                      transmission.png and update.png (the update check's answer, over the bottom right corner). The lettering is
                      Share Tech Mono (assets/ShareTechMono-Regular.ttf). On Windows the title bar takes the place of the Windows one;
                      set CONKER_NATIVE_WINDOW=1 to keep the Windows title bar instead.
  background.png      the whole screen behind everything. A 4:3 picture (1448 x 1086) shown in the middle of the window (bars at the
                      sides on a wide screen). With it the launcher lays its options out on the picture's five planks: Start Game,
                      Online, Controls, Settings and Mods sit on them, Exit on a sixth plank added under them, and Version and Add
                      ROM become small buttons in the top-left corner. (The numbers for another picture are at the top of
                      host/src/menu_skin.cpp.)

The program reads them from the "assets/menu_skin" folder next to ConkerRecomp.exe. The build copies host/assets there, so
put them in host/assets/menu_skin and build, or copy them straight into the built folder's assets/menu_skin.

The log line "[skin] plank.png loaded" shows in the console for each file it found.
