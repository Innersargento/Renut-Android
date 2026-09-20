# Plume — relatório de bugs gráficos, 19/09/2026

## Estado da sessão

Projeto: reNut, branch `codex/android-xenos-runtime`. Testes no Samsung SM-S918B, GPU Adreno 740, driver Turnip. O renderizador selecionado ao encerrar os testes é Plume. A comparação com Xenos foi feita no mesmo aparelho.

As correções anteriores da aura dourada, das rosetas verdes no terreno e da montagem das sombras distantes foram aprovadas pelo usuário. Ao encerrar esta etapa, o usuário confirmou também que os problemas de sombra foram resolvidos, incluindo os relatos de projeção em texturas e no chão com pedras. Permanecem pendentes os pelos do Banjo, a água e o veículo.

## Mudanças registradas nesta etapa

Arquivo: `android/rexglue/plume_gpu.cpp`.

- Respeitar o descarte de faces dianteiras/traseiras e a orientação das faces definidos pelo jogo, para primitivas poligonais. Antes, o Plume sempre desenhava ambos os lados. Quando ambos os lados estão descartados, o desenho é omitido.
- Incluir esses estados na chave do cache de pipelines, evitando reutilizar um pipeline com orientação ou descarte diferentes.
- Acrescentar ao diagnóstico de desenhos os estados de rasterização, deslocamento de profundidade, mistura de cores e referência de alfa. A saída adicional depende da captura de diagnóstico estar ativa.
- Manter a opção de diagnóstico `plume-skip=131072` para desativar temporariamente o novo descarte de faces.

Validação: compilação Android concluída com `BUILD SUCCESSFUL in 21s` em `out/graphics-next/build-culling.log`; APK instalado e observado no aparelho. O menu apresentou melhora visível na TV e no aquário. A passagem inicial da câmera não apresentou inversão geral da geometria. Isso não constitui validação de todos os cenários do jogo. Os pelos do Banjo continuaram incorretos.

## 1. Pelos do Banjo no menu — pendente, investigação avançada

Sintoma: braços, pernas e cabeça apresentam granulação muito forte. Na referência Xenos, os pelos têm aparência mais uniforme. A aura permanece dourada; o defeito atual não é a antiga ocorrência de faixas coloridas na aura.

Teste mais recente: capturado o menu ao vivo em `banjo-menu-now.png`. Desativar temporariamente as texturas empilhadas com `plume-skip=65536` eliminou a granulação visível (`banjo-no-stack.png`). A configuração foi restaurada para `0` imediatamente após a captura. Esse teste aponta para o caminho da textura de relevo usada pelos pelos, mas não prova ainda qual etapa está errada. Desativar a textura não é uma correção aceitável: também altera a iluminação do material.

Testes anteriores: desativar a aproximação de alpha-to-coverage (`4096`) e limitar os mipmaps das texturas 2D (`8192`) não resolveu o defeito. O teste de mipmaps 2D não elimina a hipótese de problema nos mipmaps das texturas empilhadas.

Identificação pelos dumps de texturas e pelos desenhos do menu:

- Pixel shaders envolvidos: `EA4C367F67309DCC`, `14A0D4C740686E7D` e `54A01C80D41DA2BA`.
- Vertex shader frequente: `B814D7237E615990`.
- Textura de relevo empilhada em t1: `1C21C000`, formato f18/DXT1, tamanho declarado 128×128; nível superior observado 64×64, mip mínimo 1. Outra textura usada no rosto: `1B908000`, f19/DXT3.
- Texturas de cor identificadas: braços `1C87D000`, rosto `1BFB1000`, corpo `1C17E000`. Os endereços pertencem à execução capturada e podem mudar ao reiniciar.
- O shader lê `tfetch3D(...).yzw` e reconstrói a normal. `GetGuestStackedTexture` representa a pilha como uma textura 3D com apenas um nível de mipmap.

Próxima investigação: comparar a seleção/interpolação de camadas, o endereçamento por eixo, os filtros de volume e os mipmaps com o caminho do Xenos. Confirmar também a decodificação das fatias. Não há ainda causa raiz comprovada nem correção dos pelos.

## 2. Sombras sobre texturas e chão com pedras — resolvido, validado pelo usuário

O usuário havia apontado sombras projetadas incorretamente em certas superfícies, especialmente no caminho de pedras. Referência histórica: `215927.jpg`, da passagem inicial da câmera. Na atualização final desta sessão, confirmou: “os problemas de sombra foram resolvidos”. Essa confirmação substitui o estado anterior de investigação pendente.

Preservar o comportamento atual. A validação visual final é do usuário; não foi estabelecida uma causa isolada para cada manifestação anterior de sombra.

Como histórico técnico, o registro de deslocamento de profundidade mostrou valores não nulos em poucos desenhos de partículas. Não foi aplicada uma correção de depth bias nesta etapa. As hipóteses anteriores sobre filtragem ou stencil não devem ser tratadas como defeitos confirmados nem como trabalho pendente de sombras.

## 3. Água ausente — pendente

Referências: `215953.jpg` e `215957.jpg`, no vale com ponte durante a passagem inicial. A superfície da água não aparece corretamente.

Ainda é necessário identificar o desenho da superfície e comparar com Xenos no mesmo ponto. Foi observado um possível problema de interpretação de profundidade em shaders de partículas: alguns leem o canal Z de uma textura de profundidade resolvida que o Plume fornece como R32. Isso pode produzir valores incorretos, mas não foi demonstrada relação causal com a água ausente. Não aplicar uma correção específica à água com base apenas nessa hipótese.

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
