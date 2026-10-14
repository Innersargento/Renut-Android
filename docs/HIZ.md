# Hi-Z: o que o renderizador deixa de desenhar

Implementado em 14/10/2026 (`src/plume/hiz_model.{h,cpp}`, a parte "Hi-Z" de
`src/plume/plume_gpu.cpp`, `src/plume/shaders/hiz_*.comp`). Chave de teste:
`files/plume-hiz` (relida a cada 30 quadros) ou o cvar `renut_plume_hiz`.

| Modo | O que faz |
| --- | --- |
| 0 | desligado |
| 1 | só conta o que poderia ser descartado |
| 2 (padrão) | descarta desenhos cuja caixa está toda fora do frustum ou do scissor |
| 3 | + oclusão pela profundidade que o passe já desenhou (desenhos de cor); **experimental, ver abaixo** |
| 4 | + oclusão dos desenhos só de profundidade (sombras); os com bias ficam de fora |

Tudo é exato: um desenho só some quando nada dele pode chegar ao alvo.

## Como funciona

1. **Modelo do vertex shader** (`hiz_model.cpp`). O microcódigo é executado
   simbolicamente uma vez por shader (e por conjunto de booleanos): a posição de
   clip tem que ser uma função afim de um elemento do vértice, com coeficientes
   que são expressões das constantes. Skinning, endereçamento por a0/aL, laços,
   predicação e comparações na posição invalidam o modelo (o desenho nunca é
   cortado). Na primeira vez, dois vértices reais passam pelo interpretador do
   SDK e têm que dar o mesmo resultado.
2. **Caixa** do elemento sobre os vértices que o desenho lê, em cache e
   invalidada por escrita do jogo (mesma vigilância dos streams guardados, mais
   uma amostra de conteúdo).
3. **Frustum/scissor** (modo 2): os 8 (ou 16) cantos da caixa pelas constantes do
   desenho; todos fora de um mesmo plano → o desenho cai antes de custar stream,
   constantes ou pipeline.
4. **Oclusão** (modo 3/4), dentro de uma janela de desenhos reordenáveis
   (`FlushDeferred`): os desenhos que o último teste achou escondidos ficam para
   o fim da janela; o resto é gravado normalmente. Depois o passe é fechado, a
   profundidade é copiada, reduzida a uma pirâmide (blocos de 8 a 128 px, o valor
   mais distante) e cada desenho com caixa é testado na GPU (retângulo + a
   profundidade mais próxima da caixa). Os segurados são gravados por último, cada
   um dentro de `VK_EXT_conditional_rendering` com a própria resposta. Nada chega
   com um quadro de atraso e nada pode estar errado por um quadro.
   O custo de um teste (quebra de passe + cópia + 6 dispatches) só é pago onde há
   desenho segurado; "aprendizado" em até 3 janelas por quadro, a cada 8 quadros,
   e a cada 120 se menos de 2% das respostas forem "escondido".

## O que se mediu (PC, cena do título, 300 quadros)

* colorido: 26% dos desenhos fora do frustum; 44% sem modelo (skinned etc.).
* só profundidade: 62% fora do frustum.
* oclusão: com todos os candidatos forçados (RENUT_HIZ_FORCE) e conferidos por
  consultas de oclusão (RENUT_HIZ_VERIFY): 0 violações em ~40 mil desenhos, e a
  imagem sai igual. Mas quase nenhum é respondido como escondido: ~99,8% dos
  desenhos sem amostras estão sobre profundidade ainda no plano distante quando a
  janela fecha, ou seja, não estão escondidos por profundidade (o jogo desenha
  muita coisa que não deixa um pixel por outros motivos). Os pares de shader mais
  frequentes saem na linha "Hi-Z draws with no samples" do log (só com VERIFY).

## Depuração

Variáveis de ambiente (PC): `RENUT_HIZ_FORCE=1` segura todos os candidatos;
`RENUT_HIZ_VERIFY=1` roda os segurados sem predicado, dentro de consultas, e conta
violações; `RENUT_HIZ_DEBUG=1` despeja o nível 0 da pirâmide de algumas janelas.
No Windows o arquivo de modo fica em `<dados>/plume-miss/plume-hiz`.

## Limites conhecidos

* Janelas são curtas (~13 desenhos, ~4 com caixa): o teste vê a profundidade do
  passe até ali, não a final.
* Desenhos skinned, instanciados pelo motor (c133) e com bias não têm caixa.
* A extensão `VK_EXT_conditional_rendering` é pedida na criação do dispositivo
  (`plume_renderer.cpp`); sem ela o modo 3 não faz nada.

## Medido no celular (Showdown Town, câmera parada, 14/10/2026)

Por quadro (média de 4 janelas de 300 quadros alternadas 0/2/3):

* Desenhos por quadro ~6.100 (3.600 de cor, 2.500 só de profundidade).
* Modo 2 descarta ~930 por quadro (15%): 448 de cor (12,5%) e 478 só de
  profundidade (19%); `drawn` cai de ~1.83 M para ~1.55 M por 300 quadros.
* GPU: 14,99 Mcycles/quadro desligado, 14,00 no modo 2 (-6,6%).
* Modo 3: 0 desenhos respondidos como escondidos em ~3.600 testes, e -4 FPS
  (20,8 contra 24,7): cada teste é uma quebra de passe e uma cópia de profundidade
  da tela inteira. Por isso o padrão ficou em 2; o modo 3/4 existe para
  experimento e o aprendizado se espaça (a cada 600 quadros) quando nada é achado.
* A thread do command processor está perto de 100% (cp_cpu ~14 s por 300 quadros),
  então descartar desenhos poupa GPU mas o FPS não sobe na mesma proporção.

## Medido de novo, com o corte barateado (celular, Showdown, câmera parada, 6 rodadas 0/2)

* 5.630 draws por quadro; o modo 2 descarta 779 (13,8%): 275 de cor e 504 só de
  profundidade. Pipeline binds por quadro 2.308 -> 2.015.
* CPU do command processor: 26,4 -> 23,5 ms por quadro (-11%), já contando o custo
  do corte (~2,5 ms por quadro, amostrado).
* GPU: 12,32 -> 11,82 Mcycles por quadro (-4,0%).
* FPS: 37,3 -> 41,9 (+12%), com o modo 2 à frente em todos os 6 pares.
* A medição anterior (antes de indexar as caixas por bloco e de trocar os 8 cantos
  por teste de planos) tinha dado FPS igual: o corte custava o que poupava. O
  clock estava mais alto desta vez (500-550 MHz contra ~350), então os números
  absolutos não são comparáveis entre as duas medições, só os pares dentro de cada
  uma.
