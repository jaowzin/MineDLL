# MineDLL / MineXRay

XRay interno para Minecraft Bedrock Windows x64, alvo principal `1.26.4501.0 / 26.45`.

Nao usa resource pack nem textura. A DLL tenta localizar `BlockLegacy::getRenderLayer` em runtime e aplica o comportamento de XRay no estilo Horion/Borion: blocos comuns recebem a camada de render usada pelo XRay, enquanto os tokens de `xray-blocks.txt` continuam com o render normal.

## Arquivos do build

- `MineXRay.dll` - DLL do XRay
- `MineXRayInjector.exe` - injector LoadLibraryW para `Minecraft.Windows.exe`
- `xray-blocks.txt` - lista dos blocos que permanecem visiveis
- `MineXRay.pdb` e `MineXRayInjector.pdb` quando disponiveis - simbolos para diagnostico

## Uso recomendado

1. Feche builds antigas do MineMod/MineXRay.
2. Abra o Minecraft Bedrock e fique no menu principal.
3. Deixe `MineXRayInjector.exe`, `MineXRay.dll` e `xray-blocks.txt` na mesma pasta.
4. Execute `MineXRayInjector.exe`.
5. Depois da mensagem de sucesso, entre no mundo.
6. O XRay inicia ligado.

Injetar no menu antes de entrar no mundo e o caminho mais confiavel porque os chunks novos ja sao construidos passando pelo hook.

## Teclas

- `F6` - liga/desliga o XRay e tenta reconstruir chunks
- `F7` - grava diagnostico no log
- `F8` - tenta reconstruir os chunks manualmente
- `F12` - restaura o hook e descarrega a DLL

Log: `%TEMP%\MineXRay.log`

## Blocos visiveis

O arquivo `xray-blocks.txt` usa tokens por substring. O padrao inclui:

```text
ore
ancient_debris
lava
water
chest
spawner
amethyst
vault
```

`ore` mantem todos os minerios visiveis. Para deixar somente diamante, por exemplo, remova `ore` e use:

```text
diamond_ore
deepslate_diamond_ore
```

Recarregue a DLL depois de editar o arquivo.

## Implementacao 26.45

A DLL nao usa enderecos absolutos, porque o Bedrock usa ASLR. Ela resolve estruturas em runtime por signatures/RTTI e valida candidatos antes de escrever o hook. O caminho de rebuild usa a estrutura de LevelRenderer conhecida do alvo 26.45, mas falha de forma segura se a assinatura nao existir; nesse caso, injete no menu principal e reentre no mundo para os chunks serem gerados com o XRay ativo.

## Build local

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
```

Saida em `build/Release/`.
