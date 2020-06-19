# Blinks color picker

Lets you play with the color on a Blinks tile.

Load the sketch on four tiles. Have one tile in the center with the other three around it. Once in this configuration the tiles will detect it and set themselves up.

The center tile is the palette showing the current color. The other tiles control the parameters (RGB or HSB).

When first launched, the application is in RGB mode. Each parameter is assigned one of the three colors as shown by the red, green, and blue wedges touching the palette tile.

Click one of the parameter tiles to change that parameter on the palette. Double click to change it by 4x the normal amount. Triple click to toggle the parameter between "increment" and "decrement" modes. The parameter will switch between bright and dim to show the current state.

Triple click the palette tile to toggle between RGB and HSB.

While in HSB, the parameters will show a color wheel (H), a tile going from red to white (S), and a fully white tile (B). Again, clicking or double clicking these will change the given parameter. Each click is guaranteed to have a "real" difference in the equivalent RGB value.

If something goes wrong or there's a bug, detach all four tiles from one another and then reassemble them. They should reset themselves.

You can get the RGB values out by viewing the binary value on each parameter tile. The wedge CW from the one touching the palette is the LSB. There are five bits shown, which represent the top five bits of the R, G, or B value (the bottom three bits are don't cares).

Currently there is no way to get an HSB value out other than converting it to RGB.
