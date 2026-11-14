### Highlights of Version v0.26.7

A stability and correctness release: the Floaro Main Street crash is fixed at its root, the frame pacing is right at 30 FPS, and the field-of-view slider works for every effect.

> [!NOTE]
> ✅ **No game rebuild needed** when updating from v0.26.6: the game code is unchanged, so the start screen goes straight to PLAY.
>
> 🇧🇷 **Não é preciso recompilar o jogo** ao atualizar da v0.26.6: o código do jogo não mudou, então a tela inicial vai direto para o JOGAR.

#### 🛠️ Crash fixes
- **Crash and device loss in Jubilife's Floaro Main Street fixed** ([#7](https://github.com/fabioschmidt01/PokeL-ARecomp/issues/7), [#12](https://github.com/fabioschmidt01/PokeL-ARecomp/issues/12)):
  - **Cause:** the game re-used its command memory while lists recorded there were still waiting to be translated for the GPU. The translator then read garbage: impossible draws (an address as the instance count), a jump to address `0x70`, a crash in `guest_copy`, or a lost GPU device on slower CPUs.
  - **Why this port:** its records were ~5x the size of the Switch's own commands, and it reported that size to the game. So the game's command-memory allocator went round ~5x faster than it was built for.
  - **Fix:** command lists now live in the port's own memory until they are translated. The game is shown hardware-like sizes, so its allocator behaves as it does on the Switch.
  - **Tested:** sessions of 15–22 minutes with 0 crashes and 0 corrupt lists, with memory stable at ~55 MB.
- **Black screen after ~20 minutes fixed:** a list that ended up calling itself ran millions of draws per frame. It is fixed by the change above, and a per-list safety budget (`LARECOMP_EXEC_BUDGET`) now stops anything similar from freezing the game.
- **Safety net for corrupt lists:** a damaged command list, or a draw with impossible counts, is dropped with a line in the log (`[exec] a corrupt command list`, `[draw] skipped an impossible draw`) instead of crashing the game.
- **Crash on "Connect to the internet" fixed:** the Switch network service (`nifm`) is now implemented as an offline console. The game is told there is no connection instead of reading a missing object.

#### 🎞️ Frame rate & pacing ([#9](https://github.com/fabioschmidt01/PokeL-ARecomp/issues/9), [#11](https://github.com/fabioschmidt01/PokeL-ARecomp/issues/11))
- **30 FPS is the recommended mode and 60 FPS is marked experimental**, in the menu and in the README. At 60 FPS, parts of Pokémon Legends: Arceus count frames instead of time, so thrown food, menus, dialogue and some effects run twice as fast. These are long-standing limits of this game at 60 FPS (no 60 FPS patch has fixed them), not new bugs of this port. Play at 30 FPS to feed Pokémon.
- **Frame Generation now runs on top of 30 FPS:** 60 frames on screen while the game logic stays at the Switch's speed.
- **VSync "Off (Unlimited)" no longer speeds the game up:** at 30 FPS the game ran 1.5x faster in the field and up to 8x in menus. The frame rate is now always capped at the chosen 30 or 60 FPS.
- **New log lines for bug reports:** `[speed]` (game time vs real time, every 10 s) and `[settings] changed`.

#### 📷 Field of view slider, done properly
- **F1 > Video > Field of view** now widens the game's own camera, where its projection is built. Before, only one camera buffer was rescaled afterwards, so at FOV 90 torch fire, Ponyta's mane, Pokémon eyes and alert marks stayed where the 60° view had them.
- **Title screen:** its scene picture keeps its framing; it had shrunk to a small box.
- **Damage vignette:** it is stretched with the view and covers the screen again.
- To compare with the old behaviour: `LARECOMP_FOV_UBO=1`. Every camera's field of view is logged as `[fov] camera seen`.

#### 🖥️ Performance & compatibility
- **720p (Switch handheld) resolution option**, now the default of the *Light* profile, for the Steam Deck ([#5](https://github.com/fabioschmidt01/PokeL-ARecomp/issues/5)), integrated GPUs and entry-level cards.
- **CPU / GPU overlap is chosen automatically by CPU:**
  - It is off on CPUs with 8 threads or fewer (it was involved in the Floaro device loss on a 4-core / 8-thread CPU) and stays off on AMD GPUs.
  - Override it in **F1 > Advanced Video > Compatibility**, or with `LARECOMP_GPU_OVERLAP=1` / `0`.
  - With the crash fixed at its root, this rule may be relaxed once it is confirmed on more machines.
- **Measured:** on the test machine (Xeon E5-2650 v4 + Arc B580) the frame rate in Jubilife is limited by the CPU side of the game, not by the GPU or by command translation (idle 51% of the time). This guides the next performance work ([#10](https://github.com/fabioschmidt01/PokeL-ARecomp/issues/10)).

#### 🙏 Reports
Thank you to everyone who sent logs, saves and screenshots: the Floaro crash was found from a friend's save and logs. If something still crashes or looks wrong, please open an [issue](https://github.com/fabioschmidt01/PokeL-ARecomp/issues) with your `lasrecomp.log` and `larecomp.ini`.

---

#### 🇧🇷 Resumo em português
- **Crash na Floaro Main Street corrigido na raiz** ([#7](https://github.com/fabioschmidt01/PokeL-ARecomp/issues/7), [#12](https://github.com/fabioschmidt01/PokeL-ARecomp/issues/12)): o jogo reaproveitava a memória de comandos antes de ela ser traduzida para a GPU. Também foram corrigidos a tela preta depois de ~20 minutos e o crash ao clicar em *Conectar à internet*.
- **30 FPS recomendado e 60 FPS experimental** ([#9](https://github.com/fabioschmidt01/PokeL-ARecomp/issues/9), [#11](https://github.com/fabioschmidt01/PokeL-ARecomp/issues/11)). A Geração de Quadros roda sobre 30 FPS, e o modo *VSync Desligado (Ilimitado)* não acelera mais o jogo.
- **Campo de visão (FOV) corrigido:** efeitos, fogo, olhos, alertas, tela de título e vinheta de dano acompanham o FOV escolhido.
- **Nova resolução 720p (Switch portátil)** para Steam Deck ([#5](https://github.com/fabioschmidt01/PokeL-ARecomp/issues/5)) e placas mais fracas. A sobreposição CPU / GPU é escolhida automaticamente conforme o processador.
- **Não é preciso recompilar o jogo** ao atualizar da v0.26.6.
