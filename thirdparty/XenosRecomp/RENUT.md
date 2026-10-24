# XenosRecomp no reNut

Cópia de <https://github.com/masterspike52/reNut-XenosRecomp> na revisão
`8f781f8e5f06e0ddd9c03c8155891af8f5c3786c`, com as alterações do reNut já
aplicadas. Os arquivos que o patch muda, em `XenosRecomp/`: `main.cpp`,
`pch.h`, `shader_common.h`, `shader_recompiler.cpp` e `shader_recompiler.h`.

`renut-xenosrecomp.patch` é o diff dessa revisão para estes fontes. Está aqui
só para registro: **não aplique de novo**. Para refazer a cópia:

1. clone a revisão acima com `--recursive`;
2. `git apply renut-xenosrecomp.patch`;
3. copie só os arquivos versionados (sem `.git` nem `build/`) e tire o que
   `thirdparty/README.md` diz que foi tirado.

Para atualizar o patch depois de mexer nos fontes, gere o diff contra a
revisão original com `git diff` e substitua o arquivo.

Licença: `LICENSE.md` (a do XenosRecomp). As bibliotecas em `thirdparty/`
trazem as suas (`fmt`, `smol-v`, `xxHash`, `zstd`). Os binários do DXC em
`thirdparty/dxc-bin` são do [DirectXShaderCompiler](https://github.com/microsoft/DirectXShaderCompiler)
(Microsoft), publicados pelo repositório do XenosRecomp.
