# 3D printing parameters

The part drawings define the printing parameters. The three printed parts are:

- `OBX-P022` - bearing housing 1,
- `OBX-P023` - bearing housing 2,
- `OBX-P030` - pulley spacer.

Use the following profile for all three parts:

| Parameter | Value |
| --- | --- |
| Material | ABS or ASA |
| Infill | 100% |
| Layer height | 0.30 mm |
| Nozzle | 0.40 mm |
| Perimeters / walls | At least 4 |
| Nozzle temperature | 240-255 °C, according to the filament manufacturer |
| Bed temperature | 95-110 °C, according to the filament manufacturer |
| Cooling fan | 0%; up to 20% only for bridges |
| Print speed | 40-60 mm/s |
| First-layer speed | 15-20 mm/s |
| Bed adhesion | 8-12 mm brim |
| Enclosure | Required; keep the chamber closed and free from drafts |
| Supports | Only where required by geometry |

Dry the filament before printing and clean the build surface. Allow the part to
cool inside the closed enclosure before removal to reduce warping and cracking.
Do not reduce the drawing-specified infill or change the print orientation shown
on the `OBX-P022` and `OBX-P023` drawings.

Print threaded and bearing interfaces without support material where possible.
Remove supports completely and verify holes, bearing seats and mating surfaces
before assembly. Do not force a warped or cracked part into the robot.

The supplied neutral manufacturing files are STEP. Export STL files in
millimetres from the STEP geometry if the slicer does not accept STEP directly.
