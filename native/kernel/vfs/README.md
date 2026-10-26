# native/kernel/vfs - the game's file system and the ARK reader, natively (Phase 2.7, K.3)

`gh2k_vfs` (plain C++17, links only zlib) is the host side of the four kernel calls the title uses for files. `ark.cpp` reads the player's own
`gen/main.hdr` + `main_0.ark`. The hooks that connect it to the recompiled game are `native/rt/hooks/hooks_file.cpp` and `vfs_bridge.cpp`.

## What the game does with files (measured, `GH2_VFS=shadow`)
| scene | NtCreateFile | NtReadFile | NtQueryInformationFile | what |
|---|---|---|---|---|
| title (40 s) | 5 | 155, 12.5 MB | 4 | `main.hdr` (2 opens, 5 reads, 0.5 MB), `main_0.ark` (1 open), `videos\intro.wmv` (2 opens) |
| intro (38 s) | 5 | 278, 32.3 MB | 4 | the same files; the video is 27.7 MB of reads |
| Quick Play song (115 s) | 46 (5 game files + 41 save/content) | 876 (835 game files) | 44 | `main_0.ark` 798 reads, 52 MB: songs are `.mogg` inside the ARK |
- Only d:\ paths are game files. `save:\gh2save.dat` and `cnt0000000N:\config\*.dta` (content) are not: they stay with the runtime.
- Every read is asynchronous in the runtime (STATUS_PENDING, completed within microseconds); 211 reads pass an event, none an APC; offsets are always absolute (one read per run uses the file position).
- The only information class asked for is 34 (FILE_NETWORK_OPEN_INFORMATION: the size).
- The game decrypts `main.hdr` in place the moment its read completes; a comparison made a few hundred microseconds later sees the decrypted buffer (found by the shadow mode, which now waits for the completion inside the hook).

## Modes (`--vfs=sdk|shadow|native`, env `GH2_VFS`, report `GH2_VFS_REPORT=<json>`)
- `sdk` (default, unchanged behaviour): the runtime serves everything.
- `shadow`: the runtime serves; every open is repeated natively, every read is repeated at the same offset and compared byte by byte when the runtime reports completion, every size query is compared.
- `native`: NtCreateFile / NtReadFile / NtQueryInformationFile / NtClose on a path under d:, game: or \Device\Cdrom0 that exists, is opened for reading and is not a directory are answered by `gh2k::Vfs`
  (handles from 0x7E000000, completion is synchronous: the io status block is filled, the event signalled through the runtime's NtSetEvent). Everything else is forwarded.

## Results
- Shadow, title x3 + intro + song: 0 byte mismatches, 0 status mismatches, 0 size mismatches in 1 589 compared reads (127 MB: title x3, intro, song).
- Native, title / intro / song: all game-file reads served natively (155 / 278 / 833), 60.1-60.3 fps, same frame counts as the runtime's.
- ARK: `gh2_vfs_selftest GAME_DIR MANIFEST` parses `main.hdr` (version 3, encrypted with the Park-Miller stream) in 8.6 ms: 7 912 entries, 2.11 GB, 0 compressed; `tools/ark_manifest.py` produces the same
  manifest (path, offset, size, crc32 of every entry's bytes) from ark_extract.py's reader and **the two files are identical** (reading and checksumming every entry natively: 1.6 s).

## Not done (kernel step, still ahead)
Threads, mutexes, events, KPCR/spin locks, memory (K.1/K.2), content/save files and the STFS reader for DLC, directory enumeration (the game does not use it), writes. `XamInputGetState` and the audio callback still run on the runtime's threads.
