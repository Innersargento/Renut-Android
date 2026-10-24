# Hi-Z: o que o renderizador deixa de desenhar

Implementado em 14/10/2026 (`src/plume/hiz_model.{h,cpp}` e `src/plume/gpu/hiz.cpp`).
Chave: o cvar `renut_plume_hiz` (lido a cada quadro; em builds de depuração dá
para trocá-lo com o jogo aberto, ver `docs/DESEMPENHO-2026-10-17.md`).

| Modo | O que faz |
| --- | --- |
| 0 | desligado |
| 1 | só conta o que poderia ser descartado |
| 2 (padrão) | descarta desenhos cuja caixa está toda fora do frustum, do scissor ou de um plano de recorte do usuário |

Tudo é exato: um desenho só some quando nada dele pode chegar ao alvo.

Os modos 3, 4 e 5 (oclusão pela profundidade, na GPU) foram **retirados em
17/10/2026**: medidos no celular, sempre custaram mais do que pouparam. O porquê
está em "Oclusão: por que não" abaixo; não tente de novo sem ler.

## Como funciona

1. **Modelo do vertex shader** (`hiz_model.cpp`). O microcódigo é executado
   simbolicamente uma vez por shader (e por conjunto de booleanos): a posição de
   clip tem que ser uma função afim de um elemento do vértice, com coeficientes
   que são expressões das constantes. Skinning, endereçamento por a0/aL, laços,
   predicação e comparações na posição invalidam o modelo (o desenho nunca é
   cortado). Na primeira vez, dois vértices reais passam pelo interpretador e
   têm que dar o mesmo resultado.
2. **Caixa** do elemento sobre os vértices que o desenho lê, em cache e
   invalidada por escrita do jogo (mesma vigilância dos streams guardados, mais
   uma amostra de conteúdo).
3. **Frustum/scissor/plano de recorte** (modo 2): a faixa de cada plano sobre a
   caixa, pelas constantes do desenho; a caixa toda do lado de fora de um mesmo
   plano → o desenho cai antes de custar stream, constantes ou pipeline.
   Os planos de recorte do usuário (`PA_CL_UCP_n`, ligados em `PA_CL_CLIP_CNTL`)
   entram desde 17/10: o shader guarda o que tem `dot(posição de clip, plano) >= 0`
   em todo plano ligado, então uma caixa inteira com distância < 0 a um deles não
   desenha nada. É o pass de reflexo em 1/4 de resolução (320x180) do jogo.
   Contador `clip_plane=` em `files/plume-hiz.txt`: ~14 desenhos de cor por quadro
   no Showdown.

## O que se mediu (PC, cena do título, 300 quadros)

* colorido: 26% dos desenhos fora do frustum; 44% sem modelo (skinned etc.).
* só profundidade: 62% fora do frustum.

## Medido no celular (Showdown Town, câmera parada, 14/10/2026)

* 5.630 draws por quadro; o modo 2 descarta 779 (13,8%): 275 de cor e 504 só de
  profundidade. Pipeline binds por quadro 2.308 -> 2.015.
* CPU do backend de GPU: 26,4 -> 23,5 ms por quadro (-11%), já contando o custo
  do corte (~2,5 ms por quadro, amostrado).
* GPU: 12,32 -> 11,82 Mcycles por quadro (-4,0%).
* FPS: 37,3 -> 41,9 (+12%), com o modo 2 à frente em todos os 6 pares.

## Por que um draw não tem modelo

`files/plume-hiz.txt` traz `nm_color=` e `nm_depth=` (10 causas, na ordem da enum
`renut::hiz::Why` a partir de 1: controle, laço, predicado, sem fetch,
instanciado, skinned, relativo, outro elemento, índice de vértice, não linear) e
o log lista os shaders sem modelo mais frequentes.

* **Predicado**: os shaders skinned. O predicado vem do peso do osso
  (`p0 = r0.y != 0`) e as matrizes de osso vêm por índice de vértice, então a
  posição não é afim em constantes. No Showdown: ~710 draws de cor e ~600 de
  sombra por quadro.
* **Instanciado** (a0 calculado do vertex id, constantes c133+, balanço com
  `sin()`): ~1.020 de cor e ~600 de sombra por quadro no Showdown.
* **Não linear**: era quase tudo `max(a, a)` escalar (o mov do montador);
  corrigido em 14/10.

Caixas para esses dois casos foram pesadas em 17/10 e **adiadas**: o censo
(abaixo) mostra que, dos desenhos sem amostras dessas famílias, no máximo ~250
por quadro estão fora da tela; o resto está escondido atrás de outra coisa, e
isso uma caixa de frustum não corta.

## Oclusão: por que não (17/10/2026)

O censo de desenhos sem amostras (`docs/DESEMPENHO-2026-10-17.md`) mostrou no
Showdown ~2.050 desenhos de cor por quadro (61%) que não deixam nenhum pixel, dos
quais ~1.260 por estarem escondidos por profundidade. Foi o motivo para tentar de
novo a oclusão. O que se aprendeu:

* **A passada de cor do jogo usa profundidade invertida** (`GEQUAL`, limpa em 0,
  valores 0..0,003). O teste original só aceitava `LESS/LEQUAL`, e as janelas dos
  mapas de sombra gastavam o orçamento de 12 testes por quadro: a passada de cor
  nunca tinha sido testada. A conclusão de 14/10 ("não estão escondidos por
  profundidade") estava errada por isso.
* Com o suporte a `GEQUAL` (pirâmide de 1 - profundidade) o teste ficou exato
  (0 violações em dezenas de milhares de desenhos verificados) e, testando todas
  as janelas, acha ~380 desenhos escondidos por quadro no Showdown. Mas:
  * **por janela** (modo 3): cada teste é uma quebra de passada, uma cópia de
    profundidade e uma pirâmide, para uma janela de ~13 desenhos. Testando tudo:
    FPS 43 -> 27. No ritmo normal (aprendizado): FPS 41,0 -> 37,5, GPU +1,9%, ~63
    desenhos poupados por quadro. O modo 3 também desligava a thread de gravação
    (`RecActive`), o que custava ~9% sozinho; isso foi corrigido antes da medida
    acima.
  * **por segmento** (modo 5: um teste por trecho de profundidade, segurando os
    desenhos do trecho): FPS 48,3 -> 32,3, GPU +40%, **0 escondidos**. A passada
    principal é um trecho só, que começa logo depois do clear: a profundidade do
    começo está vazia, e tudo o que esconde algo é desenhado dentro da passada.
* **No Turnip (Adreno 740), cada desenho sob `VK_EXT_conditional_rendering`
  custa ~5.500 ciclos de GPU**, mais que o dobro do próprio desenho (~2.300).
  Qualquer esquema exato precisa segurar o desenho sob predicado até a resposta,
  então nenhum pode pagar neste driver. O que sobraria seria a leitura da resposta
  na CPU, um quadro depois: não é exato (objetos podem aparecer com atraso), o que
  a regra do projeto exclui.

O código dos modos 3-5, os shaders `hiz_*.comp` e o pedido da extensão de
renderização condicional saíram junto.

## Perfil da thread de comandos (simpleperf, Showdown, câmera parada, 20/10)

Thread "GPU Commands", ~100% de um núcleo, ~4,4-4,7 µs por draw. Próprio (self) e
inclusive, % da thread:

* `IssueDraw` 12% próprio, 83% inclusive; `EmitDraw` + `FlushDeferred` 32%.
* Dentro do driver (Turnip): `setPipeline` 8%, `drawIndexedInstanced` 5,4%,
  `setIndexBuffer` 2,7%, `ioctl` 8%, e a maior parte do `memmove` (11%) é cópia
  de estado dentro dessas chamadas.
* `GuestStream` 4,2% + `BindStream` 1,3%; `GetGuestTexture` 3,7%; `HizBoxFor`
  3,9%; `HizCullDraw` inclusive 8,7-9,8%.
* Antes das otimizações deste dia: `DrainWatchHits` 3,6% (percorria todas as
  texturas e streams a cada escrita) e `UploadRepacked` 4,2% (copiava o stream
  de formato 17 a cada draw) e o hash de par de shader 1,3%: os três saíram do
  topo. Custo por draw na thread: 4,66 -> 4,38 µs (-6%).
