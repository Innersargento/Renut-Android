# key: (pt-BR, en-US, zh-CN, es)
# Placeholders are Android positional format args (%1$s, %1$d). A literal percent is %%.
T = {}


def add(key, pt, en, zh, es):
    assert key not in T, key
    T[key] = (pt, en, zh, es)


# ---- common ------------------------------------------------------------------------------------
add("common_cancel", "Cancelar", "Cancel", "取消", "Cancelar")
add("common_close", "Fechar", "Close", "关闭", "Cerrar")
add("common_delete", "Apagar", "Delete", "删除", "Eliminar")
add("common_back", "Voltar", "Back", "返回", "Volver")
add("common_disabled", "Desativado", "Off", "已关闭", "Desactivado")
add("common_ok", "OK", "OK", "确定", "Aceptar")
add("language_label", "Idioma", "Language", "语言", "Idioma")

# ---- launcher navigation -------------------------------------------------------------------------
add("nav_play", "Jogar", "Play", "开始游戏", "Jugar")
add("nav_saves", "Saves", "Saves", "存档", "Partidas")
add("nav_achievements", "Conquistas", "Achievements", "成就", "Logros")
add("nav_settings", "Configurações", "Settings", "设置", "Ajustes")

# ---- saves ---------------------------------------------------------------------------------------
add("saves_eyebrow", "SEUS SAVES", "YOUR SAVES", "你的存档", "TUS PARTIDAS")
add("saves_title", "Arquivos do jogo", "Game files", "游戏文件", "Archivos del juego")
add("saves_subtitle", "Gerencie os saves guardados neste aparelho.", "Manage the saves kept on this device.",
    "管理保存在此设备上的存档。", "Gestiona las partidas guardadas en este dispositivo.")
add("saves_panel_library", "Biblioteca", "Library", "存档库", "Biblioteca")
add("saves_none", "Nenhum save encontrado", "No saves found", "未找到存档", "No se encontraron partidas")
add("saves_found", "%1$d save(s) encontrado(s)", "%1$d save(s) found", "找到 %1$d 个存档", "%1$d partida(s) encontrada(s)")
add("saves_note", "Os arquivos são lidos da pasta usada pelo jogo.", "The files are read from the folder the game uses.",
    "文件读取自游戏使用的文件夹。", "Los archivos se leen de la carpeta que usa el juego.")
add("saves_section_files", "Arquivos", "Files", "文件", "Archivos")
add("saves_item", "Save do jogo", "Game save", "游戏存档", "Partida guardada")
add("saves_panel_transfer", "Transferir", "Transfer", "传输", "Transferir")
add("saves_export", "Exportar saves (.zip)", "Export saves (.zip)", "导出存档 (.zip)", "Exportar partidas (.zip)")
add("saves_import", "Importar saves (.zip)", "Import saves (.zip)", "导入存档 (.zip)", "Importar partidas (.zip)")
add("saves_import_note", "A importação pede confirmação antes de substituir arquivos existentes.",
    "Importing asks for confirmation before replacing existing files.",
    "导入时会先确认，然后才会替换现有文件。", "La importación pide confirmación antes de reemplazar archivos existentes.")
add("saves_import_title", "Importar saves?", "Import saves?", "导入存档？", "¿Importar partidas?")
add("saves_import_message", "Arquivos com o mesmo caminho serão substituídos. Feche o jogo antes de continuar.",
    "Files with the same path will be replaced. Close the game before continuing.",
    "路径相同的文件将被替换。请先关闭游戏再继续。",
    "Los archivos con la misma ruta serán reemplazados. Cierra el juego antes de continuar.")
add("saves_import_action", "Importar", "Import", "导入", "Importar")
add("saves_imported", "Saves importados.", "Saves imported.", "存档已导入。", "Partidas importadas.")
add("saves_exported", "Saves exportados.", "Saves exported.", "存档已导出。", "Partidas exportadas.")
add("saves_failed_title", "Falha nos saves", "Save transfer failed", "存档操作失败", "Error con las partidas")

# ---- play ----------------------------------------------------------------------------------------
add("play_eyebrow", "RENUT / JOGAR", "RENUT / PLAY", "RENUT / 开始游戏", "RENUT / JUGAR")
add("play_title", "Pronto para jogar?", "Ready to play?", "准备好开始了吗？", "¿Listo para jugar?")
add("play_subtitle", "Sua aventura continua de onde você parou.", "Your adventure picks up where you left off.",
    "你的冒险将从上次停下的地方继续。", "Tu aventura continúa donde la dejaste.")
add("play_state_full", "Jogo completo", "Full game", "完整游戏", "Juego completo")
add("play_state_xex", "Só o XEX importado", "Only the XEX imported", "仅导入了 XEX", "Solo se importó el XEX")
add("play_state_none", "Nenhum arquivo importado", "No files imported", "尚未导入任何文件", "No se importó ningún archivo")
add("play_button", "Jogar", "Play", "开始游戏", "Jugar")
add("play_hint_xex", "Importe a imagem ISO completa em Configurações para ter os arquivos que o jogo precisa.",
    "Import the full ISO image in Settings to get the files the game needs.",
    "请在“设置”中导入完整的 ISO 镜像，以获得游戏所需的文件。",
    "Importa la imagen ISO completa en Ajustes para obtener los archivos que necesita el juego.")
add("play_hint_none", "Importe o jogo em Configurações para começar.", "Import the game in Settings to get started.",
    "请在“设置”中导入游戏后开始。", "Importa el juego en Ajustes para empezar.")
add("play_experimental", "Experimental: o desempenho e a compatibilidade ainda estão em desenvolvimento.",
    "Experimental: performance and compatibility are still in development.",
    "实验性版本：性能和兼容性仍在开发中。", "Experimental: el rendimiento y la compatibilidad siguen en desarrollo.")
add("play_cheats_on", "Cheats ligados nesta partida", "Cheats are on for this session", "本局已启用作弊",
    "Trucos activados en esta partida")
add("play_ach_progress", "%1$d de %2$d conquistas", "%1$d of %2$d achievements", "%1$d / %2$d 项成就",
    "%1$d de %2$d logros")
add("play_photos_card", "Fotos do jogo", "Game photos", "游戏照片", "Fotos del juego")
add("play_photos_count", "%1$d fotos tiradas no Modo Foto", "%1$d photos taken in Photo Mode",
    "在拍照模式下拍摄的 %1$d 张照片", "%1$d fotos tomadas en el Modo Foto")
add("play_photos_none", "Nenhuma foto ainda", "No photos yet", "还没有照片", "Aún no hay fotos")
add("photos_eyebrow", "ÁLBUM", "ALBUM", "相册", "ÁLBUM")
add("photos_title", "Fotos do jogo", "Game photos", "游戏照片", "Fotos del juego")
add("photos_subtitle", "As fotos que você tirou com o Modo Foto do jogo.",
    "The pictures you took with the game's Photo Mode.", "你用游戏拍照模式拍摄的照片。",
    "Las fotos que hiciste con el Modo Foto del juego.")
add("photos_empty", "Nenhuma foto ainda. Tire uma no jogo, com o Modo Foto, e ela aparece aqui.",
    "No photos yet. Take one in the game with Photo Mode and it shows up here.",
    "还没有照片。在游戏中用拍照模式拍一张，它就会出现在这里。",
    "Aún no hay fotos. Haz una en el juego con el Modo Foto y aparecerá aquí.")
add("photos_untitled", "Foto", "Photo", "照片", "Foto")
add("photos_counter", "%1$d de %2$d", "%1$d of %2$d", "%1$d / %2$d", "%1$d de %2$d")
add("photos_save", "Salvar na galeria", "Save to gallery", "保存到相册", "Guardar en la galería")
add("photos_saved", "Foto salva em Imagens/reNut.", "Photo saved to Pictures/reNut.",
    "照片已保存到 Pictures/reNut。", "Foto guardada en Imágenes/reNut.")
add("photos_save_failed", "Não foi possível salvar a foto.", "The photo could not be saved.",
    "无法保存照片。", "No se pudo guardar la foto.")
add("photos_export", "Exportar todas as fotos (.zip)", "Export all photos (.zip)", "导出全部照片 (.zip)", "Exportar todas las fotos (.zip)")
add("photos_exported", "Fotos exportadas.", "Photos exported.", "照片已导出。", "Fotos exportadas.")
add("photos_export_none", "Não há fotos para exportar.", "There are no photos to export.",
    "没有可导出的照片。", "No hay fotos para exportar.")
add("play_saves_card", "Saves", "Saves", "存档", "Partidas guardadas")
add("play_panel_open","Ao abrir o app", "When opening the app", "打开应用时", "Al abrir la aplicación")
add("play_autostart", "Iniciar direto no jogo", "Start straight into the game", "直接进入游戏", "Iniciar directamente el juego")
add("play_autostart_note", "O jogo começa sem passar por esta tela. Para voltar aqui, saia do jogo pelo menu.",
    "The game starts without going through this screen. To come back here, leave the game from its menu.",
    "游戏将跳过此界面直接开始。要回到这里，请通过游戏菜单退出游戏。",
    "El juego arranca sin pasar por esta pantalla. Para volver aquí, sal del juego desde su menú.")

# ---- cheats --------------------------------------------------------------------------------------
add("cheats_panel", "Cheats", "Cheats", "作弊", "Trucos")
add("cheats_master", "Aplicar cheats ao jogar", "Apply cheats when playing", "游戏时启用作弊", "Aplicar trucos al jugar")
add("cheat_infinite_health", "Vida infinita", "Infinite health", "无限生命", "Vida infinita")
add("cheat_infinite_fuel_ammo", "Combustível e munição infinitos", "Infinite fuel and ammo", "无限燃料和弹药",
    "Combustible y munición infinitos")
add("cheat_infinite_parts", "Peças infinitas", "Infinite parts", "无限零件", "Piezas infinitas")
add("cheat_no_timer", "Sem limite de tempo nas missões", "No time limit on missions", "任务无时间限制",
    "Sin límite de tiempo en las misiones")
add("cheat_no_notes_spent", "Notas não são gastas", "Notes are not spent", "音符不会被消耗", "Las notas no se gastan")
add("cheat_vehicles_anywhere", "Trocar e montar veículo em qualquer lugar", "Swap and build vehicles anywhere",
    "可在任意地点更换和组装载具", "Cambiar y montar vehículo en cualquier lugar")
add("cheat_extended_build_range", "Área de montagem maior (Mumbo's Motors)", "Larger build area (Mumbo's Motors)",
    "更大的组装区域（Mumbo's Motors）", "Área de montaje mayor (Mumbo's Motors)")
add("cheats_skin", "Visual do Banjo", "Banjo's look", "班卓的外观", "Aspecto de Banjo")
add("skin_default", "Normal", "Normal", "普通", "Normal")
add("skin_robot", "Robô", "Robot", "机器人", "Robot")
add("skin_tuxedo", "Smoking", "Tuxedo", "燕尾服", "Esmoquin")
add("cheats_note", "Valem a partir da próxima partida. Peças infinitas altera as contagens de peças do seu save.",
    "They take effect from the next session. Infinite parts changes the part counts in your save.",
    "从下一局开始生效。无限零件会改变存档中的零件数量。",
    "Surten efecto desde la próxima partida. Piezas infinitas cambia el recuento de piezas de tu partida guardada.")

# ---- achievements ---------------------------------------------------------------------------------
add("ach_eyebrow", "CONQUISTAS", "ACHIEVEMENTS", "成就", "LOGROS")
add("ach_title", "Suas conquistas", "Your achievements", "你的成就", "Tus logros")
add("ach_subtitle", "As conquistas desbloqueadas no jogo, neste aparelho.", "The achievements unlocked in the game, on this device.",
    "在此设备上的游戏中解锁的成就。", "Los logros desbloqueados en el juego, en este dispositivo.")
add("ach_panel_nolist", "Ainda sem lista", "No list yet", "暂无列表", "Aún sin lista")
add("ach_nolist", "A lista de conquistas é lida do seu jogo na primeira vez em que ele inicia. Abra o jogo uma vez e volte aqui.",
    "The achievement list is read from your game the first time it starts. Open the game once and come back here.",
    "成就列表会在游戏首次启动时从游戏中读取。请先打开一次游戏，然后回到这里。",
    "La lista de logros se lee de tu juego la primera vez que arranca. Abre el juego una vez y vuelve aquí.")
add("ach_panel_progress", "Progresso", "Progress", "进度", "Progreso")
add("ach_unlocked_count", "%1$d de %2$d desbloqueadas", "%1$d of %2$d unlocked", "已解锁 %1$d / %2$d", "%1$d de %2$d desbloqueados")
add("ach_gamerscore", "%1$d de %2$d G", "%1$d of %2$d G", "%1$d / %2$d G", "%1$d de %2$d G")
add("ach_section_unlocked", "Desbloqueadas", "Unlocked", "已解锁", "Desbloqueados")
add("ach_section_locked", "Bloqueadas", "Locked", "未解锁", "Bloqueados")
add("ach_unlocked_on", "Desbloqueada em %1$s", "Unlocked on %1$s", "解锁于 %1$s", "Desbloqueado el %1$s")
add("ach_toast_heading", "CONQUISTA DESBLOQUEADA", "ACHIEVEMENT UNLOCKED", "成就已解锁", "LOGRO DESBLOQUEADO")

# ---- settings ------------------------------------------------------------------------------------
add("settings_eyebrow", "RENUT / ANDROID", "RENUT / ANDROID", "RENUT / ANDROID", "RENUT / ANDROID")
add("settings_title", "Configurações", "Settings", "设置", "Ajustes")
add("settings_subtitle", "Jogo, vídeo, controles e diagnóstico.", "Game, video, controls and diagnostics.",
    "游戏、视频、控制和诊断。", "Juego, vídeo, controles y diagnóstico.")
add("settings_game", "Jogo", "Game", "游戏", "Juego")
add("settings_game_full", "Banjo-Kazooie: Nuts & Bolts · jogo completo", "Banjo-Kazooie: Nuts & Bolts · full game",
    "Banjo-Kazooie: Nuts & Bolts · 完整游戏", "Banjo-Kazooie: Nuts & Bolts · juego completo")
add("settings_game_xex", "Banjo-Kazooie: Nuts & Bolts · só o XEX importado", "Banjo-Kazooie: Nuts & Bolts · only the XEX imported",
    "Banjo-Kazooie: Nuts & Bolts · 仅导入了 XEX", "Banjo-Kazooie: Nuts & Bolts · solo se importó el XEX")
add("settings_game_none", "Nenhum jogo importado", "No game imported", "尚未导入游戏", "Ningún juego importado")
add("settings_import_game", "Importar jogo (ISO ou XEX)", "Import game (ISO or XEX)", "导入游戏（ISO 或 XEX）",
    "Importar juego (ISO o XEX)")
add("settings_import_note", "O jogo atual só é substituído depois que o novo for validado.",
    "The current game is only replaced after the new one is validated.",
    "只有在新游戏通过验证后，才会替换当前游戏。", "El juego actual solo se reemplaza después de validar el nuevo.")
add("settings_display", "Display", "Display", "显示", "Pantalla")
add("settings_fps", "Contador de FPS", "FPS counter", "FPS 计数器", "Contador de FPS")
add("settings_fps_target", "Taxa de quadros", "Frame rate", "帧率", "Tasa de fotogramas")
add("settings_fps_value", "%1$d FPS", "%1$d FPS", "%1$d FPS", "%1$d FPS")
add("settings_fps_original", "%1$d FPS (original)", "%1$d FPS (original)", "%1$d FPS（原版）", "%1$d FPS (original)")
add("settings_fps_unlimited", "Ilimitado", "Unlimited", "不限制", "Sin límite")
add("settings_fps_target_note", "30 é o ritmo original do jogo. Acima disso o jogo apresenta sem esperar o vblank e é mantido na taxa escolhida; no fim da barra não há limite, nem o da frequência da tela. Muda na hora, e pode alterar o ritmo de partes do jogo.",
    "30 is the game's original pace. Above it the game presents without waiting for the vblank and is held to the chosen rate; the end of the bar has no limit at all, not even the screen's refresh rate. It changes at once, and can change the pacing of parts of the game.",
    "30 为游戏原版节奏。高于 30 时，游戏不再等待垂直消隐并保持在所选帧率；滑块末端完全不限制，也不受屏幕刷新率约束。更改立即生效，可能会改变游戏部分内容的节奏。",
    "30 es el ritmo original del juego. Por encima, el juego presenta sin esperar al vblank y se mantiene en la tasa elegida; el final de la barra no tiene límite, ni siquiera el de la frecuencia de la pantalla. Cambia al instante y puede alterar el ritmo de partes del juego.")
add("settings_controls", "Controles", "Controls", "控制", "Controles")
add("settings_show_controls", "Mostrar controles na tela", "Show on-screen controls", "显示屏幕控制按键",
    "Mostrar controles en pantalla")
add("settings_external", "Controles externos e jogadores", "External controllers and players", "外接手柄与玩家",
    "Mandos externos y jugadores")
add("settings_advanced", "Avançado", "Advanced", "高级", "Avanzado")
add("settings_driver", "Driver", "Driver", "驱动程序", "Controlador")
add("driver_system", "Driver do sistema", "System driver", "系统驱动程序", "Controlador del sistema")
add("driver_bundled_label", "%1$s (incluído)", "%1$s (bundled)", "%1$s（内置）", "%1$s (incluido)")
add("settings_driver_import", "Importar driver Adreno (.zip)", "Import Adreno driver (.zip)", "导入 Adreno 驱动程序 (.zip)",
    "Importar controlador Adreno (.zip)")
add("settings_driver_delete", "Apagar driver selecionado", "Delete selected driver", "删除所选驱动程序",
    "Eliminar el controlador seleccionado")
add("settings_turnip_note", "Opções do Adreno (One UI fix, TU_DEBUG, IR3_SHADER_DEBUG) aparecem ao selecionar um driver Turnip.",
    "Adreno options (One UI fix, TU_DEBUG, IR3_SHADER_DEBUG) appear when a Turnip driver is selected.",
    "选择 Turnip 驱动程序后，将显示 Adreno 选项（One UI fix、TU_DEBUG、IR3_SHADER_DEBUG）。",
    "Las opciones de Adreno (One UI fix, TU_DEBUG, IR3_SHADER_DEBUG) aparecen al seleccionar un controlador Turnip.")
add("settings_clear_shaders", "Apagar cache de shaders", "Clear shader cache", "清除着色器缓存", "Borrar la caché de shaders")
add("settings_runtime", "Opções do runtime", "Runtime options", "运行时选项", "Opciones del runtime")
add("settings_runtime_note", "Aplicadas na próxima inicialização do jogo.", "Applied the next time the game starts.",
    "将在下次启动游戏时生效。", "Se aplican en el próximo inicio del juego.")
add("settings_log_level", "Nível do log", "Log level", "日志级别", "Nivel de registro")
add("settings_async", "Compilação assíncrona de shaders e pipelines", "Asynchronous shader and pipeline compilation",
    "异步编译着色器和管线", "Compilación asíncrona de shaders y pipelines")
add("settings_no_shadows", "Remover sombras", "Remove shadows", "移除阴影", "Quitar sombras")
add("settings_no_bloom", "Remover bloom", "Remove bloom", "移除泛光", "Quitar bloom")
add("settings_adpf", "Desempenho sustentado (ADPF)", "Sustained performance (ADPF)", "持续性能 (ADPF)",
    "Rendimiento sostenido (ADPF)")
add("settings_texture_quality", "Qualidade das texturas", "Texture quality", "纹理质量", "Calidad de texturas")
add("texture_quality_full", "Completa", "Full", "完整", "Completa")
add("texture_quality_half", "Metade", "Half", "一半", "Mitad")
add("texture_quality_quarter", "Um quarto", "Quarter", "四分之一", "Un cuarto")
add("settings_internal_resolution", "Resolução interna", "Internal resolution", "内部分辨率", "Resolución interna")
add("internal_resolution_full", "Completa · 720p", "Full · 720p", "完整 · 720p", "Completa · 720p")
add("internal_resolution_three_quarters", "3/4 · 540p", "3/4 · 540p", "3/4 · 540p", "3/4 · 540p")
add("internal_resolution_half", "Metade · 360p", "Half · 360p", "一半 · 360p", "Mitad · 360p")
add("settings_shadow_quality", "Qualidade das sombras", "Shadow quality", "阴影质量", "Calidad de sombras")
add("shadow_quality_full", "Completa", "Full", "完整", "Completa")
add("shadow_quality_medium", "Média · 3/4", "Medium · 3/4", "中 · 3/4", "Media · 3/4")
add("shadow_quality_low", "Baixa · 1/2", "Low · 1/2", "低 · 1/2", "Baja · 1/2")
# ---- screen layout editor ---------------------------------------------------------------------
add("settings_temp", "Temperatura", "Temperature", "温度", "Temperatura")
add("layout_title", "Editar layout", "Edit layout", "编辑布局", "Editar diseño")
add("layout_edit_button", "Editar layout…", "Edit layout…", "编辑布局…", "Editar diseño…")
add("layout_profile", "Perfil", "Profile", "配置文件", "Perfil")
add("layout_default", "Padrão", "Default", "默认", "Predeterminado")
add("layout_save", "Salvar", "Save", "保存", "Guardar")
add("layout_save_as", "Salvar como…", "Save as…", "另存为…", "Guardar como…")
add("layout_delete", "Excluir", "Delete", "删除", "Eliminar")
add("layout_done", "Concluir", "Done", "完成", "Terminar")
add("layout_reset_item", "Restaurar posição", "Reset position", "恢复位置", "Restablecer posición")
add("layout_size", "Tamanho", "Size", "大小", "Tamaño")
add("layout_visible", "Visível", "Visible", "显示", "Visible")
add("layout_opacity", "Opacidade", "Opacity", "不透明度", "Opacidad")
add("layout_name_hint", "Nome do perfil", "Profile name", "配置文件名称", "Nombre del perfil")
add("layout_save_as_title", "Salvar perfil como", "Save profile as", "将配置文件另存为", "Guardar perfil como")
add("layout_name_taken", "Já existe um perfil com esse nome.", "A profile with that name already exists.",
    "已存在同名配置文件。", "Ya existe un perfil con ese nombre.")
add("layout_delete_title", "Excluir perfil?", "Delete profile?", "删除配置文件？", "¿Eliminar el perfil?")
add("layout_delete_message", "O perfil \"%1$s\" será removido.", "The profile \"%1$s\" will be removed.",
    "配置文件“%1$s”将被删除。", "Se eliminará el perfil «%1$s».")
add("layout_unsaved_title", "Alterações não salvas", "Unsaved changes", "更改尚未保存", "Cambios sin guardar")
add("layout_unsaved_message", "Salvar as mudanças feitas neste layout?", "Save the changes made to this layout?",
    "要保存对此布局所做的更改吗？", "¿Guardar los cambios hechos en este diseño?")
add("layout_discard", "Descartar", "Discard", "放弃", "Descartar")
add("layout_hint", "Arraste um item para movê-lo. Toque nele para ajustar tamanho e visibilidade.",
    "Drag an item to move it. Tap it to adjust its size and visibility.",
    "拖动项目即可移动。点按项目可调整大小和显示。",
    "Arrastra un elemento para moverlo. Tócalo para ajustar su tamaño y visibilidad.")
add("layouts_title", "Layout do touchscreen", "Touchscreen layout", "触摸屏布局", "Diseño de la pantalla táctil")
add("layouts_manage", "Layout do touchscreen", "Touchscreen layout", "触摸屏布局", "Diseño de la pantalla táctil")
add("layouts_new", "Criar novo layout", "Create new layout", "创建新布局", "Crear nuevo diseño")
add("layouts_create", "Criar", "Create", "创建", "Crear")
add("layouts_edit", "Editar", "Edit", "编辑", "Editar")
add("layouts_builtin", "Padrão do app (não editável)", "App default (not editable)", "应用默认布局（不可编辑）", "Predeterminado de la app (no editable)")
add("layouts_active", "Em uso", "In use", "使用中", "En uso")
add("layout_item_stick_l", "Analógico esquerdo", "Left stick", "左摇杆", "Stick izquierdo")
add("layout_item_stick_r", "Analógico direito", "Right stick", "右摇杆", "Stick derecho")
add("layout_item_dpad", "Direcional", "D-pad", "方向键", "Cruceta")
add("layout_item_lt", "Gatilho esquerdo (LT)", "Left trigger (LT)", "左扳机 (LT)", "Gatillo izquierdo (LT)")
add("layout_item_rt", "Gatilho direito (RT)", "Right trigger (RT)", "右扳机 (RT)", "Gatillo derecho (RT)")
add("layout_item_lb", "Botão LB", "LB button", "LB 按钮", "Botón LB")
add("layout_item_rb", "Botão RB", "RB button", "RB 按钮", "Botón RB")
add("layout_item_back", "Botão Back", "Back button", "Back 按钮", "Botón Back")
add("layout_item_start", "Botão Start", "Start button", "Start 按钮", "Botón Start")
add("layout_item_face", "Botão %1$s", "%1$s button", "%1$s 按钮", "Botón %1$s")
add("settings_footer", "reNut · prévia de desenvolvimento", "reNut · development preview", "reNut · 开发预览版",
    "reNut · versión preliminar de desarrollo")
add("driver_delete_title", "Apagar driver?", "Delete driver?", "删除驱动程序？", "¿Eliminar el controlador?")
add("driver_delete_message", "\"%1$s\" será removido do aparelho e o driver do sistema passará a ser usado. Você poderá importá-lo de novo.",
    "\"%1$s\" will be removed from the device and the system driver will be used. You can import it again.",
    "“%1$s”将从设备中移除，并改用系统驱动程序。你可以再次导入。",
    "«%1$s» se quitará del dispositivo y se usará el controlador del sistema. Podrás importarlo de nuevo.")
add("driver_import_done", "Driver importado e validado.", "Driver imported and validated.", "驱动程序已导入并通过验证。",
    "Controlador importado y validado.")
add("driver_import_failed", "Importação não concluída: %1$s", "Import not completed: %1$s", "导入未完成：%1$s",
    "Importación no completada: %1$s")
add("driver_remove_failed", "Driver removido da lista, mas os arquivos não foram apagados: %1$s",
    "Driver removed from the list, but its files were not deleted: %1$s",
    "驱动程序已从列表中移除，但文件未能删除：%1$s",
    "Controlador quitado de la lista, pero no se eliminaron sus archivos: %1$s")
add("driver_importing", "Importando driver…", "Importing driver…", "正在导入驱动程序…", "Importando controlador…")
add("driver_invalid_pack", "Pacote de driver inválido: use um ZIP com arquivos na raiz.",
    "Invalid driver package: use a ZIP with files at its root.", "驱动程序包无效：请使用根目录下包含文件的 ZIP。",
    "Paquete de controlador no válido: usa un ZIP con los archivos en la raíz.")
add("driver_zip_path", "Caminho inválido no ZIP.", "Invalid path in the ZIP.", "ZIP 中的路径无效。", "Ruta no válida en el ZIP.")
add("driver_too_big", "Driver excede 256 MB.", "Driver exceeds 256 MB.", "驱动程序超过 256 MB。", "El controlador supera 256 MB.")
add("driver_meta_big", "Metadados excessivos.", "Metadata is too large.", "元数据过大。", "Metadatos excesivos.")
add("driver_lib_missing", "Biblioteca do driver ausente.", "Driver library is missing.", "缺少驱动程序库。",
    "Falta la biblioteca del controlador.")
add("driver_needs_android", "Driver exige uma versão Android mais recente.", "The driver requires a newer Android version.",
    "该驱动程序需要更新的 Android 版本。", "El controlador requiere una versión de Android más reciente.")
add("driver_not_arm64", "O driver precisa ser uma biblioteca ELF ARM64.", "The driver must be an ARM64 ELF library.",
    "驱动程序必须是 ARM64 ELF 库。", "El controlador debe ser una biblioteca ELF ARM64.")
add("driver_save_failed", "Não foi possível salvar o driver.", "The driver could not be saved.", "无法保存驱动程序。",
    "No se pudo guardar el controlador.")
add("import_cancelled", "Importação cancelada.", "Import cancelled.", "导入已取消。", "Importación cancelada.")
add("shader_cache_title", "Apagar cache de shaders?", "Clear the shader cache?", "清除着色器缓存？", "¿Borrar la caché de shaders?")
add("shader_cache_message", "Remove o cache Vulkan salvo pelo Plume. Os shaders serão compilados novamente na próxima execução; seus jogos e saves serão preservados.",
    "Removes the Vulkan cache saved by Plume. Shaders will be compiled again on the next run; your games and saves are kept.",
    "将删除 Plume 保存的 Vulkan 缓存。下次运行时会重新编译着色器；你的游戏和存档不会受影响。",
    "Elimina la caché de Vulkan guardada por Plume. Los shaders se compilarán de nuevo en la próxima ejecución; tus juegos y partidas se conservan.")
add("shader_cache_cleared", "Cache de shaders apagado. Será recriado na próxima execução.",
    "Shader cache cleared. It will be rebuilt on the next run.", "着色器缓存已清除，将在下次运行时重建。",
    "Caché de shaders borrada. Se recreará en la próxima ejecución.")
add("shader_cache_none", "Nenhum cache de shaders salvo para apagar.", "There is no saved shader cache to clear.",
    "没有可清除的着色器缓存。", "No hay ninguna caché de shaders guardada que borrar.")
add("shader_cache_failed", "Não foi possível apagar o cache de shaders: %1$s", "Could not clear the shader cache: %1$s",
    "无法清除着色器缓存：%1$s", "No se pudo borrar la caché de shaders: %1$s")
add("toml_save_failed", "Não foi possível salvar renut.toml: %1$s", "Could not save renut.toml: %1$s",
    "无法保存 renut.toml：%1$s", "No se pudo guardar renut.toml: %1$s")

# ---- in-game menu ---------------------------------------------------------------------------------
add("menu_open", "Abrir menu", "Open menu", "打开菜单", "Abrir menú")
add("menu_close", "Fechar menu", "Close menu", "关闭菜单", "Cerrar menú")
add("menu_back", "Voltar ao menu", "Back to menu", "返回菜单", "Volver al menú")
add("menu_paused", "Pausado", "Paused", "已暂停", "En pausa")
add("menu_resume", "Retomar", "Resume", "继续", "Reanudar")
add("menu_eyebrow", "RENUT / JOGO", "RENUT / GAME", "RENUT / 游戏", "RENUT / JUEGO")
add("menu_title", "Menu do jogo", "Game menu", "游戏菜单", "Menú del juego")
add("menu_continue", "Continuar", "Continue", "继续游戏", "Continuar")
add("menu_resume_game", "Retomar jogo", "Resume game", "继续游戏", "Reanudar juego")
add("menu_paused_hint", "O jogo está pausado", "The game is paused", "游戏已暂停", "El juego está en pausa")
add("menu_pause_game", "Pausar jogo", "Pause game", "暂停游戏", "Pausar juego")
add("menu_pause_hint", "Congela o jogo até você retomar", "Freezes the game until you resume",
    "冻结游戏，直到你继续", "Congela el juego hasta que lo reanudes")
add("menu_ach_hint", "Ver a lista do jogo", "See the game's list", "查看游戏的成就列表", "Ver la lista del juego")
add("menu_ach_summary", "%1$d de %2$d · %3$d G", "%1$d of %2$d · %3$d G", "%1$d / %2$d · %3$d G", "%1$d de %2$d · %3$d G")
add("menu_onscreen_controls", "Controles na tela", "On-screen controls", "屏幕控制按键", "Controles en pantalla")
add("menu_controllers", "Controles e jogadores", "Controllers and players", "手柄与玩家", "Mandos y jugadores")
add("menu_controllers_count", "%1$d conectado(s)", "%1$d connected", "已连接 %1$d 个", "%1$d conectado(s)")
add("menu_controllers_two", "2 jogadores", "2 players", "双人模式", "2 jugadores")
add("menu_exit", "Sair do jogo", "Exit game", "退出游戏", "Salir del juego")
add("menu_ach_none", "A lista é lida do seu jogo na primeira vez em que ele inicia. Ela aparece aqui depois dessa primeira partida.",
    "The list is read from your game the first time it starts. It appears here after that first session.",
    "该列表会在游戏首次启动时从游戏中读取，首局结束后会显示在这里。",
    "La lista se lee de tu juego la primera vez que arranca. Aparece aquí después de esa primera partida.")

# ---- external controller --------------------------------------------------------------------------
add("ctrl_title", "Controles externos", "External controllers", "外接手柄", "Mandos externos")
add("ctrl_intro", "Conecte um controle USB ou Bluetooth. O mapeamento é salvo para cada aparelho.",
    "Connect a USB or Bluetooth controller. The mapping is saved for each device.",
    "连接 USB 或蓝牙手柄。按键映射会为每个设备分别保存。",
    "Conecta un mando USB o Bluetooth. La asignación se guarda para cada dispositivo.")
add("ctrl_panel_device", "Dispositivo", "Device", "设备", "Dispositivo")
add("ctrl_none", "Nenhum controle conectado", "No controller connected", "未连接手柄", "Ningún mando conectado")
add("ctrl_refresh", "Atualizar lista de controles", "Refresh controller list", "刷新手柄列表", "Actualizar lista de mandos")
add("ctrl_panel_mapping", "Mapeamento", "Mapping", "按键映射", "Asignación")
add("ctrl_auto", "Mapear automaticamente", "Map automatically", "自动映射", "Asignar automáticamente")
add("ctrl_cancel_capture", "Cancelar captura", "Cancel capture", "取消捕获", "Cancelar captura")
add("ctrl_choose_title", "Selecionar controle", "Select controller", "选择手柄", "Seleccionar mando")
add("ctrl_status_default", "Mapeamento padrão do Android aplicado.", "Android's default mapping applied.",
    "已应用 Android 默认映射。", "Se aplicó la asignación predeterminada de Android.")
add("ctrl_status_select", "Selecione um comando abaixo para mapear manualmente.", "Select a control below to map it manually.",
    "请在下方选择一个按键进行手动映射。", "Selecciona un control abajo para asignarlo manualmente.")
add("ctrl_status_loaded", "Mapeamento carregado. Toque em um comando para alterar manualmente.",
    "Mapping loaded. Tap a control to change it manually.", "已加载映射。点按某个按键即可手动更改。",
    "Asignación cargada. Toca un control para cambiarlo manualmente.")
add("ctrl_status_connect", "Conecte um controle e toque em Atualizar lista de controles.",
    "Connect a controller and tap Refresh controller list.", "请连接手柄，然后点按“刷新手柄列表”。",
    "Conecta un mando y toca Actualizar lista de mandos.")
add("ctrl_press", "Pressione %1$s no controle.", "Press %1$s on the controller.", "请在手柄上按下 %1$s。",
    "Pulsa %1$s en el mando.")
add("ctrl_move", "Mova %1$s no controle.", "Move %1$s on the controller.", "请在手柄上移动 %1$s。",
    "Mueve %1$s en el mando.")
add("ctrl_assigned", "%1$s atribuído a %2$s.", "%1$s assigned to %2$s.", "%1$s 已分配给 %2$s。", "%1$s asignado a %2$s.")
add("ctrl_need_axis", "Mova o analógico; esta função precisa de um eixo.", "Move the stick; this function needs an axis.",
    "请移动摇杆；此功能需要使用摇杆轴。", "Mueve el stick; esta función necesita un eje.")
add("ctrl_unassigned", "Não atribuído", "Unassigned", "未分配", "Sin asignar")
add("ctrl_axis", "Eixo %1$s", "Axis %1$s", "轴 %1$s", "Eje %1$s")
add("ctrl_players_title", "Controles conectados e jogadores", "Connected controllers and players",
    "已连接的手柄与玩家", "Mandos conectados y jugadores")
add("ctrl_players_none", "Nenhum controle conectado. Conecte um controle USB ou Bluetooth; ele aparece aqui na hora.",
    "No controller connected. Plug in a USB or Bluetooth controller; it shows up here right away.",
    "未连接手柄。请连接 USB 或蓝牙手柄，它会立即显示在这里。",
    "Ningún mando conectado. Conecta un mando USB o Bluetooth; aparecerá aquí al instante.")
add("ctrl_player_one", "Jogador 1", "Player 1", "玩家 1", "Jugador 1")
add("ctrl_player_two", "Jogador 2", "Player 2", "玩家 2", "Jugador 2")
add("ctrl_player_off", "Não usar", "Don't use", "不使用", "No usar")
add("ctrl_players_two_active", "Modo de 2 jogadores ativo: o jogo reconhece o Jogador 2.",
    "2-player mode is on: the game recognizes Player 2.",
    "双人模式已开启：游戏已识别玩家 2。", "Modo de 2 jugadores activo: el juego reconoce al Jugador 2.")
add("ctrl_players_hint_one", "Conecte um segundo controle (ou escolha Jogador 2 em um controle) para jogar em dois.",
    "Connect a second controller (or set one to Player 2) to play with two.",
    "连接第二个手柄（或将某个手柄设为玩家 2）即可双人游戏。",
    "Conecta un segundo mando (o pon uno como Jugador 2) para jugar de a dos.")
add("ctrl_players_touch", "A tela sensível ao toque sempre controla o Jogador 1.",
    "The touch screen always controls Player 1.", "触屏始终控制玩家 1。",
    "La pantalla táctil siempre controla al Jugador 1.")
add("ctrl_rumble", "Vibração", "Vibration", "震动", "Vibración")
add("ctrl_rumble_note", "Cada controle vibra com o jogador dele. Se o controle não tem motor de vibração, ou você joga só na tela, o celular vibra no lugar.",
    "Each controller rumbles for its own player. If a controller has no rumble motor, or you play on the touch screen alone, the phone vibrates instead.",
    "每个手柄为各自的玩家震动。如果手柄没有震动马达，或只使用触屏，则由手机震动代替。",
    "Cada mando vibra para su jugador. Si el mando no tiene motor de vibración, o juegas solo en la pantalla, vibra el teléfono.")
add("ctrl_rumble_test", "Testar vibração", "Test vibration", "测试震动", "Probar vibración")

# ---- diagnostic screens ---------------------------------------------------------------------------
add("result_panel", "Resultado", "Result", "结果", "Resultado")
add("runtime_eyebrow", "TESTE DO RUNTIME", "RUNTIME TEST", "运行时测试", "PRUEBA DEL RUNTIME")
add("runtime_subtitle", "Validação experimental", "Experimental validation", "实验性验证", "Validación experimental")
add("runtime_graphics_status", "RexGlue + Plume\nImagem de teste na memória do runtime. Ainda não representa cenas do jogo.",
    "RexGlue + Plume\nTest image in the runtime's memory. It does not represent game scenes yet.",
    "RexGlue + Plume\n运行时内存中的测试图像，尚不代表游戏场景。",
    "RexGlue + Plume\nImagen de prueba en la memoria del runtime. Aún no representa escenas del juego.")
add("runtime_validating", "Validando o runtime ARM64…\nEste teste ainda não inicia o jogo.",
    "Validating the ARM64 runtime…\nThis test does not start the game yet.", "正在验证 ARM64 运行时…\n此测试尚不会启动游戏。",
    "Validando el runtime ARM64…\nEsta prueba aún no inicia el juego.")
add("runtime_start_failed", "Falha ao iniciar o runtime: %1$s", "Failed to start the runtime: %1$s", "运行时启动失败：%1$s",
    "Error al iniciar el runtime: %1$s")
add("file_unavailable", "Arquivo indisponível.", "File unavailable.", "文件不可用。", "Archivo no disponible.")
add("precompile_title", "Preparando shaders", "Preparing shaders", "正在准备着色器", "Preparando shaders")
add("precompile_detail", "%1$d de %2$d pipelines · %3$d%%", "%1$d of %2$d pipelines · %3$d%%", "%1$d / %2$d 条管线 · %3$d%%",
    "%1$d de %2$d pipelines · %3$d%%")

# ---- text input box ---------------------------------------------------------------------------------
add("textbox_default_title", "Digite o texto", "Type the text", "请输入文字", "Escribe el texto")

# ---- notification -----------------------------------------------------------------------------------
add("service_channel", "Jogo em execução", "Game running", "游戏运行中", "Juego en ejecución")
add("service_text", "O jogo está aberto. Toque para voltar.", "The game is open. Tap to go back.",
    "游戏正在运行。点按即可返回。", "El juego está abierto. Toca para volver.")

# ---- welcome ----------------------------------------------------------------------------------------
add("welcome_intro_eyebrow", "RENUT / BEM-VINDO", "RENUT / WELCOME", "RENUT / 欢迎", "RENUT / BIENVENIDO")
add("welcome_intro_title", "Renut Android", "Renut Android", "Renut Android", "Renut Android")
add("welcome_intro_subtitle", "Um projeto independente e experimental.", "An independent, experimental project.",
    "一个独立的实验性项目。", "Un proyecto independiente y experimental.")
add("welcome_intro_body1", "O reNut leva Banjo-Kazooie: Nuts & Bolts para o Android. O código do jogo é recompilado para ARM64 e os gráficos rodam em Vulkan, direto no seu aparelho.",
    "reNut brings Banjo-Kazooie: Nuts & Bolts to Android. The game's code is recompiled to ARM64 and the graphics run on Vulkan, right on your device.",
    "reNut 将《Banjo-Kazooie: Nuts & Bolts》带到 Android。游戏代码被重新编译为 ARM64，图形通过 Vulkan 直接在你的设备上运行。",
    "reNut lleva Banjo-Kazooie: Nuts & Bolts a Android. El código del juego se recompila a ARM64 y los gráficos funcionan con Vulkan, directamente en tu dispositivo.")
add("welcome_intro_body2", "É uma prévia de desenvolvimento: o desempenho e a compatibilidade ainda estão sendo trabalhados.",
    "This is a development preview: performance and compatibility are still being worked on.",
    "这是开发预览版：性能和兼容性仍在持续改进。",
    "Es una versión preliminar de desarrollo: el rendimiento y la compatibilidad aún se están trabajando.")
add("welcome_continue", "Continuar", "Continue", "继续", "Continuar")
add("notice_eyebrow", "RENUT / ANTES DE COMEÇAR", "RENUT / BEFORE YOU START", "RENUT / 开始之前", "RENUT / ANTES DE EMPEZAR")
add("notice_title", "Importe o seu jogo", "Import your game", "导入你的游戏", "Importa tu juego")
add("notice_subtitle", "O reNut não inclui o jogo e não baixa nada.", "reNut does not include the game and downloads nothing.",
    "reNut 不包含游戏本体，也不会下载任何内容。", "reNut no incluye el juego ni descarga nada.")
add("notice_legal", "Use apenas uma cópia do jogo que você possui.", "Only use a copy of the game that you own.",
    "请仅使用你自己拥有的游戏副本。", "Usa únicamente una copia del juego de tu propiedad.")
add("notice_iso_title", "Imagem ISO (recomendado)", "ISO image (recommended)", "ISO 镜像（推荐）", "Imagen ISO (recomendado)")
add("notice_iso_body", "Traz todos os arquivos do jogo. Precisa de espaço livre para a imagem copiada e para os arquivos extraídos.",
    "Brings every game file. Needs free space for the copied image and for the extracted files.",
    "包含游戏的全部文件。需要有足够的可用空间来存放复制的镜像和解压出的文件。",
    "Incluye todos los archivos del juego. Necesita espacio libre para la imagen copiada y para los archivos extraídos.")
add("notice_xex_title", "Apenas o XEX", "XEX only", "仅 XEX", "Solo el XEX")
add("notice_xex_body", "Só o executável (default.xex). Serve para testes: o jogo não inicia sem os arquivos da ISO.",
    "Just the executable (default.xex). Useful for tests: the game does not start without the ISO's files.",
    "仅可执行文件（default.xex）。可用于测试：没有 ISO 中的文件，游戏无法启动。",
    "Solo el ejecutable (default.xex). Sirve para pruebas: el juego no arranca sin los archivos de la ISO.")
add("notice_time", "A importação leva alguns minutos. Mantenha o app aberto até terminar.",
    "Importing takes a few minutes. Keep the app open until it finishes.",
    "导入需要几分钟。请在完成前保持应用打开。", "La importación tarda unos minutos. Mantén la aplicación abierta hasta que termine.")
add("btn_import_iso", "Importar imagem ISO", "Import ISO image", "导入 ISO 镜像", "Importar imagen ISO")
add("btn_import_xex", "Importar apenas o XEX", "Import XEX only", "仅导入 XEX", "Importar solo el XEX")
add("install_eyebrow", "RENUT / INSTALAÇÃO", "RENUT / INSTALLATION", "RENUT / 安装", "RENUT / INSTALACIÓN")
add("install_title", "Instalando o jogo", "Installing the game", "正在安装游戏", "Instalando el juego")
add("install_sub_disc", "Copiando a imagem e extraindo os arquivos.", "Copying the image and extracting the files.",
    "正在复制镜像并解压文件。", "Copiando la imagen y extrayendo los archivos.")
add("install_sub_xex", "Copiando o executável.", "Copying the executable.", "正在复制可执行文件。", "Copiando el ejecutable.")
add("install_dont_close", "Não feche o app durante a instalação.", "Do not close the app during installation.",
    "安装期间请勿关闭应用。", "No cierres la aplicación durante la instalación.")
add("done_eyebrow", "RENUT / PRONTO", "RENUT / DONE", "RENUT / 完成", "RENUT / LISTO")
add("done_title", "Tudo pronto", "All set", "一切就绪", "Todo listo")
add("done_subtitle", "O jogo foi importado e validado.", "The game was imported and validated.", "游戏已导入并通过验证。",
    "El juego se importó y se validó.")
add("done_body_disc", "Banjo-Kazooie: Nuts & Bolts está instalado e pronto para jogar.",
    "Banjo-Kazooie: Nuts & Bolts is installed and ready to play.", "《Banjo-Kazooie: Nuts & Bolts》已安装，可以开始游戏。",
    "Banjo-Kazooie: Nuts & Bolts está instalado y listo para jugar.")
add("done_body_xex", "O executável foi importado. Para jogar, importe também a imagem ISO completa em Configurações.",
    "The executable was imported. To play, also import the full ISO image in Settings.",
    "可执行文件已导入。要开始游戏，还需要在“设置”中导入完整的 ISO 镜像。",
    "Se importó el ejecutable. Para jugar, importa también la imagen ISO completa en Ajustes.")
add("btn_start", "Começar", "Get started", "开始使用", "Empezar")
add("fail_eyebrow", "RENUT / IMPORTAÇÃO", "RENUT / IMPORT", "RENUT / 导入", "RENUT / IMPORTACIÓN")
add("fail_title", "Não foi possível importar", "Could not import", "无法导入", "No se pudo importar")
add("fail_subtitle", "Nada foi alterado no jogo anterior.", "The previous game was not changed.", "之前的游戏未作任何更改。",
    "No se cambió nada en el juego anterior.")
add("fail_unknown", "Erro desconhecido.", "Unknown error.", "未知错误。", "Error desconocido.")
add("btn_retry", "Tentar novamente", "Try again", "重试", "Intentar de nuevo")

# ---- importer messages --------------------------------------------------------------------------------
add("imp_preparing", "Preparando a importação…", "Preparing the import…", "正在准备导入…", "Preparando la importación…")
add("imp_copy_image", "Copiando a imagem para o aparelho…", "Copying the image to the device…", "正在将镜像复制到设备…",
    "Copiando la imagen al dispositivo…")
add("imp_copy_image_mb", "Copiando a imagem · %1$d MB", "Copying the image · %1$d MB", "正在复制镜像 · %1$d MB",
    "Copiando la imagen · %1$d MB")
add("imp_extract", "Extraindo os arquivos do jogo…", "Extracting the game files…", "正在解压游戏文件…",
    "Extrayendo los archivos del juego…")
add("imp_copy_xex", "Copiando o executável…", "Copying the executable…", "正在复制可执行文件…", "Copiando el ejecutable…")
add("imp_verify", "Verificando o executável…", "Verifying the executable…", "正在验证可执行文件…", "Verificando el ejecutable…")
add("imp_no_xex", "Esta imagem não tem o executável do jogo (default.xex).", "This image does not contain the game's executable (default.xex).",
    "此镜像不包含游戏的可执行文件（default.xex）。", "Esta imagen no contiene el ejecutable del juego (default.xex).")
add("imp_xex_big", "XEX excessivamente grande.", "The XEX is excessively large.", "XEX 文件过大。", "El XEX es excesivamente grande.")
add("imp_stage_fail", "Não foi possível criar a pasta de importação.", "Could not create the import folder.",
    "无法创建导入文件夹。", "No se pudo crear la carpeta de importación.")
add("imp_data_fail", "Não foi possível criar a pasta de dados.", "Could not create the data folder.",
    "无法创建数据文件夹。", "No se pudo crear la carpeta de datos.")
add("imp_marker_fail", "Falha ao registrar importação.", "Failed to register the import.", "无法记录此次导入。",
    "Error al registrar la importación.")
add("imp_xex_missing", "XEX ausente.", "XEX is missing.", "缺少 XEX。", "Falta el XEX.")
add("imp_unreadable", "Arquivo inacessível.", "File is not accessible.", "无法访问该文件。", "Archivo inaccesible.")
add("imp_limit", "Arquivo excede o limite de importação.", "The file exceeds the import limit.", "文件超出导入大小限制。",
    "El archivo supera el límite de importación.")
add("imp_no_space", "Espaço livre insuficiente.", "Not enough free space.", "可用空间不足。", "Espacio libre insuficiente.")
add("incompatible_xex", "Este executável não é compatível com o reNut. O jogo anterior foi preservado.",
    "This executable is not compatible with reNut. The previous game was preserved.",
    "此可执行文件与 reNut 不兼容。之前的游戏已保留。",
    "Este ejecutable no es compatible con reNut. Se conservó el juego anterior.")
add("xex_size_invalid", "Tamanho XEX inválido.", "Invalid XEX size.", "XEX 大小无效。", "Tamaño de XEX no válido.")
add("xex_no_header", "O executável não tem cabeçalho XEX2.", "The executable has no XEX2 header.", "可执行文件没有 XEX2 头。",
    "El ejecutable no tiene cabecera XEX2.")
add("temp_unreachable", "Não foi possível acessar a pasta temporária.", "Could not access the temporary folder.",
    "无法访问临时文件夹。", "No se pudo acceder a la carpeta temporal.")
add("temp_remove", "Não foi possível remover arquivo temporário.", "Could not remove a temporary file.",
    "无法删除临时文件。", "No se pudo eliminar un archivo temporal.")

add("disc_unknown", "Imagem XDVDFS não reconhecida.", "XDVDFS image not recognized.", "无法识别 XDVDFS 镜像。",
    "Imagen XDVDFS no reconocida.")
add("disc_no_default", "A imagem não contém default.xex na raiz.", "The image has no default.xex at its root.",
    "镜像根目录中没有 default.xex。", "La imagen no contiene default.xex en la raíz.")
add("disc_truncated", "Imagem truncada ou referência fora do arquivo.", "Truncated image or reference outside the file.",
    "镜像已被截断，或引用超出了文件范围。", "Imagen truncada o referencia fuera del archivo.")
add("disc_dir_cyclic", "Diretório inválido ou cíclico.", "Invalid or cyclic directory.", "目录无效或存在循环。",
    "Directorio no válido o cíclico.")
add("disc_meta_limit", "Metadados de diretórios excedem o limite.", "Directory metadata exceeds the limit.",
    "目录元数据超出限制。", "Los metadatos de directorios superan el límite.")
add("disc_tree_invalid", "Árvore de diretório inválida.", "Invalid directory tree.", "目录树无效。", "Árbol de directorios no válido.")
add("disc_name_invalid", "Nome inválido na imagem.", "Invalid name in the image.", "镜像中存在无效名称。",
    "Nombre no válido en la imagen.")
add("disc_path_invalid", "Caminho inválido na imagem.", "Invalid path in the image.", "镜像中存在无效路径。",
    "Ruta no válida en la imagen.")
add("disc_name_control", "Nome contém controle.", "A name contains control characters.", "名称中包含控制字符。",
    "Un nombre contiene caracteres de control.")
add("disc_name_dup", "Nome duplicado na imagem.", "Duplicate name in the image.", "镜像中存在重复的名称。",
    "Nombre duplicado en la imagen.")
add("disc_size_exceeds", "Tamanho extraído excede a imagem.", "Extracted size exceeds the image.", "解压后的大小超出了镜像。",
    "El tamaño extraído supera la imagen.")
add("disc_dest_missing", "Pasta de destino ausente.", "Destination folder is missing.", "缺少目标文件夹。",
    "Falta la carpeta de destino.")
add("disc_no_space", "Espaço insuficiente para extrair o jogo.", "Not enough space to extract the game.",
    "空间不足，无法解压游戏。", "Espacio insuficiente para extraer el juego.")
add("disc_outside", "Caminho fora da pasta de importação.", "Path outside the import folder.", "路径位于导入文件夹之外。",
    "Ruta fuera de la carpeta de importación.")
add("disc_mkdir", "Falha ao criar pasta.", "Failed to create a folder.", "创建文件夹失败。", "Error al crear la carpeta.")
add("disc_exists", "Arquivo já existe no destino.", "The file already exists at the destination.", "目标位置已存在该文件。",
    "El archivo ya existe en el destino.")

add("save_folder", "Pasta de saves inacessível.", "Saves folder is not accessible.", "无法访问存档文件夹。",
    "La carpeta de partidas es inaccesible.")
add("save_too_many", "Há arquivos demais para exportar.", "There are too many files to export.", "要导出的文件过多。",
    "Hay demasiados archivos para exportar.")
add("save_none", "Nenhum save encontrado.", "No saves found.", "未找到存档。", "No se encontraron partidas.")
add("save_size", "Saves excedem 512 MB.", "Saves exceed 512 MB.", "存档超过 512 MB。", "Las partidas superan 512 MB.")
add("save_prepare", "Não foi possível preparar a importação.", "Could not prepare the import.", "无法准备导入。",
    "No se pudo preparar la importación.")
add("save_zip_path", "ZIP contém caminho inválido ou arquivos demais.", "The ZIP has an invalid path or too many files.",
    "ZIP 中包含无效路径或文件过多。", "El ZIP contiene una ruta no válida o demasiados archivos.")
add("save_zip_path2", "Caminho inválido no ZIP.", "Invalid path in the ZIP.", "ZIP 中的路径无效。", "Ruta no válida en el ZIP.")
add("save_mkdir", "Falha ao criar pasta de save.", "Failed to create a save folder.", "创建存档文件夹失败。",
    "Error al crear la carpeta de partida.")
add("save_zip_size", "ZIP de saves excede 512 MB.", "The saves ZIP exceeds 512 MB.", "存档 ZIP 超过 512 MB。",
    "El ZIP de partidas supera 512 MB.")
add("save_zip_empty", "ZIP não contém saves compatíveis.", "The ZIP has no compatible saves.", "ZIP 中没有兼容的存档。",
    "El ZIP no contiene partidas compatibles.")
add("save_dest", "Falha ao criar pasta de destino.", "Failed to create the destination folder.", "创建目标文件夹失败。",
    "Error al crear la carpeta de destino.")
add("unavailable_dest", "Destino indisponível.", "Destination unavailable.", "目标不可用。", "Destino no disponible.")

# controller labels, in ExternalController.LABELS order
CONTROLLER_LABELS = [
    ("Direcional cima", "D-pad up", "方向键 上", "Cruceta arriba"),
    ("Direcional baixo", "D-pad down", "方向键 下", "Cruceta abajo"),
    ("Direcional esquerda", "D-pad left", "方向键 左", "Cruceta izquierda"),
    ("Direcional direita", "D-pad right", "方向键 右", "Cruceta derecha"),
    ("Start", "Start", "Start", "Start"),
    ("Back", "Back", "Back", "Back"),
    ("L3", "L3", "L3", "L3"),
    ("R3", "R3", "R3", "R3"),
    ("LB", "LB", "LB", "LB"),
    ("RB", "RB", "RB", "RB"),
    ("A", "A", "A", "A"),
    ("B", "B", "B", "B"),
    ("X", "X", "X", "X"),
    ("Y", "Y", "Y", "Y"),
    ("Gatilho esquerdo", "Left trigger", "左扳机", "Gatillo izquierdo"),
    ("Gatilho direito", "Right trigger", "右扳机", "Gatillo derecho"),
    ("Analógico esquerdo →", "Left stick →", "左摇杆 →", "Stick izquierdo →"),
    ("Analógico esquerdo ↑", "Left stick ↑", "左摇杆 ↑", "Stick izquierdo ↑"),
    ("Analógico direito →", "Right stick →", "右摇杆 →", "Stick derecho →"),
    ("Analógico direito ↑", "Right stick ↑", "右摇杆 ↑", "Stick derecho ↑"),
]

# language names are shown in their own language everywhere
LANGUAGE_NAMES = [
    ("pt-BR", "Português (Brasil)"),
    ("en-US", "English (US)"),
    ("zh-CN", "中文（普通话）"),
    ("es", "Español"),
]
