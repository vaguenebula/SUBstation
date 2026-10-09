# File Manager and hot swap

The File Manager lists every audio file your project plays, finds the ones that went
missing, and puts any file in another's place. Hot swap tries samples from the browser
in place of one, while the song plays. Both work as in Ableton.

## The File Manager

**Ctrl+Alt+F** (*View › File Manager*) shows or hides it, at the right of the window.
It lists the files your project plays: its audio clips' files and samplers' samples (in
racks too). Each row says the file's name, what plays it (*3 clips*, *Sampler*...) and
its folder. Missing files come first, in red; then the rest by name. Over the list it
says how many files there are, and how many are missing.

- Type in its field to filter the list by name or folder.
- **Double-click** a file (or press **Enter**) to select its clips in the arrangement.
- **Drop** an audio file onto a file (from the browser, or from Explorer) to replace it
  everywhere it plays ([below](#replacing-a-file)).
- Its **hot swap button** (⇄, at the right of the row) hot-swaps it
  ([below](#hot-swap)); it is lit while that hot swap runs, and a click ends it.
- **Right-click** a file for *Hot-Swap*, *Replace…*, *Locate…* (a missing file),
  *Select Clips*, *Find Similar Sounds* and *Show in Folder*.

Showing, it checks again which files are there (a drive plugged back in, a file put
back).

Frozen tracks' own audio (made when you freeze, see [mixing.md](mixing.md)) isn't
listed: SUBstation makes it.

## Missing files

A file is missing when it isn't where the project last found it: a sample folder
moved or renamed, a drive not plugged in, a project copied to another computer
without its samples. Its clips stay where they are, show *Missing file* and are
silent until it is found. When a project opens with missing files, the status line
says how many, and the File Manager shows.

A box over the list says how many files are missing:

- **Search** looks for them by name in the project's folder (and the folders in it)
  and in the browser's places (see [browser.md](browser.md)), in the background
  (**Cancel** stops it). What it finds is put in place in one undo step (*Locate
  Missing Files*), and the status line says how many it found.
- **Search Folder…** looks in a folder you choose (and in the browser's places).

Of several files with a missing file's name, the one whose folders match the missing
one's most closely wins (`Samples\Drums\Kick.wav` before `Downloads\Kick.wav`). One
file found also says where the others went: if `D:\Samples\Drums\Kick.wav` is found at
`E:\Samples\Drums\Kick.wav`, the other files missing from `D:\Samples` are looked for in
`E:\Samples`.

**Locate…** (a missing file's right-click menu) asks you where it is. The other files
missing from its folder, or from folders beside it, are then looked for where it went,
so locating one sample of a pack finds the rest.

A missing file found is the same audio: the clips playing it stay as they are, and
files on frozen tracks are found too.

## Replacing a file

**Replace…** (a file's right-click menu), or dropping an audio file onto it, puts
another file in its place everywhere it plays: every clip and sampler of the project,
in one undo step. Swap the kick of the whole song for another in one go.

Each clip stays where it is, with its settings: warp, transpose and detune, gain, pan,
fades, and whether it is deactivated. It takes the new file's name.

- A clip that played all of its file (a one-shot as you dropped it in) plays all of
  the new one: a longer kick makes a longer clip, cut where the next clip on its track
  starts.
- A clip that played a stretch of its file (a kick cut out of a loop) plays the same
  stretch of the new one, as far as the new file goes.
- A reversed clip plays the new file forwards: press **R** to reverse it again.

Clips and samplers on frozen tracks are left as they are (unfreeze them to replace
their files there); the status line says so.

## Hot swap

Hot swap links the browser to a sample in your project: the audio file you pick in the
browser plays in its place at once, so you hear it in the song, while it plays.

Start it from:

- an audio clip's right-click menu › **Hot-Swap Sample**: the clip's file, in every
  clip and sampler of the project that plays it;
- a file's **hot swap button** in the File Manager (or *Hot-Swap* in its menu): every
  clip and sampler that plays it.

The browser shows, and lists the **sounds most like it** at once (as *Find Similar
Sounds* does, see [browser.md](browser.md#find-similar-sounds); for a clip, like the
part of its file the clip plays). A bar over its list says what you are swapping
(*Hot-Swap Kick (4 clips)*), and its list takes the keyboard:

- **Click** a sample, or move through the list with **Up** and **Down**, to swap each
  one in. Any list works: the similar sounds, a search, a place's folder tree. Only
  a click you let go of where you pressed counts: pressing a sample to drag it
  swaps nothing.
- **Double-click** a sample, or press **Enter**, to keep it and end the hot swap.
- **Esc**, the bar's **✕**, or the File Manager's button again end it, keeping the
  sample swapped in last.
- The bar's **Similar** lists the sounds most like the sample swapped in now.

However many samples you try, a hot swap is one undo step: **Ctrl+Z** goes back to the
sample it began with. Trying that sample again leaves no undo step at all. While the
song plays, the samples you try aren't previewed as well: you hear them in the song.

Each try works like [replacing a file](#replacing-a-file), from the clips as they were
before the hot swap began: trying a long sample, then a short one, leaves no clip cut
short by the long one.

A hot swap ends as soon as you do something else, keeping the sample swapped in last:

- you click anywhere but the browser, the File Manager and the transport bar (so you
  can still play and stop the song while you try samples);
- you start dragging something out of the browser: what you drag is added where you
  drop it, never swapped in;
- you make any other edit, or undo (**Ctrl+Z** also takes back its samples);
- another project opens.

---

For developers: [../app/session.md](../app/session.md#the-file-manager-and-hot-swaps-filemanager-hotswap),
[../app/model.md](../app/model.md).
