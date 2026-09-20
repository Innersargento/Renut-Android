# Plume — relatório de bugs gráficos, 19/09/2026

## Estado da sessão

Projeto: reNut, branch `codex/android-xenos-runtime`. Testes no Samsung SM-S918B, GPU Adreno 740, driver Turnip. O renderizador selecionado ao encerrar os testes é Plume. A comparação com Xenos foi feita no mesmo aparelho.

As correções anteriores da aura dourada, das rosetas verdes no terreno e da montagem das sombras distantes foram aprovadas pelo usuário. Ao encerrar esta etapa, o usuário confirmou também que os problemas de sombra foram resolvidos, incluindo os relatos de projeção em texturas e no chão com pedras. Os pelos do Banjo foram corrigidos nesta etapa. A causa da água ausente foi identificada, sem correção ainda. Permanece pendente o veículo.

## Mudanças registradas nesta etapa

Arquivo: `android/rexglue/plume_gpu.cpp`.

- Respeitar o descarte de faces dianteiras/traseiras e a orientação das faces definidos pelo jogo, para primitivas poligonais. Antes, o Plume sempre desenhava ambos os lados. Quando ambos os lados estão descartados, o desenho é omitido.
- Incluir esses estados na chave do cache de pipelines, evitando reutilizar um pipeline com orientação ou descarte diferentes.
- Acrescentar ao diagnóstico de desenhos os estados de rasterização, deslocamento de profundidade, mistura de cores e referência de alfa. A saída adicional depende da captura de diagnóstico estar ativa.
- Manter a opção de diagnóstico `plume-skip=131072` para desativar temporariamente o novo descarte de faces.
- Limitar o eixo Z das texturas empilhadas à extremidade da pilha, em vez de repeti-lo com o modo do eixo X, e carregar a cadeia de mipmaps do guest nas pilhas de uma única fatia. Ver a seção 1.
- Acrescentar ao diagnóstico de desenhos os modos de limite por eixo, a faixa de níveis e a profundidade de cada textura, e as constantes do pixel shader nos desenhos com textura empilhada.
- Manter a opção de diagnóstico `plume-skip=262144` para desativar temporariamente os mipmaps das pilhas de uma fatia.
- Construir os amostradores a partir do estado que a constante de busca declara — filtros de ampliação, redução e mipmap, anisotropia, modo de limite por eixo e deslocamento de nível — em vez de reduzir tudo a ponto/linear e ao modo do eixo X. Ver a seção 5.
- Manter a opção de diagnóstico `plume-skip=524288` para voltar ao amostrador único anterior.

Validação: compilação Android concluída com `BUILD SUCCESSFUL in 21s` em `out/graphics-next/build-culling.log`; APK instalado e observado no aparelho. O menu apresentou melhora visível na TV e no aquário. A passagem inicial da câmera não apresentou inversão geral da geometria. Isso não constitui validação de todos os cenários do jogo. Os pelos do Banjo continuaram incorretos.

## 1. Pelos do Banjo no menu — resolvido

Sintoma anterior: braços, pernas e cabeça apresentavam granulação muito forte,
enquanto na referência Xenos os pelos têm aparência uniforme.

Causa: o eixo Z das texturas empilhadas era endereçado com o modo de repetição do
eixo X. A pilha é um arranjo de fatias; o Z escolhe uma camada e o guest a limita
à extremidade, nunca a repete. O registro de desenho confirma o que o jogo pede
na constante de busca da textura de relevo `1C21C000`: `clamp_x=0` e `clamp_y=0`
(repetir), `clamp_z=2` (limitar à extremidade).

O shader entrega Z = `-0.03125`, ou seja meia fatia abaixo da primeira de uma
pilha de 16. Limitando o eixo, isso é a fatia 0; repetindo, vira a fatia 15, no
outro extremo. As 16 fatias despejadas do aparelho mostram por que a diferença é
tão visível: são o mesmo relevo em amplitude crescente, da fatia 0 plana (canais
G e B constantes em 125 e 123) até a fatia 15 com amplitude total (G de 0 a 255).
O Plume aplicava o relevo mais forte da pilha onde o jogo pede o mais fraco.

Correção em `android/rexglue/plume_gpu.cpp`: a tabela de amostradores passa a ter
uma segunda metade com o eixo W limitado, e as buscas empilhadas usam essa
metade. Os eixos U e V continuam com o modo do guest.

Também nesta etapa, as texturas empilhadas de uma única fatia passam a carregar a
cadeia de mipmaps do guest, como já faziam as 2D. Uma imagem 3D do host divide a
profundidade a cada nível, o que misturaria fatias vizinhas, então pilhas com
mais de uma fatia continuam com um nível só. O menu não exercita esse caminho:
todas as pilhas vistas nele têm 16 fatias.

Validação: `BUILD SUCCESSFUL` em `out/graphics-next/build-fur.log`; APK instalado
no SM-S918B com Plume/Turnip e menu capturado em
`out/graphics-next/banjo-fur-fixed.png`, com os pelos uniformes e a textura de
relevo ainda ligada — diferente do teste `plume-skip=65536`, que a desligava e
alterava a iluminação do material. Evidências: `fur-stack-slices.png` (as 16
fatias), `fur-ucode-trace.txt` (a constante Z por desenho) e
`fur-clamped-trace.txt` (os modos de limite por textura).

Identificação, mantida para referência:

- Pixel shaders envolvidos: `EA4C367F67309DCC`, `14A0D4C740686E7D`,
  `54A01C80D41DA2BA`, `785DDFE8F61DF106` e `5C13267B0C24C259`.
- Vertex shader frequente: `B814D7237E615990`.
- Textura de relevo empilhada em t1: `1C21C000`, f18/DXT1, 128×128 declarados,
  nível superior 64×64, mip mínimo 1, 16 fatias.
- Texturas de cor identificadas: braços `1C87D000`, rosto `1BFB1000`, corpo
  `1C17E000`. Os endereços pertencem à execução capturada.

Testes anteriores que não resolviam: desativar a aproximação de alpha-to-coverage
(`4096`) e limitar os mipmaps das texturas 2D (`8192`).

## 2. Sombras sobre texturas e chão com pedras — resolvido, validado pelo usuário

O usuário havia apontado sombras projetadas incorretamente em certas superfícies, especialmente no caminho de pedras. Referência histórica: `215927.jpg`, da passagem inicial da câmera. Na atualização final desta sessão, confirmou: “os problemas de sombra foram resolvidos”. Essa confirmação substitui o estado anterior de investigação pendente.

Preservar o comportamento atual. A validação visual final é do usuário; não foi estabelecida uma causa isolada para cada manifestação anterior de sombra.

Como histórico técnico, o registro de deslocamento de profundidade mostrou valores não nulos em poucos desenhos de partículas. Não foi aplicada uma correção de depth bias nesta etapa. As hipóteses anteriores sobre filtragem ou stencil não devem ser tratadas como defeitos confirmados nem como trabalho pendente de sombras.

## 5. Nitidez das texturas — corrigido

O Plume montava seis amostradores fixos e escolhia entre eles por dois bits: o
filtro de redução, para decidir ponto ou linear, e o modo de limite do eixo X,
aplicado também a Y e a Z. Tudo o mais que a constante de busca declara era
descartado.

O registro de um quadro da passagem inicial mostra o que o jogo pede:

| Estado pedido | Ocorrências |
| --- | --- |
| anisotropia 2× (`aniso_filter=2`) | 12.807 |
| ampliação e redução lineares com mipmap por ponto (`f110`) | 12.807 |
| trilinear (`f111`) | 1.002 |
| só o nível base (`mip_filter=kBaseMap`) | 79 |
| limite diferente entre X e Y (`c022`, `c202`, `c220`) | 742 |
| limite por borda em X e Y (`c660`) | 79 |

A anisotropia era o que mais pesava: mais de doze mil vínculos de textura por
quadro pediam 2×, e nenhum a recebia, o que tirava a nitidez de tudo o que é
visto em ângulo — o caminho de pedras, o terreno e o Banjo.

Os amostradores passam a ser construídos sob demanda a partir da constante de
busca, com uma tabela indexada pelo estado pedido, e o descritor 0 continua
sendo o amostrador padrão para o caso de a tabela encher. O eixo W das texturas
empilhadas continua limitado, pelo motivo da seção 1.

Evidência: `sampler-trace.txt`. Validação no aparelho com a passagem inicial e o
menu, sem artefato novo.

## 3. Água ausente — causa identificada, correção pendente

Não é falta de patch do jogo nem de dados: o jogo emite os desenhos da água, e o
Plume é que os descarta. O registro da passagem inicial mostra a sequência, logo
após o resolve da cena e antes do resolve final do quadro:

```
dropped prim=1  n=1   vs=9CD3AF1135A8FEE3 ps=60E8DEF931549772 tess=2 indexed=2
dropped prim=1  n=99  vs=1DF3417E061967C2 ps=88680E4C8A6BCFFE tess=2 indexed=2
dropped prim=1  n=512 vs=3D0794ADDBF5A4F3 ps=60E8DEF931549772 tess=2 indexed=2
dropped prim=17 n=99  vs=F09B139EE9188255 ps=3277104CE11D8635 tess=2 indexed=0
```

`prim=17` é `kTrianglePatch` e `tess=2` é o modo adaptativo. As três listas de
pontos antes dele são passagens de *memory export*: escrevem na memória os
fatores de tesselação que o desenho adaptativo lê. O próprio rexglue registra
esse uso para o título 4D5307E6 em `primitive_processor.cpp`: «4D5307E6 — water —
adaptive».

O Plume não tem caminho de tesselação nem de memory export. No `switch` de
topologia, `kTrianglePatch` cai no `default` e o desenho é omitido; as listas de
pontos caem no mesmo lugar. O vertex shader da água confirma que é um domain
shader: ele mistura três pontos de controle com pesos baricêntricos que o
hardware entrega em `r0.xyz`.

```
r2 = P0 * r0.z + P1 * r0.y + P2 * r0.x
```

O XenosRecomp não conhece domain shaders: ele inicializa `r0` com o índice do
vértice (`float4 r0 = float4(float(iVertexId), 0, 0, 0)`), então o shader
recompilado hoje não reproduz a superfície nem se o desenho fosse aceito. O
recompilador também não emula memory export — as escritas vão para um registrador
descartado.

Isso explica também por que o Xenos não desenha a água no mesmo aparelho: o
defeito não está nos dados do jogo, está no suporte a tesselação e a memory
export do renderizador.

Para resolver são necessários, no mínimo:

1. `r0.xyz` vindo dos pesos do domínio nos shaders recompilados, com um spec
   constant para não alterar os desenhos comuns — o que exige regerar
   `generated/shaders/`.
2. No Plume, expandir cada patch em um triângulo (fator de tesselação 1),
   repetindo o elemento de cada patch três vezes e emitindo os pesos por canto.
3. Opcionalmente, memory export, para usar os fatores adaptativos reais em vez
   de um fator fixo.

Evidências: `water-trace.txt` (a sequência acima) e `flyby-states.txt`. O plano
completo, com as definições do Xenos que o domain shader e o hull shader têm de
respeitar, está em `docs/AGUA-TESSELACAO.md`; a primeira etapa — tesselação na
interface do Plume — já está no repositório.

A hipótese anterior sobre a leitura do canal Z de uma textura de profundidade
resolvida entregue como R32 não tem relação com a água ausente e permanece sem
demonstração.

## 4. Veículo incompleto na cena da água — pendente

Nas mesmas referências da água, o veículo apresenta partes ausentes ou incorretas. Ainda não foi isolado se o problema está em geometria, material, transparência ou profundidade. Reavaliar a cena com a correção de descarte de faces desta etapa e comparar com Xenos antes de alterar outro estado de renderização. Não há validação de correção do veículo.

## Outras diferenças vistas no menu

A imagem do quadro na parede e o fundo escuro atrás das instruções inferiores aparecem no Xenos e estão ausentes nas capturas Plume. São observações adicionais que podem ajudar a encontrar estados de renderização ainda não implementados; não foram corrigidas nesta etapa.

## Histórico aprovado

- `4c08ec9` — `Fix Plume golden aura resolves and terrain UV fetches`: aura e coordenadas de textura do terreno, com correções de texturas associadas.
- `2c9149d` — `Fix Plume shadow atlas assembly and initial clears`: montagem do atlas de sombras e preservação das limpezas iniciais.

## Evidências e retomada

Capturas, registros e dumps locais estão em `C:/Users/Sidney-PC/Projetos/reNut/out/graphics-next/` (arquivos de trabalho ignorados pelo Git):

- `banjo-menu-now.png`: menu Plume antes do teste de isolamento.
- `banjo-no-stack.png`: teste temporário sem texturas empilhadas; não representa a configuração final.
- `xenos-menu.png`: referência Xenos.
- `banjo-culling.png` e `culling-flyby.png`: verificação do descarte de faces.
- `215927.jpg`, `215953.jpg`, `215957.jpg`, `220042.jpg`, `220046.jpg`: referências fornecidas pelo usuário.
- `banjo-culling-trace.txt`, `banjo-states.txt`, `flyby-states.txt`: registros de desenhos.
- `menu-textures.png` e `menu-textures.tar`: identificação das texturas do menu.

Ao retomar, preservar as correções aprovadas, manter a aura dourada e validar as mudanças com capturas equivalentes no aparelho. Configuração final do teste: Plume selecionado e `plume-skip=0`.
