# Constantes relativas dos vertex shaders e teste rápido

03/10/2026. Continua `DESEMPENHO-2026-10-02.md`.

## O que o perfil por par mostrou

Perfil ao vivo no Showdown Town (parado, 401 MHz, fetchspec ligado, três
capturas de `plume-pairprof` modo 2, ~5.270 desenhos por quadro). Todo VS que
busca vértices pelo input assembler cai num piso de ~5-5,6 µs por desenho nas
sombras e ~9-10 µs nos desenhos de cor, qualquer que seja o tamanho do shader
(132 ou 521 instruções do ir3). Só a família que combina fetch ao vivo com
leitura de constante por `a0`/`aL` fica acima do piso:

| VS | % do tempo de desenho | desenhos/quadro | µs/desenho |
| --- | --- | --- | --- |
| 5053F7DC (sombra) | 12,4 | 544 | 9,5 |
| A017E382 | 6,3 | 183 | 14,4 |
| 0869F744 | 3,6 | 129 | 11,8 |
| AA574274 | 2,5 | 90 | 11,8 |
| D0F3B9AD | 2,2 | 66 | 14,0 |

Somando o que cada um custa acima do piso da sua classe: ~4,4 ms de 41,9 ms
(~10% do tempo de desenho, escala da captura serializada). É um teto.

No dump de ir3 de 26/09 de um desses VS (`files/ir/76EB0224...`) o Turnip
promove os quatro blocos de 64 float4 inteiros para o arquivo de constantes
(const file 316 float4, ~4 KB por desenho), porque o índice relativo pode cair em
qualquer um, e o shader escolhe o bloco com um `switch` em
`g_VertexShaderConstant`.

## Mudança

- Gerador (`tools/shaders/renut-xenosrecomp.patch`): a leitura relativa de um
  vertex shader sai como `cRegRel(N + a0)` e o shader ganha a flag
  `RENUT_SHADER_FLAG_REL_CONSTANTS` (bit 30 de `specConstantsMask`). São 108 VS;
  o único pixel shader com leitura relativa continua como estava. O resto do
  HLSL é idêntico byte a byte (4.781 arquivos).
- `shader_common.h`: `g_VertexShaderConstantRel(index)` escolhe pela constante de
  especialização 12 (`g_SpecRelConstants`): 0 o `switch` de antes, 1 o arquivo
  inteiro como um bloco (binding 10, 256 float4), 2 o mesmo bloco pelo binding 11,
  declarado com 4.096 float4 para o driver não poder promovê-lo ao arquivo de
  constantes e carregar cada leitura (`ldc`). O índice é limitado a 255 nos três.
- Host (`plume_gpu.cpp`): set 4 passa de 10 para 12 bindings. Um desenho cujo VS
  tem a flag, com o modo ligado, coloca os 4 KB do arquivo de constantes do VS
  contíguos no anel (só copia se mudou) e liga os bindings 10 e 11 nesse offset;
  a constante 12 entra na chave do pipeline. Desenhos sem a flag mantêm o último
  offset, então não religam o set. Desenhos de patch (domínio) ficam no modo 0.
- Chave ao vivo `files/plume-relconst` (0, 1, 2) ou o cvar
  `renut_plume_rel_constants`. Padrão 0: nada muda até alguém ligar.

## Teste rápido

`tools/quick-ab.py` alterna a chave com o jogo parado e em primeiro plano,
espera o número de desenhos do perfil se estabilizar (os pipelines novos
compilam em segundo plano), captura o perfil por par duas vezes por estado e
imprime os µs por desenho de cada VS da família e o total, mais uma comparação de
screenshots. Cerca de um minuto por estado, sem janelas de FPS:

    python tools/quick-ab.py                          # plume-relconst 0,1,2,0
    python tools/quick-ab.py --file plume-fetchspec --modes 0,1,0

O segundo `0` no fim é a repetibilidade: se o primeiro e o último estado 0
divergirem mais que o efeito, a medida não vale.

Armadilhas descobertas: o binário `verify` do script de shaders não lê o blob
que recebe, ele grava em `argv[2]` o cache embutido nele (não rodar em
`generated/shaders`); o `/tmp` do WSL é apagado quando a VM fica ociosa, então a
saída da regeneração vai para `/mnt/c/...`; a regeneração usa um núcleo só
(~14-16 min).

## Resultado (03/10/2026, Showdown Town, aparelho SM-S918B)

Medido nos mesmos quadros, desenho a desenho (`tools/quick-ab.py --interleave 1,2`;
o host alterna o modo 0 com o modo 1 ou 2 a cada desenho de um mesmo VS e marca o
hash do VS alternado no perfil por par). Cena parada, 6.100 desenhos por quadro,
3 capturas por modo, dispersão entre capturas de ~1 ponto:

| modo | família de 108 VS (µs por desenho) | por VS |
| --- | --- | --- |
| 1 (um bloco promovível) | **+8,1%** (pior) | +6,6 a +21,6% |
| 2 (um bloco grande demais para promover) | **-27,1%** | -21,5 a -33,1% |

No modo 2 o 5053F7DC vai de 8,7 para 5,8 µs por desenho (o piso das sombras é
5,1-5,6) e os VS de cor de ~12 para ~9 µs (o piso da classe é 9-10). Ou seja, a
família volta ao piso. O modo 1 piora porque promove os 4 KB inteiros e ainda
mantém as faixas estáticas dos quatro blocos: mais bytes no arquivo de constantes
por desenho. O que custa é a quantidade de constantes que o processador de
comandos carrega por desenho, não o `switch`.

Ciclos por quadro (`tools/quick-fps.py`, janelas de 20 s, A B B A, praça pesada
a ~15 fps): modo 0 18,30 Mcycles/quadro, modo 2 14,30 (-21,8%; as janelas de cada
estado diferem 0,3-0,5). O FPS subiu menos (15,6 para 16,3) porque o governador da
GPU baixou o clock de ~290 para ~237 MHz no modo 2.

Imagem: a diferença entre o modo 0 e o 2 (1,32% dos pixels acima de 8/255, média
absoluta 0,454) é a mesma que entre dois screenshots do mesmo modo com segundos de
intervalo (1,33%, 0,466). As constantes são os mesmos bytes e o índice é limitado
a 255 nos dois caminhos.

Padrão do cvar `renut_plume_rel_constants` agora é 2. Limites: medido numa cena e
com o clock da GPU em 520-660 MHz nas capturas do perfil; áreas com peles e
personagens diferentes (Jolly Joe, água, o carro do jogador) ainda não foram
conferidas a olho.

Pista seguinte: se o custo é o volume de constantes promovido por desenho, o
mesmo vale para os outros blocos grandes (pixel shaders com arquivo de constantes
de 92-96 float4, o bloco compartilhado). Testar com o mesmo método.
