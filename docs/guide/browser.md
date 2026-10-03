# Browser

The browser, on the left of the window, lists built-in devices, plug-ins and the audio
files in your folders. Search it, preview files, and drag or double-click items into
the project. **Ctrl+Alt+B** (*View › Browser*) shows or hides it.

## Categories and places

The sidebar has two sections:

- **Categories**: *All* (built-in devices, plug-ins and samples at once), *Samples*
  (the audio files under your places), *Built-in* (*Instruments*, *Audio Effects*) and
  *Plug-ins* (*Instruments*, *Audio Effects*; see [plugins.md](plugins.md)).
- **Places**: folders of your own, which you browse as a folder tree. The browser starts
  with your Music folder as a place. Add more with *Add Folder…* (at the end of the
  list, or in the right-click menu); right-click a place › *Remove from Places* to take
  it away.

The files listed are WAV, FLAC and MP3 files.

## Search

- Searching is instant. **Ctrl+F** (*Edit › Find in Browser*) searches everything
  (*All*); **Enter** (or Down) in the search field selects the first result, and Enter
  again adds it.
- An item matches when every word you type is in its name or its detail (folder,
  vendor, category), ignoring case.
- Searching 200 000 files takes about 10 ms and never holds up the window or the audio;
  see [benchmarks/README.md](../../benchmarks/README.md).

## Sorting

Lists sort by *Rank* or *Name*, chosen next to the search field:

- *Rank* puts what you add most, and most recently, first, then the best name matches.
  With nothing used yet, a search lists the names that start with what you typed first.
  An item counts as used when it is added to the project from the browser, by
  double-click, Enter or a drag that is dropped somewhere. The counts are kept in
  `%LOCALAPPDATA%\SUBstation\library.json`.
- *Name* is alphabetical.

## Preview

Click a file to preview it. The headphones button in the browser's footer turns preview
on or off. A preview stops when you click anywhere outside the browser.

## Adding to the project

- Drag and drop items, or double-click them (or press Enter), to add them: audio files
  become clips (set up from their names; see
  [audio-clips.md](audio-clips.md#tempo-and-key-from-file-names)), devices go on the
  selected track.
- Instruments (built-in or plug-in) go on a MIDI track, replacing its instrument. With
  no MIDI track selected, double-clicking one or dropping it below the tracks makes one.

## The index

- The files under the places are indexed in the background and the index is kept
  (`%LOCALAPPDATA%\SUBstation\browser-index.bin`), so the next start shows it at once and
  only looks again at folders that changed.
- Files added, removed or renamed in a place show up while the program runs.
- Results show while a first scan is still going.
- Right-click › *Rescan* reads every folder again (for drives that don't report
  changes).
- The index goes 16 folders deep and up to 300 000 files a place, and skips names
  starting with `.` or `$`; it follows junctions but not symbolic links.

---

For developers: [../browser.md](../browser.md).
