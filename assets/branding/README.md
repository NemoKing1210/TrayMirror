# TrayMirror icon

Two monitors with matching notification dots represent mirroring the Windows
notification area onto secondary monitors. The artwork was generated with
OpenAI's built-in image generation tool for this project.

- `traymirror-icon.png`: original 1254 x 1254 RGBA artwork with transparency;
  also used in the repository README.
- `traymirror-icon.ico`: Windows application icon with 16, 20, 24, 32, 40, 48,
  64, 96, 128 and 256 pixel RGBA images. Windows selects the appropriate size
  for the display scale.

`resources/TrayMirror.rc` embeds the ICO as the default icon group in
`TrayMirror.exe`. No external image files are needed at runtime. The existing
application manifest is embedded separately by the build.

To regenerate the ICO after changing the PNG, run from the repository root
with Python and Pillow installed (asset maintenance only; neither is a build
dependency):

```powershell
python -c "from PIL import Image; im = Image.open('assets/branding/traymirror-icon.png').convert('RGBA'); im.save('assets/branding/traymirror-icon.ico', format='ICO', sizes=[(s, s) for s in (16, 20, 24, 32, 40, 48, 64, 96, 128, 256)])"
```
