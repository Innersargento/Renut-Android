# Desempenho, 16 e 17/10/2026

O que se fez e se mediu numa sessão longa sobre CPU (flags de codegen) e GPU
(desenhos que não deixam pixel, oclusão). Celular: S23 Ultra (Adreno 740, Turnip
empacotado). Cena: Showdown Town, câmera parada.

## 1. Flags de codegen (CPU do código do jogo)

`config/renut_funcs.toml` liga agora todas as flags que o UnleashedRecomp usa,
menos `reserved_as_local` (deixada desligada por escolha; no PC não fez diferença).

O gerador do RexGlue 0.10.0 supõe que nenhum valor desses registradores cruza uma
chamada, e o jogo cruza (o compilador do Xbox fazia alocação de registradores do
programa inteiro). `tools/codegen_handoff.py`, rodado por `prepare.py codegen`,
acha esses cruzamentos no próprio código gerado (vivacidade por fluxo de controle,
iterada sobre o grafo de chamadas) e escreve a passagem pelo `ctx`:

* `non_argument_as_local`: `r12` para os helpers de salvar/restaurar, o
  `__chkstk` e os tratadores de exceção; `r11` para 15 funções; `r11`/`v60` de
  saída em 9. Sem isso o jogo travava na tela-título 3D.
* `non_volatile_as_local`: saídas reais em `r19-r31`, `v21`, `v31` de poucas
  funções; valores em `v65-v77`, `v123`, `v126` chegando a milhares de funções.
  Por isso `v14-v31` e `v64-v127` ficam no `ctx` (com os helpers de vetor chamados
  de novo) e só `r14-r31`/`f14-f31` viram locais. Também: os helpers de
  salvar/restaurar voltam, pelo `ctx`, nos 6 funclets SEH que dividem registradores
  com o dono, e os locais são copiados em volta do `setjmp`. A primeira tentativa,
  sem tratar as saídas, desenhava só o céu na tela-título.

Medido no PC (tela-título, rodadas intercaladas): +1,5% a +2,8% de quadros e um
pouco menos de CPU por quadro em relação às 7 flags; `.text` da biblioteca 3,1%
menor com as 7 primeiras. A tela-título não é limitada pelo código do jogo, então o
ganho real esperado é menos CPU (calor) no celular.

## 2. Canal ao vivo de cvars (builds de depuração)

Desde o commit c6a41bc os interruptores do Plume são cvars, não arquivos, e só
mudavam na abertura do jogo. Agora, num APK de depuração:

```
adb shell am broadcast -a org.renut.android.SET_CVAR -p org.renut.android --es name renut_plume_hiz --es value 2
```

`RuntimeActivity.registerCvarReceiver` (só se o app for *debuggable*, protegido
pela permissão `DUMP`, que o shell do adb tem e app nenhum consegue) chama
`rex::cvar::SetFlagByName`. `result=1` na resposta = aceito.

`tools/bench/quick-fps.py --cvar <nome> --states a,b` usa esse canal (o `--file` antigo
ficou para o que ainda lê arquivo) e passou a mostrar a CPU por quadro das threads
"GPU Commands" e "PlumeRecord" (lidas via `run-as` do processo
`org.renut.android:runtime`).

`renut_plume_skip` (os antigos bits de `plume-skip`) voltou a existir como cvar,
seguido ao vivo. **Cuidado: o bit 256 derrubou o jogo no celular duas vezes.**

## 3. Censo dos desenhos que não deixam pixel

`renut_plume_zero_census` (seguido ao vivo): todo desenho roda dentro de uma
consulta de oclusão e é somado por assinatura de estado (shaders, profundidade,
stencil, cull, alpha, planos de recorte, máscaras, viewport). Relatório em
`files/plume-zero-census.txt` a cada 300 quadros e quando é desligado. Custa
muito FPS enquanto ligado (~21 FPS no Showdown); é ferramenta de medida.

**Showdown Town:**

| | Desenhos por quadro | Sem nenhum pixel |
| --- | --- | --- |
| Cor | ~3.360 | ~2.050 (61%) |
| Sombra | ~1.630 | ~690 (42%) |

* Com o teste de profundidade desligado (`renut_plume_skip 1024`), os zeros de cor
  caem de ~2.078 para ~759 por quadro: **~1.260 estão escondidos atrás de outra
  coisa**, ~600-760 estão fora da tela ou somem por outro motivo (alpha-test,
  triângulos de costas, sub-pixel).
* 978 desenhos de cor por quadro estão em assinaturas que não deixaram pixel em
  nenhum dos 1.400 quadros medidos.
* As famílias com skinning (`D0F3B9AD`, `AA574274`, `A017E382`, `76EB0224`)
  somam no máximo ~250 desenhos fora da tela por quadro. O resto dos zeros delas
  é oclusão.
* O pass de reflexo em 320x180 usa plano de recorte (`ucp=1`): 88 desenhos sem
  pixel por quadro (49% do pass). Daí o corte por plano de recorte do Hi-Z.

**Tela-título no PC:** 44-46% dos desenhos de cor e 56-58% dos de sombra sem pixel;
lá só 40% dos zeros são profundidade, e 58% dos tiles têm céu.

## 4. Hi-Z

* **Modo 2 ganhou o corte por plano de recorte do usuário** (exato), ~14 desenhos
  de cor por quadro no Showdown. Continua o padrão.
* **A passada de cor do jogo usa profundidade invertida (`GEQUAL`).** A oclusão
  antiga nunca a testava; com isso entendido, ela ficou exata, mas não paga: o
  Turnip cobra ~5.500 ciclos de GPU por desenho sob renderização condicional, mais
  que o desenho. Os modos 3, 4 e 5 foram retirados. Detalhes e números em
  `docs/performance/HIZ.md`, seção "Oclusão: por que não".

## 5. O que pesa na GPU no Showdown

Juntando as medidas desta sessão e das anteriores (26/09, 02-03/10, 10/10, 14-16/10):

* **O quadro é quantidade de desenhos vezes um custo fixo alto por desenho.**
  ~14-15 Mcycles por quadro para ~5.700 desenhos (depois do corte do modo 2),
  ~2.300-3.100 ciclos por desenho. Uns 65-70% disso é custo fixo por desenho, que
  não muda com o conteúdo; cada desenho paga a cadeia de latência do vertex shader
  da sua primeira onda, em série. Pixels pesam pouco: cortar todos (scissor de
  1 px) poupava ~14%.
* **Mais da metade desses desenhos não deixa pixel nenhum** (seção 3) e paga o
  custo inteiro mesmo assim. É o maior desperdício da cena, mas não há como
  descartá-los de forma exata e barata neste driver.
* Por fatia: passada de cor ~80% do tempo de desenho, sombras 15-20% (9-15% dos
  ciclos), água 4-6%, pós-processamento 3,5-4,4%.
* Os shaders mais caros por desenho são os com skinning e busca de vértice "ao
  vivo" (`A017E382`, `D0F3B9AD`, `0869F744`, `76EB0224`), que também não têm
  modelo para o corte de frustum.
* O clock: quente, o celular segura a GPU em 475-550 MHz em vez de 719, o que
  tira 25-35% do FPS em sessões longas.
