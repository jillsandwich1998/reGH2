# gh2render - the native renderer (Phase 3)

`gh2render::Renderer` is the `gh2gpu::Sink` of `--gpu=native` (the default): the command processor (`../gpu`) reports shader loads, draws, resolves and the swap, the renderer turns
them into Vulkan (`../vk`) with the shaders of the shader cache (`../shader_cache`, SPIR-V made by XenosRecomp). No SDK header, no `rex::` (checked by `native/tools/check_isolation.py`).
The EDRAM is not emulated: the game draws straight into host images.

## How a frame goes (FACTS: read from the running game, traces in `work/analysis`)
1. The game's D3D library writes PM4 packets; `gh2gpu` consumes them and keeps the register file. `IM_LOAD` tells which microcode is active; the D3D hooks (`rt/hooks/gpu_renderer.cpp`)
   tell the renderer which shader container each microcode address belongs to (hash) and the container's vertex element table (`RegisterShader`).
2. **Draw** (`draw.cpp`): vertex shader and pixel shader modules come from the cache by hash; the vertex input is built from the *patched* microcode's vertex fetches (`ScanFetches`) matched
   with the container's element table (usage -> location, as in XenosRecomp); indices and vertices are read from guest memory, endian-swapped in ordinary memory, and copied to the per-frame
   upload ring (write-combined: never read back). RECTLIST is expanded on the host (fourth vertex = v1 + v2 - v0), QUADLIST becomes triangles; point lists are skipped.
   Constants (256 float4 for each stage + a "shared" block: texture/sampler heap indices, booleans, NDC transform, vertex index base) live in the ring and reach the shaders through device
   addresses (push constants: three addresses). Textures and samplers are bindless heaps (`device.cpp`).
3. **Render targets and resolves** (`targets.cpp`): each (EDRAM base, format) is a frame-sized image; formats that alias the same bits (2_10_10_10 and its 10_10_10_10 view) share one. The game
   splits a frame into two tiles (predicated tiling): draws carry absolute screen coordinates, so each tile lands in its half of the full image. A resolve copies the scissor rectangle into an
   image keyed by the destination address minus the tile offset; a texture fetch of that address samples the image (no round trip through guest memory). Clears that ride on the resolve
   (RB_COPY_CONTROL bits 8/9) are applied after the copy. The swap presents the resolved image of the front buffer address, letterboxed.
4. **Textures** (`textures.cpp`): decoded from guest memory (tiled 32x32 blocks, endian modes, DXT1/3/5, 8-bit, 8888, 2_10_10_10, ...), 2D and cube. **Mips** (FACT, measured on the game's textures): level 1 starts at the
   fetch constant's mip address, each level follows the previous one padded to whole 32x32-block tiles, and the levels whose smaller side is 16 texels or less share one packed tail (not read; the image ends before it
   and the last stored level answers further minification). Uncompressed textures get the levels that are not stored from the GPU (blit chain). Content is re-hashed once per frame (every level) to notice rewrites.
5. **Shaders the library patches and shaders it builds** (`draw.cpp`, `renderer.cpp`): the D3D library delivers most vertex shaders with `IM_LOAD_IMMEDIATE` (microcode inline in the stream, patched to the draw's vertex
   declaration; ~10k/s) - the renderer identifies them by a hash of the microcode's shape, keeps the stored microcode and pairs each vertex fetch with a container element by destination register (the library reorders
   fetches). The library's own clear/rect shaders (no container) are replaced by GLSL stand-ins (`builtin/`). **Dependent vertex fetches** (patch 0004): a fetch whose index register the shader computed itself
   (multi-mesh draws: the note gems, some effects - 14 of 377 vertex shaders) reads its stream through a device address described in the shared constants (`80 + 4 * element`), the cache entry says which elements.

## Destination swizzle of vertex fetches (`draw.cpp`)
The D3D library rewrites each vertex fetch's destination swizzle per vertex declaration; the translated shader was made with the container's. Two cases mattered (**measured**): every D3DCOLOR vertex colour (format 8_8_8_8) arrives with swizzle `0x60A` (z y x w) against the container's `0x688` - ignoring it swapped red and blue of every vertex colour (the blue hit flames that should be orange, cyan outlines) - and a fetch the declaration does not have is switched off with an all-constant swizzle (e.g. `0xB24` = 0, 0, 0, 1): the host used to read whatever bytes sat at its offset (for the drum kit that was the first dword of the position, as tangent: streaks and a pixelated logo on the bass drum). Now the uploaded data is rearranged component by component (8/16/32-bit formats, dependent streams too) when the live swizzle differs, and constants-only fetches become a stride-0 binding holding (0/1) values. `RENDER fetch swizzle differs:` logs each new case once.

## Internal resolution (`--render_resolution`)
`Config::render_height` (cvar `render_resolution`: `720p` default = the console's image, `900p`, `1080p`, `1440p`, `2160p`, any `<height>p` (16:9), or `auto` = the monitor's height in borderless fullscreen / the window's otherwise). Every render target and resolved snapshot is the guest's size times `scale = height / 720` (any real factor, not only integers: 1.25 = 900p, 1.5 = 1080p, 2 = 1440p, 3 = 2160p); the guest keeps working in its 1280x720 pixels: viewports, scissors and resolve rectangles are scaled when they reach Vulkan (`ScaleX/ScaleY`, one rounding rule, so the two 640-wide tiles meet without a seam), texture addressing of resolves stays in guest pixels, and window-space draws are mapped with `2 / 1280`. The window opens at the internal resolution when windowed (an explicit `--window_width/--window_height` wins; the window manager may clamp it to the desktop) and the picture is letterboxed into whatever size the window has. **Measured** (RX 6600 XT, 4x/2x MSAA as the game asks, virtual player): 1080p GPU 6-8 ms per frame, vulkan memory 570 MB; 1440p 7.5-10.8 ms, 850 MB; both 60.0 fps with a 17 ms worst interval. Not scaled and unknown: the one translated pixel shader that reads the pixel position (`iPos`) and shaders that offset by whole texels (`tfetch ... offset / dimensions`); nothing visibly wrong in 1080p song frames so far.

## On-screen menu and runtime options (`overlay.cpp`)
The application records Dear ImGui draw lists (`OverlayFrame`: vertices, uint16 indices, draws with texture id and clip rectangle) and calls `OverlaySubmit`; the latest frame is drawn at every swap straight into the swapchain image (dynamic rendering, alpha blend, push constants 2/width,2/height), after the letterboxed blit, so text stays sharp whatever the internal resolution. Textures (`OverlayCreateTexture`, RGBA8) are uploaded on the render thread through the frame ring; id 0 is a white texel. `Config::on_present` is called after every swap (the app schedules its UI frame). `SetOptions` (height, msaa, anisotropy floor) is applied in `BeginFrame` between two frames: a new height destroys every target and resolved snapshot (vkDeviceWaitIdle; heap indices of the old snapshots are not reused). `GH2_RENDER_DUMP_FINAL=1` adds `<prefix>_<frame>_final.ppm`: the presented window with the menu. FACT: validation layer shows no error from any of this.

## GPU profile and clears (`GH2_RENDER_PROFILE`)
`GH2_RENDER_PROFILE=1` writes a timestamp at every render pass, resolve and clear boundary and prints, with the statistics line, the GPU ms per frame by label (`pass s<samples> color <base>/f<format> depth <base>`, `resolve color|depth s<n>`, `resolve clears`); `=2` also marks every draw (label = vertex/pixel shader pair; the draws of the library's clear shader show as vs 00000001). `GH2_RENDER_PROFILE_SLOW=<ms>` (default 14) prints the breakdown of each frame above it (after frame 1500, 40 lines). The timestamps drain the pipeline, so labels distort: judge totals with the unprofiled `GPU/frame` of the statistics line. FACTS at 2160p (RX 6600 XT): every full-image clear of a 4x MSAA image is costly, and the game clears targets it never draws into over and over (~34 per frame); a clear that would change nothing is dropped (`TargetImage::clear_state`: 1 = cleared to `clean_value` and no pass has begun on it since); every other clear is issued at once, in the game's order (`GH2_RENDER_EAGER_CLEAR=1` restores the old no-skip behaviour to compare): peaks 17.7 -> 13-15 ms. LESSON: a first version made clears lazy (issued at the image's next use) and broke the crowd (a pass already open on the same target skipped the pending clear: black squares); do not defer clears. MSAA costs the remaining peaks (off: max 11-12 ms), hence `msaa=auto` (off from 2160p). Keeping the targets in GENERAL layout was measured and is much worse (no DCC). The depth snapshots (2 of them) are really sampled by draws (`depth snapshot` lines), so the depth resolves (~2-3 ms at 4K) cannot be skipped.

## Multi-tap shaders at a scaled resolution (`GenerateSnapshotMips`)
Blur / bloom shaders read a snapshot with several taps (>= 2 texture fetches of the same fetch constant, counted by `ScanFetches`) one GUEST texel apart. At scale S that is S of our texels, and a bilinear tap covers only 2x2, so gaps appear: vertical stripes, ghost copies and hard halos around characters at 2160p (S=3), barely at 1080p, none at 720p. Colour snapshots made at S > 1 therefore have `1 + ceil(log2 S)` levels; a multi-tap read generates the chain on demand (box blits, once per resolve, `mips_valid`) and uses a sampler with `minLod = maxLod = log2 S`, so each tap covers the footprint of one guest texel. Every other read of such a snapshot is clamped to level 0 (its other levels are not always current). `GH2_RENDER_NO_TAP_LOD=1` restores the old behaviour for comparison. Found with the F9 screenshots (`~/gh2_shots`).

## Translucent effects and the mip level at a scaled resolution
Sparkle/glow quads are textures whose borders rely on the mip level the console picks at 1280x720; at scale S the GPU picks a sharper level and hard square edges showed around the star power sparkles at 2160p (user report, F9 prints). Samplers used by translucent draws (blend other than source one / destination zero) get `mipLodBias = log2(S)` (`GH2_RENDER_EFFECT_LOD=0` turns it off); opaque textures keep their sharpness. `GH2_RENDER_LOD_BIAS=<x>` biases every game sampler (experiments). The cause (residual alpha at the quad border in mip 0) is a hypothesis; the user saw the artifact go away with the bias.

## Sample modes vs. GPU samples
The EDRAM base is shared by every sample mode (the crowd is drawn at 4x into a base other passes use at 1x), so a render target image is keyed by the GAME's mode (`TargetSamples`: 1/2/4) and clears are replayed between modes. What the GPU draws with is `HostSamples` (the game's count, or 1 when multisampling is off): never key an image by it, or the 4x crowd target and the 1x passes become one image (measured: crowd drawn over the picture as blocks, venue Battle of the Bands, `msaa=off`). Changing the option destroys the targets.

## Pipeline cache (`pipelines.cpp`)
`pipeline_cache.bin` (the driver's VkPipelineCache blob) and `pipelines.list` (every pipeline the game has asked for: shader hashes + state, in order of first use) live in `$GH2_PIPELINE_CACHE_DIR`, else
`$XDG_CACHE_HOME/gh2`, else `~/.cache/gh2` (`GH2_PIPELINE_CACHE=off` disables both). At start a low-priority thread builds the listed pipelines while the game boots; a draw that needs one takes it from there (or waits for
the few ms left when the thread is just building it), an unlisted one is built by the draw as before and joins the list. The thread saves both files every 15 s and at exit. **Measured** (RX 6600 XT, RADV, Quick Play with the virtual
player, driver shader cache disabled to mimic a first run): longest frame in the song-load window 493 ms and 930 pipelines built by draws (712 ms in all, longest 19.7 ms) without the cache; 67 ms with it (pipelines built by draws: 78-147
per run, 1-7 ms in all). With the driver's own disk cache warm (the normal case on a machine that has run the game) both are already close (66-70 ms): RADV makes a repeated pipeline nearly free, the cache is what a first run, a driver
update or a new venue need. The list grows to the union of what the runs used (1385 entries after five runs; a run uses 570-680).

## MSAA (`targets.cpp`)
The game renders with 2x and 4x MSAA (RB_SURFACE_INFO bits 16-17: the main scene is 2x, the crowd impostors clear at 4x). Render targets are keyed by (EDRAM base, format, samples); pipelines carry the sample count. A resolve of a multisampled
colour target is `vkCmdResolveImage` into the snapshot (the average, as the Xenos does); of a depth target a render pass with a SAMPLE_ZERO resolve attachment. `GH2_MSAA=off` renders everything at 1 sample. GPU time per frame went from 4-5 ms to 5-6.5 ms.
**EDRAM aliasing:** the game clears a target with 4x MSAA (a quarter of the pixels to touch) and then renders into the same EDRAM bytes at 1x; with one image per sample count that clear never reached the images used afterwards, so the 8 crowd
impostors were drawn over the leftovers of earlier frames (the zebra wall and the mini copies of the downsample chain). A clear drawn by the library's clear shader therefore also wipes, with the same colour/depth/stencil, the images the
same base has at the other sample counts (`clears replayed` in the statistics).

## Alpha test
The translated shader discards when `alpha - threshold < 0`, i.e. it implements GREATER_EQUAL. The game's cut-outs (crowd billboards, sprites on the loading screen) use GREATER with reference 0: pixels of alpha exactly 0 survived and showed as black rectangles. For
function 4 the threshold is now `ref + 0.5/255` (the next float up is a denormal, which the GPU flushes to zero).

## Shader patch (native/third_party/patches/0003)
The translated vertex shaders get two things the Xenos hardware did around the shader: (1) `oPos.xy = oPos.xy * g_NdcScale + g_HalfPixelOffset * oPos.w` - the renderer sets (1, -1) / 0 for draws that
use the viewport transform and (2/width, -2/height) / (-1, 1) for draws with window-space positions (PA_CL_VTE_CNTL scale/offset off: all 2D); **measured**: with the registers' viewport every picture
(FMV, logos) came out mirrored vertically, and the mirrored winding made the game's back-face culling remove the whole interface, so y is negated. (2) `r0.x` = vertex index
(`SV_VertexID + g_VertexIndexBase`): skinned meshes pick their bone set with it (without it the characters came out as shards). Patch 0004 (dependent vertex fetch, see 5 above): the recompiler marks a fetch dependent when an earlier statement wrote the index register's component, emits `vertexFetchDependent(element, index)`
(decodes 8/16/32-bit, float, half, 2_10_10_10 like Vulkan would read the bytes) and leaves the element out of the shader's inputs; the mask is column `dep_mask` of `index.tsv` and `dependent_mask` of the pak entry.
Changing a patch means regenerating the cache (`native/tools/shader_pipeline.py --stages recompile,validate,cache,selftest`, ~10 s).

## Debugging (environment variables; all off by default)
`GH2_VK_VALIDATION=1` Khronos validation (slows the renderer a lot: judge speed without it). `GH2_RENDER_DUMP=<prefix>` writes the shown frame as PPM every `GH2_RENDER_DUMP_EVERY` frames from
`GH2_RENDER_DUMP_FROM`, at most `GH2_RENDER_DUMP_MAX`; `GH2_RENDER_DUMP_TARGETS=1` also every render target and resolved image; `GH2_RENDER_NOCLEAR=1` keeps the targets as drawn.
`GH2_RENDER_DUMP_TEX=<prefix>` raw decoded textures. `GH2_RENDER_LOG=1` (+ `GH2_RENDER_LOG_FROM=<frame>`) logs draws with vertices, constants and texture slots; `GH2_RENDER_LOG_LAYOUTS=1` every distinct
vertex layout; `GH2_RENDER_LOG_NEAR=1` textures that miss a neighbouring resolved image. `GH2_RENDER_ONLY="frame:first-last,..."` draws only that range of the frame's issued draws (clears and post-processing passes always run, tile draws outside the range are skipped; the dump of frame N shows the draws of
frame N-1); `GH2_RENDER_ONLY_VS="frame:hexsuffix[+hexsuffix],..."` the same by vertex shader hash suffix (this is how the note gems were isolated). `GH2_RENDER_LOG_FRAME=<n>` (+`_SPAN`) one line per draw of a frame;
`GH2_RENDER_LOG_VS=<hash suffix>` (+`GH2_RENDER_LOG_VS_N=<index count>`) the first draws of one vertex shader with attributes, constants and textures. `native/tools`: `gh2_input_selftest play HZ SECS DELAY` (virtual guitar that
strums random frets) to exercise the effects of hitting notes in a test. `GH2_RENDER_ONLY_STRICT=1` applies the ranges to every draw (the render-to-texture passes too); `GH2_RENDER_HITCH_MS` (default 20) the renderer CPU above which a `RENDER hitch:` line says where a long frame went; `GH2_RENDER_DUMP_ALPHA=1` dumps 8888 images as their alpha;
`GH2_RENDER_DUMP_TEX_FMT=<n>` limits `GH2_RENDER_DUMP_TEX` to one Xenos texture format; `GH2_TEX_NOMIPS=1` reads only the top level of every texture; `GH2_MSAA=off`, `GH2_PIPELINE_CACHE=off`, `GH2_PIPELINE_CACHE_DIR` as above.
`GH2_RENDER_DEBUG=<bits>`: 1 no culling, 2 no depth/stencil, 4 no blending, 16 flat magenta pixel shader,
64 only draws with a viewport transform, 128 pixel shaders show texture slot 0, 512 show pixel constant c5.y / texture alpha. The periodic `GPU T+..s render:` line has the counters, the frame-time
histogram, renderer CPU per frame, GPU time per frame (timestamps) and the draw cost by phase. **Never take desktop screenshots to look at the game: use the dumps.**

## Known gaps (FACT unless marked)
- No 3D textures (a fetch of one reads the fallback texel and is logged once): none is requested between the intro and the end of a Quick Play song (measured: fallback reads 0, 313 2D + 3 cube textures), so it is not implemented yet. The packed mip tail (levels of
  16 texels or less) of DXT textures is not read either (the last stored level answers; a 128x128 texture keeps 128/64/32). No video-memory eviction.
- Point lists are skipped; polygon offset is parsed but not applied; alpha-to-mask, dual-source and per-RT blend are ignored; `RB_COPY_DEST_INFO` swap/exponent bias are ignored (the round trip is assumed to be the identity).
- The venue is dark with coloured spots (HYPOTHESIS that this is the game's own look: no reference frame of the original to compare).
  Earlier darkness and the shard-like characters were wrong shaders/vertex layouts (IM_LOAD_IMMEDIATE ignored, fetch reordering, unset textures), and the corrupt notes were dependent vertex fetches - not missing light passes.
- Pipelines: see "Pipeline cache" above. What is left of the load hitches (50-70 ms around the start of a song) is texture work (132 textures in one frame: images, views, staging), not pipelines.
- The SDK's F2 menu and overlays are not drawn in this mode.
- Tests: `gh2_render_selftest` (tiling, endian, level decoding), `gh2_gpu_selftest` (command processor).
