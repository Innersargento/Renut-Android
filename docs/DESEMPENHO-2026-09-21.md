# Plume — onde o quadro realmente gasta, e o que otimizar

21/09/2026. Samsung SM-S918B, Adreno 740, Turnip v26.3.0-R5, renderizador Plume,
Showdown Town, com cooler. Continua `Relatorio-Plume-resolves-2026-09-21.md`
(a pergunta aberta dos resolves) e os `DESEMPENHO-*-2026-09-20.md`.

Todas as medições abaixo são A/B **dentro da mesma sessão**: câmera parada,
`plume-skip` alternado por adb a cada 35-45 s, clock da GPU amostrado a cada
segundo. Números por quadro. Commits `3e7cdda`, `ab8c2bb` e o deste relatório.

---

## 1. Resumo

Numa área pesada (~6.600 desenhos, ~50 ms, 20 FPS):

| parte | GPU/quadro | como foi medido |
| --- | --- | --- |
| tudo | ~50 ms | — |
| sem draw calls (só estado) | 7,8 ms | bit `1073741824` |
| **custo fixo por desenho** | **~32 ms** (~4,8 µs × 6.600) | 3 vértices/draw − sem draws |
| **vértices** | **~10 ms** | normal − 3 vértices/draw (bit `2147483648`) |
| pixels (fragmento) | ≤ 2 ms | scissor 1x1 (bit `8388608`) |
| banda de textura | ~0 | texturas brancas (bit `16777216`) |
| resolves + clears + cópias | ≤ 3,3 ms | bits `33554432`, `134217728`, `268435456` |

**A CPU está empatada.** A thread `GPU Commands` gasta ~42-50 ms por quadro nessa
área; com a GPU aliviada (3 vértices/draw) o quadro parou em ~46 ms, preso nela.
Para 30 FPS aqui é preciso cortar **os dois lados**.

## 2. O que foi descartado com medição (não repetir)

- **Estrutura de passes.** O Turnip roda o Plume em **sysmem** (automático = `sysmem`
  forçado; `gmem` forçado custa 20-30% a mais). `LOAD_OP`/`renderArea` não importam.
- **Barreiras.** Barreiras precisas por layout e barreiras agrupadas (336 → 276 por
  quadro): zero efeito nas duas áreas. Revertidas.
- **Resolves.** Remover todos economiza no máximo 3,3 ms (7%).
- **Pixels e texturas.** fp16, formatos de textura, overdraw, LRZ: nada a ganhar aqui.
- **"entre-passes 88%"** do instrumento por pass era enganoso.

## 3. Contadores de uma área pesada

Do relatório de quadro (`pipeline_binds=` etc., novos) e de um trace de um quadro:

- 6.600 desenhos, **3.700 trocas de pipeline** (3.400 trocas consecutivas de par de shaders)
- **6,2 milhões de vértices**; 402 draws com ≥ 3.000 vértices somam 61%
- **água: 4 draws, 1,32 milhão de vértices (21% do total)**, um deles com 653.568
- índices por faixa de vértices ≈ 1,06: quase nenhum reaproveitamento de vértice
- 1 draw com geometry shader, 44 quad lists — irrelevantes

## 4. O que otimizar, em ordem

### A. Água: parar de gerar 21 MB por quadro na CPU (fácil, alto retorno nos dois lados)
`IssueDraw` expande cada patch em nível 16 (`kMaxPatchLevel`, o teto que o jogo
permite, não o fator real por aresta) e **escreve na CPU, todo quadro**, 4 floats
por vértice: 1,32 M vértices ≈ 21 MB no ring. A grade é idêntica para todo patch
do mesmo nível — só o índice do patch muda.
- Guardar a grade num buffer persistente por `(nível, patches)` e reusar: zera o
  custo de CPU por quadro. `IssueDraw` próprio é 12% da thread de gravação.
- Em seguida, testar nível menor (8 → 4× menos vértices) e ver se a água muda
  visivelmente. É a maior fatia isolada dos ~10 ms de vértices.
- Draw não indexado: cada vértice compartilhado roda o shader 6×. Indexar a grade
  corta as invocações.

### B. Rehash de texturas (fácil, só CPU)
`GetGuestTexture` recalcula o XXH3 de **toda textura usada, inteira, com todos os
mips, uma vez por quadro**: ~8,5% da thread de gravação, mais ~5% de busca no
mapa. Checar cada textura a cada N quadros, escalonado por chave, corta a maior
parte. Risco: textura escrita pela CPU (vídeo, UI) com até N quadros de atraso —
testar a interface e as cenas de vídeo.

### C. Custo fixo por desenho, ~32 ms de GPU (médio, o maior prêmio)
~4,8 µs por desenho com 3 vértices. 3.700 trocas de pipeline por quadro são o
suspeito principal: cada troca de programa carrega os shaders recompilados
(grandes) e roda o preâmbulo. Próximos passos, nesta ordem:
1. **Medir:** um bit que reusa o pipeline anterior quando o layout de vértices é
   compatível (tela errada) — diz quanto dos 32 ms são trocas de pipeline.
2. Se for isso: ordenar ou agrupar não é possível (ordem do jogo), então reduzir
   o custo por troca — shaders menores, menos variantes por chave de pipeline,
   conferir se o Turnip está recompilando/re-carregando por estado dinâmico.
3. Se não for: olhar o que o Turnip emite por draw (`TU_DEBUG` / trace de comandos).

### D. CPU da gravação, o resto (médio)
Perfil com símbolos, área pesada, thread `GPU Commands` (86% do tempo em trabalho):

| item | % da thread |
| --- | --- |
| `IssueDraw` próprio (inclui a geração da água) | 12,2 |
| XXH3 em `GetGuestTexture` (item B) | ~8,5 (+ ~5 busca) |
| `CopySwapped` (troca de bytes de vértices/índices) | 7,8 |
| `WriteRegister` + parsing de pacotes | ~14 |
| `CommitConstants` (`memmove` + `memcmp`) | ~6,5 |
| driver Turnip | ~11 |
| XXH3 do microcódigo em `LoadShader` | ~2,8 (+1,8) |

`LoadShader` rehasheia o microcódigo a cada chamada; cachear por endereço + tamanho
é barato.

## 5. Instrumentos

`plume-skip` (relido a cada 30 swaps): `33554432` sem resolves · `134217728` sem
clears · `268435456` sem cópias · `536870912` sem `CopyDepth` · `8388608` scissor
1x1 · `16777216` texturas brancas · `1073741824` sem draw call · `2147483648`
3 vértices por draw. Relatório de quadro: `barriers=`, `pipeline_binds=`,
`vertices=`, `gs_draws=`, `patch_draws=`, `patch_vertices=`. Launcher: opção
TU_DEBUG (desativado / perf / sysmem / gmem).

O jogo roda no processo `org.renut.android:runtime`, não no principal — `top`,
`simpleperf -t` e `ps -T` têm que apontar para ele. Para símbolos, guardar o
`librenut_runtime.so` de `app/build/intermediates/cxx/Debug/*/obj/arm64-v8a/`
da mesma build que está instalada e usar o `simpleperf` de host do NDK com
`--symfs`.
