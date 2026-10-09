# O passe de cor, desenho a desenho

03-04/10/2026. Continua `DESEMPENHO-2026-10-03.md`.

## Desfecho (05/10): revertido

O corte de desenhos vazios foi implementado, medido, instalado e **revertido por decisão
do usuário**: depois de jogar com ele, o ganho não era perceptível no FPS e apareceram
pop-ups. O código não foi commitado; o repositório voltou ao commit `41bda40` e os shaders
gerados ao estado de `out/shaders-rel`. O experimento inteiro (diff de `plume_gpu.cpp`, o
patch do gerador com a bandeira de visibilidade, `spirv_early_tests.h`, `vis_flag.frag`,
`quick-occ/ovr/trivial.py`) está guardado em `out/colour-pass-experiment/` (pasta fora do
git) para quem quiser retomar.

Por que não valeu a pena:

- Os ciclos de GPU caíam 12–26% **em cena parada**, mas o FPS quase não se mexia: o
  governador troca a folga por clock mais baixo (a GPU ficava 74–84% ocupada em vez de
  91–99%) e o limite do quadro passa a ser outro. Em movimento (o jogo quase todo, o
  veículo arrasta a câmera) a guarda desliga o corte, então não deve haver ganho; isso
  nunca foi medido, só a cena parada.
- A bandeira que diz "este desenho apareceu" chega com 2 quadros de atraso (dois
  quadros em voo), então um objeto que entra em cena aparece 2 a 5 quadros depois,
  ~300 ms a 15 FPS, e pode piscar. A auditoria média (poucos pixels por quadro) subestima
  o que o olho vê.
- O nome do desenho (shader, índices, ordem no quadro) se embaralha quando o jogo muda
  quantas instâncias envia. Uma assinatura de conteúdo (constantes, buffers, texturas,
  estado) por desenho resolveria isso e foi instalada na última versão; não deu tempo de
  medir, e o usuário preferiu parar.
- Conclusão do usuário: parece um limite técnico de desempenho. O que sobra é o custo
  fixo por desenho (~700 ciclos, 32% do quadro), que só cairia pulando desenhos; pular sem
  nenhum atraso exigiria visibilidade exata no mesmo quadro (volume envolvente por desenho
  e teste de oclusão na hora), que os comandos do Xenos não trazem.

O resto deste documento descreve o que foi medido e construído antes da reversão.

## Para onde vai o quadro

Showdown Town, mesma cena, ciclos de GPU por quadro (`tools/quick-fps.py`, que não
depende do clock que o governador escolhe):

| | Mcycles/quadro | |
| --- | --- | --- |
| normal | 15,10 | |
| pixel shader trivial, geometria rasterizada | 11,43 | PS ≈ 24% |
| VS e PS triviais, nada rasterizado | 8,38 | shaders + raster ≈ 44% |
| idem, sem chamada de desenho (só passes) | 2,63 | passes, resolves, clears ≈ 17% |
| idem, só o primeiro triângulo de cada desenho | 7,44 | vértices/primitivas ≈ 7% |

O desenho com shaders triviais e nada na tela custa ~4,8 Mcycles por quadro (32%): é
o custo fixo de um desenho (~700 ciclos), que nenhum grupo de estado explica sozinho
(religar tudo: −5%; manter o pipeline: −10%).

## Dentro do ubershader (PS C139857B, ~160 desenhos por quadro)

Quase todo desenho de cor usa a mesma família de PS (material iluminado). Variantes
editadas à mão, trocadas a cada dois desenhos do mesmo par (`tools/quick-ovr.py`, o
controle `id` deu −0,5%):

| variante | µs por desenho |
| --- | --- |
| buscas finais de textura içadas para o topo | −1,1% |
| sem as 24 amostras de sombra | −6,2% |
| sem constantes | −4,6% |
| descritores de textura como literais (ainda amostra) | −3,9% |
| sem nenhuma textura | −16,3% |
| sem constantes e sem texturas | −23,3% |

No VS comum (29688423) tirar todas as constantes não mudou nada. O "preamble" que sobra
com `nopreamble` é só a carga das faixas de UBO (um `ldc` serial por faixa, 4 no VS e 8
no PS), mas o custo dos desenhos não segue o número de faixas. Não há nenhuma alavanca de
shader maior que ~6% aqui.

## O achado: dois terços dos desenhos não põem nada na tela

Captura de oclusão (`plume-pairprof` modo 3, `tools/quick-occ.py`), cena parada:

- desenhos de cor: 72% (2.915 de 4.044 por quadro) não passam nenhuma amostra; em todas
  as famílias de material (65–90%) e em todos os tamanhos;
- desenhos só de profundidade (sombras): 53–58%;
- 99,8% dos desenhos de cor vazios num quadro continuam vazios no seguinte.

Pular exatamente esses desenhos (experimento, cena parada, conjunto aprendido por uma
captura) derrubou o quadro de **14,07 para 8,29 Mcycles (−41%)**, com a imagem igual.

Com a câmera girando, 88,9% continuam vazios, mas 2,4% dos vazios passam a aparecer no
quadro seguinte (~95 por quadro, ~900 pixels cada): prever só pelo quadro anterior faz
objetos entrarem atrasados. Uma sonda (desenho de verdade a cada K quadros) limita o
atraso a K−1 quadros, e uma guarda que desliga o pulo quando várias sondas viram visíveis
cobre o giro de câmera.

## O que não funciona: uma consulta de oclusão por desenho

Primeira versão de `renut_plume_occ_cull`: histórico por desenho, sonda, guarda, com uma
consulta de oclusão em cada desenho. Medido: **cada consulta custa ~2.500 ciclos** (esvazia
o pipeline), mais que o desenho que ela evitaria. Só ligar as consultas sobe o quadro de
14,26 para 28,68 Mcycles.

## O que funciona: o pixel shader escreve uma bandeira

Todo pixel shader recompilado, como primeira instrução, grava `1` numa palavra de 32 bits
por desenho (`renutMarkVisible` em `shader_common.h`, endereço no 4º push constant,
`RENUT_SPEC_VIS_FLAGS` liga). Custa uma escrita por onda, nada que se meça. O host lê as
bandeiras quando o slot do quadro volta (`ReadCullFlags`), mantém por nome de desenho
(hash de VS, PS, índices, primitiva + ocorrência no quadro) quantos quadros seguidos ele
ficou vazio, e:

- desenho vazio há 3 quadros: **não é emitido**, salvo a cada 4º quadro (escalonado por
  nome), quando é desenhado de verdade como sonda;
- um desenho que aparece chega com no máximo K−1 = 3 quadros de atraso;
- guarda: se numa frame várias sondas (≥ max(10, sondas/100)) mostram pixels, o corte
  fica desligado por 45 quadros (câmera girando, cena trocando). Só contam as sondas da
  vez do desenho, também com a guarda ligada (senão ela se realimenta);
- desenhos sempre visíveis só são rastreados a cada 4º quadro (não pagam nada);
- nunca rastreados: memexport, patch/rect/point list, sem pixel shader (sombras), e
  desenhos com stencil cujo *fail* ou *z-fail* não é `keep` (têm efeito sem pixel);
- se um quadro com ≥ 256 desenhos rastreados passa 6 vezes seguidas sem uma única
  bandeira, o corte desliga de vez (build cego). Antes eram 64 desenhos e 3 quadros, e uma
  tela de carregamento desligou o corte no meio de uma medição.

Para a bandeira só valer para fragmento que passou nos testes de profundidade/stencil, o
shader roda com `EarlyFragmentTests` (inserido no SPIR-V por `spirv_early_tests.h`), mas só
se ele não pode descartar nem escrever profundidade (`RENUT_SHADER_FLAG_LATE_TEST`, vindo
do gerador) e o desenho não usa alpha test nem alpha-to-coverage. Sem isso só 45% dos
vazios eram detectados e a guarda ficava sempre ligada.

### Resultados (cena parada, ciclos por quadro, `tools/quick-fps.py`)

| versão | variação |
| --- | --- |
| bandeiras, sem early test, rastreia tudo | −7,7% |
| + early test, rastreia 1 em 4 dos sempre visíveis | −17% a −26% |
| Showdown à noite, padrão atual (3 rodadas intercaladas) | −12% em média (13,0–15,4 contra 15,4–16,7 Mcycles) |

O FPS sobe menos que os ciclos: o governador baixa o clock quando a GPU fica mais folgada
(busy 74–84% contra 91–99%). Sombras (53–61% vazias, ~1.250–1.500 por quadro) ficam de fora:
usar um fragment shader só de bandeira nelas piorou (−12% contra −25,6%).

### Auditoria (modo 3: desenha tudo e pergunta a uma consulta de oclusão o que o modo 1 teria pulado)

Simula a guarda e a demora até a próxima sonda. O alvo principal é de 1280×720 (~920 mil
pixels).

| fase | pulados/quadro | com pixels | pixels faltando/quadro | quadros com guarda |
| --- | --- | --- | --- | --- |
| cena parada | 1.800–2.100 | 3,2–3,4 (0,17%) | 25–330 | 0–15% |
| câmera girando 30 s | 1.066 e 0 | 2,25 e 0 | 61 e 0 | 50% e 100% |
| parada de novo | 1.570–1.680 | 1,5–2,3 | 16–28 | 0–6% |

Ou seja: parado, o corte erra um desenho a cada ~600 e some, no pior quadro medido, ~330 pixels (0,04%);
em movimento a guarda o desliga, e a parte não guardada erra pouco mais que isso. O custo
real é perder o ganho enquanto a câmera se mexe (45 quadros depois de cada giro).

(Estado antes da reversão.) Padrão era ligado (`renut_plume_occ_cull` = 1). Desligar ao vivo: `files/plume-occcull` com
`0`; `2` escreve as bandeiras sem pular; `3` é a auditoria. O arquivo aceita
`<modo> <vazio> <sonda> <guarda> <viradas> <sombras> <amostra>` (padrão
`1 3 4 45 10 0 4`).

## Caminhos que seguem de pé

1. Sombras (só profundidade, 100% reordenáveis, ~1.250 vazias por quadro): consultas por
   grupo de desenhos consecutivos com divisão adaptativa; ~7–10% do quadro.
2. Guarda mais fina: hoje qualquer giro desliga tudo; dar para pular só os desenhos longe
   do que está aparecendo exigiria saber onde eles estão na tela.
3. Escolha de K e dos limites da guarda é qualidade contra desempenho; os padrões acima
   saíram das medições, não de um ajuste fino.

## Ferramentas

- `tools/quick-trivial.py`: `plume-trivial` 16+N alterna, por par, o shader real e o
  trivial (1: VS+PS, 2: só PS, 8: só VS).
- `tools/quick-ovr.py`: `plume-ovr "<hash> <sufixo> 2"` troca o módulo de um shader por
  `files/ovr/<hash>.<sufixo>.spv` em desenhos alternados; as variantes saem do HLSL
  editado compilado com o DXC do XenosRecomp (`-T ps_6_0 -HV 2021 -all-resources-bound
  -spirv -fvk-use-dx-layout -Qstrip_debug -DRENUT_RECOMP`, `-fvk-invert-y` no VS).
- `tools/quick-occ.py`: captura de oclusão e estatística de estabilidade entre quadros.
- `tools/quick-fps.py --fixed plume-trivial=1`: mantém outros switches durante a medida.
- Armadilha: a chave do pipeline precisa incluir *qual* módulo de override está em uso,
  senão as variantes trocadas ao vivo herdam o pipeline da primeira.
