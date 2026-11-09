# Mechanical CAD

The complete native SOLIDWORKS model is in [`solidworks/`](solidworks/). Open
`OBX-A000.SLDASM` as the top-level robot assembly. Keep all files in that
directory together so that assembly references can be resolved.

Neutral STEP exports are divided into:

- [`step/assemblies/`](step/assemblies/) - robot and module assemblies,
- [`step/parts/`](step/parts/) - manufactured parts.

`OBX-A006` is included as a SOLIDWORKS assembly and manufacturing drawing. No
separate STEP export for this assembly was supplied. Its manufactured plate
`OBX-P034` is available as STEP.

## Viewing the model with eDrawings

1. Download and install the free
   [eDrawings Viewer](https://www.edrawingsviewer.com/download-edrawings).
2. Clone or download the complete repository. Do not download only the top-level
   assembly file because its referenced parts must remain available.
3. In eDrawings select **File > Open** and open
   `hardware/cad/solidworks/OBX-A000.SLDASM`.
4. Use the assembly tree, hide/show controls, section view and explode controls
   to inspect component locations.
5. Open the corresponding assembly PDF from
   [`../drawings/assemblies/`](../drawings/assemblies/) for the mounting order,
   fastener quantities and detailed notes.

The STEP exports can also be opened in CAD applications that support AP214/AP242
STEP data. Dimensions and manufacturing requirements are defined by the PDF
drawings, not by measuring the screen image.
